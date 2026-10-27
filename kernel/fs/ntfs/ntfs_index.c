/* NTFS write side: folder indexes (the B-tree): open, search, insert, remove. */
#include "ntfs_internal.h"

/* ---- folder indexes: the B-tree ------------------------------------------------ */
/* A folder's index is a B-tree of $FILE_NAME entries. Its root lives inside the folder's MFT record ($INDEX_ROOT),
 * the other nodes are 4 KiB blocks ($INDEX_ALLOCATION, used ones marked in the index $BITMAP). A node is a list
 * of entries ending with an "end" entry. In a node that is not a leaf every entry (also the end entry) points to a
 * child node that holds the names smaller than that entry's name. The rules this code keeps:
 *   - the entries of a node are sorted (collation: upper case via $UpCase) and a name appears once in the tree;
 *   - every block that is reachable is marked used in the bitmap, every marked block is reachable;
 *   - all leaves are at the same depth;
 *   - a block never holds zero entries: such a block is freed (its parent's separator moves down into a sibling).
 * The folder's record is edited in DREC and written by idx_close(). */
Idx IX;
int IDX_WROTE; /* something of the index reached the disk (a block, or the folder's record) during this change */
Lvl PATH[MAXD + 2];
int PDEPTH;

u8 NB1[16384], NB2[16384], ISEAL[16384];
static u8 SPL[16384 + 1400];
static u8 DSNAP[4096];
static u8 SENT[640], DEMOTE[640], EWORK[640];
NtRuns CAND, NEWR;

static u32 e_len(const u8 *e) { return rd16(e + 8); }
static int e_end(const u8 *e) { return (rd16(e + 12) & 2) != 0; }
static int e_sub(const u8 *e) { return (rd16(e + 12) & 1) != 0; }
static u64 e_ptr(const u8 *e) { return rd64(e + e_len(e) - 8); }
static void e_setptr(u8 *e, u64 v) { wr64(e + e_len(e) - 8, v); }

u32 nd_ent0(const Node *n) { return n->hdr + rd32(n->buf + n->hdr); }
static u32 nd_end(const Node *n) { return n->hdr + rd32(n->buf + n->hdr + 4); }
static void nd_set_end(Node *n, u32 end) { wr32(n->buf + n->hdr + 4, end - n->hdr); }
static int nd_leaf(const Node *n) { return !(n->buf[n->hdr + 12] & 1); }
static int nd_empty(const Node *n) { return e_end(n->buf + nd_ent0(n)); }

static u32 vcn_unit(const Ntfs *nt) { return nt->csize <= nt->idx_size ? nt->csize : 512; }
static u64 blk_vcn(const Ntfs *nt, u64 n) { return n * nt->idx_size / vcn_unit(nt); }
static u64 vcn_blk(const Ntfs *nt, u64 v) { return v * vcn_unit(nt) / nt->idx_size; }

static int nd_check(const Node *n, u32 limit)
{
    u32 e0 = nd_ent0(n), end = nd_end(n);
    if (e0 < n->hdr + 16 || e0 > end || end > limit)
        return -1;
    int leaf = nd_leaf(n);
    for (u32 p = e0;;)
    {
        if (p + 16 > end)
            return -1;
        const u8 *e = n->buf + p;
        u32 len = e_len(e);
        if (len < 16 || len > 640 /* the biggest entry we make is 600; the buffers hold 640 */ || (len & 7) || p + len > end ||
            ((!leaf) != (e_sub(e) != 0)) || (e_sub(e) && len < 24))
            return -1;
        if (e_end(e))
            return p + len == end ? 0 : -1;
        u32 kl = rd16(e + 10);
        if (kl < 66 || 16 + kl + (e_sub(e) ? 8u : 0u) > len)
            return -1;
        p += len;
    }
}

/* ---- open / close a folder ------------------------------------------------ */
int idx_open(Ntfs *nt, u32 recno)
{
    u32 rs = nt->rec_size;
    if (record_read(nt, recno, DREC))
        return NTFS_ERR_IO;
    if (!(rd16(DREC + 22) & 2))
        return NTFS_ERR_ARG;
    if (rec_find(DREC, rs, AT_ATTRIBUTE_LIST, 0))
        return unsup(__LINE__);
    u8 *root = rec_find(DREC, rs, AT_INDEX_ROOT, "$I30");
    if (!root || root[8] != 0 || rd32(root + 16) < 32 || rd16(root + 20) + rd32(root + 16) > rd32(root + 4))
        return NTFS_ERR_IO;
    const u8 *v = root + rd16(root + 20);
    if (rd32(v) != AT_FILE_NAME || rd32(v + 4) != 1 || rd32(v + 8) != nt->idx_size)
        return unsup(__LINE__);
    IX.rec = recno;
    IX.dirty = 0;
    IX.has_ia = 0;
    IX.ia.n = 0;
    IX.ia_size = 0;
    IX.ib_res = 1;
    IX.ib.n = 0;
    IX.ib_size = 0;
    u8 *ia = rec_find(DREC, rs, AT_INDEX_ALLOCATION, "$I30");
    u8 *ib = rec_find(DREC, rs, AT_BITMAP, "$I30");
    if (ia)
    {
        if (ia[8] == 0 || rd16(ia + 12) || parse_runs(ia + rd16(ia + 32), ia + rd32(ia + 4), rd64(ia + 16), &IX.ia))
            return unsup(__LINE__);
        IX.ia_size = rd64(ia + 48);
        IX.has_ia = 1;
        if (IX.ia_size % nt->idx_size || IX.ia_size > runs_clusters(&IX.ia) * nt->csize)
            return NTFS_ERR_IO;
        if (!ib)
            return NTFS_ERR_IO;
    }
    if (ib)
    {
        if (ib[8] == 0)
            IX.ib_res = 1;
        else
        {
            IX.ib_res = 0;
            if (rd16(ib + 12) || parse_runs(ib + rd16(ib + 32), ib + rd32(ib + 4), rd64(ib + 16), &IX.ib))
                return unsup(__LINE__);
            IX.ib_size = rd64(ib + 48);
        }
    }
    if (!IX.has_ia && (v[28] & 1))
        return NTFS_ERR_IO;
    return 0;
}

/* write the folder's record if the index changed it */
int idx_close(Ntfs *nt, int ok, u64 touch)
{
    if (!ok)
        return 0;
    if (touch) /* the folder's times change with its names, also when only a block of its index did */
    {
        touch_si(DREC, nt->rec_size, touch);
        IX.dirty = 1;
    }
    if (!IX.dirty)
        return 0;
    IX.dirty = 0;
    IDX_WROTE = 1;
    return rec_store(nt, IX.rec, DREC) ? NTFS_ERR_IO : 0;
}

/* ---- the index bitmap ----------------------------------------------------- */
static int ib_get(Ntfs *nt, u64 n)
{
    if (IX.ib_res)
    {
        u8 *b = rec_find(DREC, nt->rec_size, AT_BITMAP, "$I30");
        if (!b || n / 8 >= rd32(b + 16))
            return 0;
        return (b[rd16(b + 20) + n / 8] >> (n & 7)) & 1;
    }
    u8 x;
    if (n / 8 >= IX.ib_size)
        return 0;
    if (runs_read(nt, &IX.ib, n / 8, &x, 1))
        return -1;
    return (x >> (n & 7)) & 1;
}

static int ib_set(Ntfs *nt, u64 n, int val)
{
    if (IX.ib_res)
    {
        u8 *b = rec_find(DREC, nt->rec_size, AT_BITMAP, "$I30");
        if (!b || n / 8 >= rd32(b + 16))
            return -1;
        u8 *p = b + rd16(b + 20) + n / 8;
        if (val)
            *p |= (u8)(1u << (n & 7));
        else
            *p &= (u8)~(1u << (n & 7));
        IX.dirty = 1;
        return 0;
    }
    u8 x;
    if (n / 8 >= IX.ib_size || runs_read(nt, &IX.ib, n / 8, &x, 1))
        return -1;
    if (val)
        x |= (u8)(1u << (n & 7));
    else
        x &= (u8)~(1u << (n & 7));
    if (runs_write(nt, &IX.ib, n / 8, &x, 1))
    {
        nt->poisoned = 1;
        return -1;
    }
    return 0;
}

/* Take a block of the allocation (growing it by one block when all are used; making the allocation when the folder
 * has none). The record edits are all-or-nothing: on failure DREC is put back and the clusters are given back. */
static int idx_new_block_mem(Ntfs *nt, u64 *out)
{
    u32 rs = nt->rec_size;
    if (nt->csize > nt->idx_size)
        return unsup(__LINE__);
    u64 nblocks = IX.has_ia ? IX.ia_size / nt->idx_size : 0;
    for (u64 n = 0; n < nblocks; n++)
    {
        int b = ib_get(nt, n);
        if (b < 0)
            return NTFS_ERR_IO;
        if (!b)
        {
            if (ib_set(nt, n, 1))
                return NTFS_ERR_IO;
            *out = n;
            return 0;
        }
    }
    u32 ncl = nt->idx_size / nt->csize;
    u64 need_bytes = nblocks / 8 + 1;
    if (!IX.ib_res && need_bytes > IX.ib_size)
        return unsup(__LINE__); /* a non-resident bitmap would have to grow */
    NEWR.n = 0;
    u64 hint = (IX.has_ia && IX.ia.n && IX.ia.r[IX.ia.n - 1].lcn >= 0) ? (u64)IX.ia.r[IX.ia.n - 1].lcn + IX.ia.r[IX.ia.n - 1].len : nt->hint;
    int r = alloc_clusters(nt, ncl, hint, &NEWR);
    if (r)
        return r;
    copy(DSNAP, DREC, rs);
    int ok = 1;
    u64 bytes_after = (nblocks + 1) * nt->idx_size;
    u8 tmp[512];
    CAND.n = 0;
    if (IX.has_ia)
        copy(&CAND, &IX.ia, sizeof CAND);
    for (int i = 0; i < NEWR.n && ok; i++)
        if (runs_append(&CAND, (u64)NEWR.r[i].lcn, NEWR.r[i].len))
            ok = 0;
    if (ok)
    {
        u64 alloc = runs_clusters(&CAND) * nt->csize;
        if (IX.has_ia)
        {
            u8 *a = rec_find(DREC, rs, AT_INDEX_ALLOCATION, "$I30");
            ok = a && nonres_set(DREC, rs, a, &CAND, alloc, bytes_after, bytes_after) == 0;
        }
        else
        {
            u32 l = build_nonres(tmp, sizeof tmp, AT_INDEX_ALLOCATION, "$I30", &CAND, alloc, bytes_after, bytes_after);
            ok = l && rec_insert(DREC, rs, tmp);
            if (ok)
            {
                static const u8 Z8[8];
                l = build_res(tmp, AT_BITMAP, "$I30", Z8, 8, 0);
                ok = rec_insert(DREC, rs, tmp) != 0;
            }
        }
    }
    if (ok && IX.ib_res)
    {
        u8 *b = rec_find(DREC, rs, AT_BITMAP, "$I30");
        if (!b)
            ok = 0;
        else if (need_bytes > rd32(b + 16))
        {
            u8 big[64];
            u32 old = rd32(b + 16), nl = al8((u32)need_bytes);
            if (nl > sizeof big || old > sizeof big)
                ok = 0;
            else
            {
                zero(big, sizeof big);
                copy(big, b + rd16(b + 20), old);
                ok = res_set(DREC, rs, b, big, nl) == 0;
            }
        }
    }
    if (!ok)
    {
        copy(DREC, DSNAP, rs);
        free_runs(nt, &NEWR, 0);
        return NTFS_ERR_FULL;
    }
    copy(&IX.ia, &CAND, sizeof IX.ia);
    IX.ia_size = bytes_after;
    IX.has_ia = 1;
    IX.dirty = 1;
    if (ib_set(nt, nblocks, 1))
        return NTFS_ERR_IO;
    *out = nblocks;
    return 0;
}

/* idx_new_block_mem edits only DREC in RAM. A block that is going to be pointed to by a block already on disk must
 * be allocated on disk first (the allocation size and the bitmap are in the folder's record), or a power cut leaves
 * a pointer to a block that does not exist: persist = write the record now. */
static int idx_new_block(Ntfs *nt, u64 *out, int persist)
{
    int r = idx_new_block_mem(nt, out);
    if (r == 0 && persist && IX.dirty)
    {
        IX.dirty = 0;
        IDX_WROTE = 1;
        if (rec_store(nt, IX.rec, DREC))
            return NTFS_ERR_IO;
    }
    return r;
}

/* ---- loading and storing a node --------------------------------------------- */
int node_load(Ntfs *nt, Node *n, u8 *buf, u64 blk)
{
    n->buf = buf;
    if (blk == ROOT_BLK)
    {
        u8 *a = rec_find(DREC, nt->rec_size, AT_INDEX_ROOT, "$I30");
        if (!a)
            return NTFS_ERR_IO;
        u32 vlen = rd32(a + 16);
        if (vlen > sizeof NB1 / 2)
            return NTFS_ERR_IO;
        copy(buf, a + rd16(a + 20), vlen);
        n->hdr = 16;
        n->root = 1;
        return nd_check(n, vlen) ? NTFS_ERR_IO : 0;
    }
    u32 bs = nt->idx_size;
    if (!IX.has_ia || (blk + 1) * bs > IX.ia_size)
        return NTFS_ERR_IO;
    if (runs_read(nt, &IX.ia, blk * bs, buf, bs))
        return NTFS_ERR_IO;
    if (buf[0] != 'I' || buf[1] != 'N' || buf[2] != 'D' || buf[3] != 'X' || apply_fixups(buf, bs))
        return NTFS_ERR_IO;
    n->hdr = 24;
    n->root = 0;
    return nd_check(n, bs) ? NTFS_ERR_IO : 0;
}

static int node_store(Ntfs *nt, Node *n, u64 blk)
{
    if (n->root)
    {
        u32 vlen = nd_end(n);
        wr32(n->buf + n->hdr + 8, vlen - n->hdr); /* allocated = used: a resident index */
        u8 *a = rec_find(DREC, nt->rec_size, AT_INDEX_ROOT, "$I30");
        if (!a || res_set(DREC, nt->rec_size, a, n->buf, vlen))
            return NTFS_ERR_FULL;
        IX.dirty = 1;
        return 0;
    }
    u32 bs = nt->idx_size, usa = rd16(n->buf + 4), cnt = rd16(n->buf + 6);
    u32 usn = (rd16(n->buf + usa) + 1u) & 0xFFFF;
    if (usn == 0 || usn == 0xFFFF)
        usn = 1;
    copy(ISEAL, n->buf, bs);
    wr16(ISEAL + usa, usn);
    for (u32 i = 1; i < cnt; i++)
    {
        u8 *end = ISEAL + i * 512 - 2;
        ISEAL[usa + 2 * i] = end[0];
        ISEAL[usa + 2 * i + 1] = end[1];
        wr16(end, usn);
    }
    wr16(n->buf + usa, usn);
    IDX_WROTE = 1;
    if (runs_write(nt, &IX.ia, blk * bs, ISEAL, bs))
    {
        nt->poisoned = 1;
        return NTFS_ERR_IO;
    }
    return 0;
}

/* an empty block image: header, update sequence array, just the end entry */
static void node_init_block(Ntfs *nt, Node *n, u8 *buf, u64 blk, int leaf)
{
    u32 bs = nt->idx_size, cnt = bs / 512 + 1, first = al8(0x28 + 2 * cnt);
    zero(buf, bs);
    buf[0] = 'I';
    buf[1] = 'N';
    buf[2] = 'D';
    buf[3] = 'X';
    wr16(buf + 4, 0x28);
    wr16(buf + 6, cnt);
    wr64(buf + 16, blk_vcn(nt, blk));
    wr32(buf + 24, first - 0x18);
    wr32(buf + 28, first - 0x18 + 16);
    wr32(buf + 32, bs - 0x18);
    buf[36] = leaf ? 0 : 1;
    wr16(buf + 0x28, 1);
    wr16(buf + first + 8, 16);
    wr16(buf + first + 12, 2);
    n->buf = buf;
    n->hdr = 24;
    n->root = 0;
    if (!leaf) /* the end entry of an inner node carries a pointer */
    {
        wr16(buf + first + 8, 24);
        wr16(buf + first + 12, 3);
        wr32(buf + 28, first - 0x18 + 24);
    }
}

static int nd_fits(const Ntfs *nt, const Node *n, u32 add)
{
    u32 end = nd_end(n);
    if (!n->root)
        return end + add <= n->hdr + rd32(n->buf + n->hdr + 8);
    u8 *a = rec_find(DREC, nt->rec_size, AT_INDEX_ROOT, "$I30");
    if (!a)
        return 0;
    u32 newlen = al8(rd16(a + 20) + end + add), oldlen = rd32(a + 4);
    return newlen <= oldlen || rd32(DREC + 24) + (newlen - oldlen) <= rd32(DREC + 28);
}

static void nd_insert(Node *n, u32 at, const u8 *e, u32 len)
{
    u32 end = nd_end(n);
    move_bytes(n->buf + at + len, n->buf + at, end - at);
    copy(n->buf + at, e, len);
    nd_set_end(n, end + len);
}

static void nd_remove(Node *n, u32 at)
{
    u32 len = e_len(n->buf + at), end = nd_end(n);
    move_bytes(n->buf + at, n->buf + at + len, end - at - len);
    nd_set_end(n, end - len);
}

/* ---- the root: single child, empty ------------------------------------------- */
/* the root becomes just an end entry: pointing to block `vcn` (child >= 0) or a plain empty leaf (child < 0) */
static int root_reset(Ntfs *nt, s64 child)
{
    u8 *a = rec_find(DREC, nt->rec_size, AT_INDEX_ROOT, "$I30");
    if (!a)
        return NTFS_ERR_IO;
    u8 v[56];
    zero(v, sizeof v);
    copy(v, a + rd16(a + 20), 16);
    u32 elen = child >= 0 ? 24 : 16;
    wr32(v + 16, 16);
    wr32(v + 20, 16 + elen);
    wr32(v + 24, 16 + elen);
    v[28] = child >= 0 ? 1 : 0;
    wr16(v + 32 + 8, elen);
    wr16(v + 32 + 12, child >= 0 ? 3 : 2);
    if (child >= 0)
        wr64(v + 32 + 16, (u64)child);
    if (res_set(DREC, nt->rec_size, a, v, 32 + elen))
        return NTFS_ERR_FULL;
    IX.dirty = 1;
    return 0;
}

/* The root is full: its entries move into a new block, and the root points to that block. Returns the block. */
static int push_down(Ntfs *nt, Node *root, u64 *blk)
{
    u32 bs = nt->idx_size;
    if (bs < 2048)
        return unsup(__LINE__);
    Node c;
    node_init_block(nt, &c, NB2, 0, nd_leaf(root));
    u32 r0 = nd_ent0(root), body = nd_end(root) - r0, c0 = nd_ent0(&c);
    if (c0 + body > bs)
        return unsup(__LINE__);
    copy(NB2 + c0, root->buf + r0, body);
    nd_set_end(&c, c0 + body);
    /* shrink the root first: the new attributes of a folder that had no allocation need its room */
    int r = root_reset(nt, 0);
    if (r)
        return r;
    r = idx_new_block(nt, blk, 0); /* the root is a placeholder just now: do not write the record */
    if (r)
        return r;
    wr64(NB2 + 16, blk_vcn(nt, *blk));
    r = node_store(nt, &c, *blk);
    if (r)
        return r;
    return root_reset(nt, (s64)blk_vcn(nt, *blk));
}

/* ---- search ------------------------------------------------------------------ */
/* Walk down from the root to the entry for `key` (a $FILE_NAME). PATH / PDEPTH say how we got there; for a name
 * that is not there, the last level is the leaf and pos is where it goes. */
int idx_locate(Ntfs *nt, const u8 *key, int *found)
{
    u64 blk = ROOT_BLK;
    *found = 0;
    for (int lv = 0;; lv++)
    {
        Node n;
        int r = node_load(nt, &n, NB1, blk);
        if (r)
            return r;
        u32 e0 = nd_ent0(&n), p = e0;
        const u8 *e;
        for (;; p += e_len(e))
        {
            e = n.buf + p;
            if (e_end(e))
                break;
            int c = key_cmp(nt, key, e + 16);
            if (c == 0)
            {
                PATH[lv].blk = blk;
                PATH[lv].pos = p - e0;
                PDEPTH = lv + 1;
                *found = 1;
                return 0;
            }
            if (c < 0)
                break;
        }
        PATH[lv].blk = blk;
        PATH[lv].pos = p - e0;
        if (!e_sub(e))
        {
            PDEPTH = lv + 1;
            return 0;
        }
        if (lv + 1 >= MAXD)
            return NTFS_ERR_IO;
        u64 v = e_ptr(e);
        if ((v * vcn_unit(nt)) % nt->idx_size)
            return NTFS_ERR_IO;
        blk = vcn_blk(nt, v);
        if ((blk + 1) * nt->idx_size > IX.ia_size || ib_get(nt, blk) != 1)
            return NTFS_ERR_IO;
    }
}

/* Like idx_locate, for a change that may add blocks to the index. A new block makes the folder's record bigger (one
 * more run in the $INDEX_ALLOCATION list, now and then 8 more bytes of bitmap), and the root keeps taking separators
 * until the record is full: with no room left the block could not be added in the middle of a split. So when the
 * record has too little room for the blocks this change can add (one per level of the tree), the root's entries go
 * down into a block first, which frees most of the root. */
static int idx_locate_room(Ntfs *nt, const u8 *key, int *found, int extra_levels)
{
    int r = idx_locate(nt, key, found);
    if (r || !IX.has_ia)
        return r; /* without an allocation the push-down makes its own room */
    if (rd32(DREC + 28) - rd32(DREC + 24) >= 32 + 25 * (u32)(PDEPTH + extra_levels))
        return 0;
    Node n;
    r = node_load(nt, &n, NB1, ROOT_BLK);
    if (r || nd_empty(&n))
        return r; /* nothing to move down: the record is full of other things */
    u64 nb;
    r = push_down(nt, &n, &nb);
    if (r)
        return r;
    return idx_locate(nt, key, found);
}

/* ---- insert -------------------------------------------------------------------- */
/* Put entry E (elen bytes) into the node of PATH[lv] before the entry at PATH[lv].pos. When fixptr >= 0 the entry
 * after the new one gets that child pointer (a split: the old child keeps the left half, the new block the right).
 * A full block splits: the middle entry goes up to the parent and so on up to the root; a full root pushes its
 * entries down into a block. */
static int ins_entry(Ntfs *nt, int lv, const u8 *E0, u32 elen, s64 fixptr)
{
    copy(EWORK, E0, elen);
    for (;;)
    {
        Lvl *L = &PATH[lv];
        Node n;
        int r = node_load(nt, &n, NB1, L->blk);
        if (r)
            return r;
        u32 e0 = nd_ent0(&n), at = e0 + L->pos;
        if (nd_fits(nt, &n, elen))
        {
            nd_insert(&n, at, EWORK, elen);
            if (fixptr >= 0)
                e_setptr(n.buf + at + elen, (u64)fixptr);
            return node_store(nt, &n, L->blk);
        }
        if (n.root)
        {
            u64 nb;
            u32 pos = L->pos;
            r = push_down(nt, &n, &nb);
            if (r)
                return r;
            PATH[1].blk = nb;
            PATH[1].pos = pos;
            lv = 1;
            continue;
        }

        /* split: the entries with the new one in SPL, the middle entry M goes up */
        u32 end = nd_end(&n), body = end - e0, pos = L->pos;
        copy(SPL, n.buf + e0, pos);
        copy(SPL + pos, EWORK, elen);
        copy(SPL + pos + elen, n.buf + at, end - at);
        if (fixptr >= 0)
            e_setptr(SPL + pos + elen, (u64)fixptr);
        u32 total = body + elen, half = total / 2, p = 0, m = 0, cnt = 0;
        int leaf = nd_leaf(&n);
        for (; !e_end(SPL + p); p += e_len(SPL + p))
        {
            if (p >= half && cnt >= 1 && !m && !e_end(SPL + p + e_len(SPL + p))) /* not the last name: the right half needs one */
                m = p;
            cnt++;
        }
        if (!m || cnt < 3)
            return unsup(__LINE__);
        u32 mlen = e_len(SPL + m), after = m + mlen, endlen = e_len(SPL + p);
        u64 this_vcn = blk_vcn(nt, L->blk);
        /* the promoted entry, pointing at this node (which keeps the left half) */
        static u8 M[640];
        copy(M, SPL + m, mlen);
        u32 mnew = mlen;
        if (leaf)
        {
            zero(M + mlen, 8);
            mnew = mlen + 8;
            wr16(M + 8, mnew);
            wr16(M + 12, rd16(M + 12) | 1);
        }
        wr64(M + mnew - 8, this_vcn);
        /* the right half: a new block */
        u64 rb;
        r = idx_new_block(nt, &rb, 1);
        if (r)
            return r;
        Node rn;
        node_init_block(nt, &rn, NB2, rb, leaf);
        u32 r0 = nd_ent0(&rn), rbody = total - after;
        copy(NB2 + r0, SPL + after, rbody);
        nd_set_end(&rn, r0 + rbody);
        (void)endlen;
        r = node_store(nt, &rn, rb);
        if (r)
            return r;
        /* the left half: entries before M, then an end entry (pointing where M pointed) */
        u32 lbody = m;
        copy(n.buf + e0, SPL, lbody);
        u32 lend = e0 + lbody;
        zero(n.buf + lend, leaf ? 16 : 24);
        wr16(n.buf + lend + 8, leaf ? 16 : 24);
        wr16(n.buf + lend + 12, leaf ? 2 : 3);
        if (!leaf)
            wr64(n.buf + lend + 16, rd64(SPL + m + mlen - 8));
        nd_set_end(&n, lend + (leaf ? 16 : 24));
        r = node_store(nt, &n, L->blk);
        if (r)
            return r;
        /* go up: insert M in the parent before the entry that pointed here, which now points to the new block */
        copy(EWORK, M, mnew);
        elen = mnew;
        fixptr = (s64)blk_vcn(nt, rb);
        lv--;
    }
}

/* add a name (a ready entry without sub-node) to the open folder */
int idx_add(Ntfs *nt, const u8 *ent, u32 elen)
{
    int found;
    int r = idx_locate_room(nt, ent + 16, &found, 1);
    if (r)
        return r;
    if (found)
        return NTFS_ERR_EXISTS;
    return ins_entry(nt, PDEPTH - 1, ent, elen, -1);
}

/* ---- remove ---------------------------------------------------------------------- */
static int prune(Ntfs *nt, int lv);

/* take the entry at PATH[lv].pos out of its node. *empty: the node has no entries left. */
static int del_at(Ntfs *nt, int lv, int *empty)
{
    Node n;
    int r = node_load(nt, &n, NB1, PATH[lv].blk);
    if (r)
        return r;
    nd_remove(&n, nd_ent0(&n) + PATH[lv].pos);
    *empty = nd_empty(&n);
    return node_store(nt, &n, PATH[lv].blk);
}

/* The block at PATH[lv] (not the root) has to go: it has no entries. Its parent's entry X that points to it:
 *  - X is a separator (a name): the name moves down into the neighbour on the right, X goes;
 *  - X is the end entry: the last name before it moves down into the neighbour on the left and becomes... the end
 *    entry takes over that name's child pointer;
 *  - X is the only entry: the parent has no children any more and goes too (the root becomes an empty leaf). */
static int prune(Ntfs *nt, int lv)
{
    u64 gone = PATH[lv].blk;
    int pl = lv - 1;
    Node p;
    int r = node_load(nt, &p, NB1, PATH[pl].blk);
    if (r)
        return r;
    u32 e0 = nd_ent0(&p), at = e0 + PATH[pl].pos;
    u32 dlen = 0;
    if (!e_end(p.buf + at))
    {
        dlen = e_len(p.buf + at) - 8;
        copy(DEMOTE, p.buf + at, dlen);
        wr16(DEMOTE + 8, dlen);
        wr16(DEMOTE + 12, rd16(DEMOTE + 12) & ~1u);
        nd_remove(&p, at);
    }
    else if (at == e0)
    {
        if (ib_set(nt, gone, 0))
            return NTFS_ERR_IO;
        if (p.root)
            return root_reset(nt, -1);
        return prune(nt, pl); /* the parent is left with nothing below it */
    }
    else
    {
        u32 y = e0;
        while (y + e_len(p.buf + y) != at)
            y += e_len(p.buf + y);
        dlen = e_len(p.buf + y) - 8;
        u64 ychild = e_ptr(p.buf + y);
        copy(DEMOTE, p.buf + y, dlen);
        wr16(DEMOTE + 8, dlen);
        wr16(DEMOTE + 12, rd16(DEMOTE + 12) & ~1u);
        nd_remove(&p, y); /* the end entry moved to y */
        e_setptr(p.buf + y, ychild);
    }
    r = node_store(nt, &p, PATH[pl].blk);
    if (r)
        return r;
    if (ib_set(nt, gone, 0))
        return NTFS_ERR_IO;
    return idx_add(nt, DEMOTE, dlen);
}

/* take a leaf entry out (at PATH[lv]) and free the block when that leaves it empty */
static int rm_leaf_entry(Ntfs *nt, int lv)
{
    int empty;
    int r = del_at(nt, lv, &empty);
    if (r)
        return r;
    if (empty && lv > 0)
        return prune(nt, lv);
    return 0;
}

/* Remove the entry for `key` from the open folder. An entry inside an inner node is replaced by the smallest
 * entry of the subtree on its right (taken out of its leaf). */
int idx_remove(Ntfs *nt, const u8 *key)
{
    int found;
    int r = idx_locate_room(nt, key, &found, 2);
    if (r)
        return r;
    if (!found)
        return NTFS_ERR_IO;
    int lv = PDEPTH - 1;
    Node n;
    r = node_load(nt, &n, NB1, PATH[lv].blk);
    if (r)
        return r;
    u32 e0 = nd_ent0(&n), at = e0 + PATH[lv].pos;
    if (!e_sub(n.buf + at))
        return rm_leaf_entry(nt, lv);

    /* inner node: find the successor, the first entry of the leftmost leaf below the entry after this one */
    u32 zpos = PATH[lv].pos + e_len(n.buf + at);
    u64 v = e_ptr(n.buf + at + e_len(n.buf + at));
    PATH[lv].pos = zpos;
    int l2 = lv;
    for (;;)
    {
        if (++l2 >= MAXD || (v * vcn_unit(nt)) % nt->idx_size)
            return NTFS_ERR_IO;
        u64 blk = vcn_blk(nt, v);
        if ((blk + 1) * nt->idx_size > IX.ia_size || ib_get(nt, blk) != 1)
            return NTFS_ERR_IO;
        r = node_load(nt, &n, NB1, blk);
        if (r)
            return r;
        PATH[l2].blk = blk;
        PATH[l2].pos = 0;
        const u8 *f = n.buf + nd_ent0(&n);
        if (nd_leaf(&n))
        {
            if (e_end(f))
                return NTFS_ERR_IO;
            u32 sl = e_len(f);
            copy(SENT, f, sl);
            break;
        }
        v = e_ptr(f);
    }
    r = rm_leaf_entry(nt, l2); /* S leaves its leaf (the leaf may go too) */
    if (r)
        return r;
    u32 slen_ = e_len(SENT);

    /* now E: still in its inner node, unless freeing a block made it move down into a leaf */
    r = idx_locate(nt, key, &found);
    if (r)
        return r;
    if (!found)
        return NTFS_ERR_IO;
    lv = PDEPTH - 1;
    r = node_load(nt, &n, NB1, PATH[lv].blk);
    if (r)
        return r;
    e0 = nd_ent0(&n);
    at = e0 + PATH[lv].pos;
    if (!e_sub(n.buf + at))
    {
        r = rm_leaf_entry(nt, lv);
        if (r)
            return r;
        return idx_add(nt, SENT, slen_);
    }
    static u8 R[640];
    u64 child = e_ptr(n.buf + at);
    copy(R, SENT, slen_);
    zero(R + slen_, 8);
    wr16(R + 8, slen_ + 8);
    wr16(R + 12, rd16(R + 12) | 1);
    wr64(R + slen_, child);
    nd_remove(&n, at);
    r = node_store(nt, &n, PATH[lv].blk);
    if (r)
        return r;
    return ins_entry(nt, lv, R, slen_ + 8, -1);
}

/* patch the fixed-size part of an entry's key (times, sizes, flags) in place */
int idx_update(Ntfs *nt, const u8 *key, const u8 *newkey)
{
    int found;
    int r = idx_locate(nt, key, &found);
    if (r)
        return r;
    if (!found)
        return NTFS_ERR_IO;
    Node n;
    r = node_load(nt, &n, NB1, PATH[PDEPTH - 1].blk);
    if (r)
        return r;
    u8 *e = n.buf + nd_ent0(&n) + PATH[PDEPTH - 1].pos;
    copy(e + 16 + 8, newkey + 8, 56); /* times, sizes, flags, reparse: bytes 8..63 */
    return node_store(nt, &n, PATH[PDEPTH - 1].blk);
}

/* is the folder (open) free of names? */
int idx_is_empty(Ntfs *nt, int *empty)
{
    *empty = 1;
    Node n;
    int r = node_load(nt, &n, NB1, ROOT_BLK);
    if (r)
        return r;
    if (!nd_empty(&n))
    {
        *empty = 0;
        return 0;
    }
    u64 nblocks = IX.has_ia ? IX.ia_size / nt->idx_size : 0;
    for (u64 b = 0; b < nblocks; b++)
    {
        int used = ib_get(nt, b);
        if (used < 0)
            return NTFS_ERR_IO;
        if (!used)
            continue;
        r = node_load(nt, &n, NB1, b);
        if (r)
            return r;
        if (!nd_empty(&n))
        {
            *empty = 0;
            return 0;
        }
    }
    return 0;
}
