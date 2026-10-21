/* Test helper: an NTFS consistency checker that does not use ntfs.c. It reads the image the way chkdsk would and
 * reports (printing "  nfsck: ..."): bad fixups, bad attribute chains, clusters owned twice, clusters owned but
 * free in $Bitmap (and, strict, used by nobody), MFT records in use but free in the MFT bitmap (and the reverse),
 * folder indexes that are not a valid B-tree (order, depth, blocks reached twice, blocks used but unreachable,
 * the "INDX" header), index entries that do not match the $FILE_NAME of the file they point to, link counts,
 * the dirty bit.
 *
 * nck_check(img, nclusters, reserved, nres, strict):
 *   strict = 1: the volume must be perfect (after a complete change).
 *   strict = 0: what a power cut may leave: leaked clusters / blocks / records, files that no folder lists, and
 *     entries lost from an index are all allowed; things that could damage data are not (clusters owned by two
 *     files, an owned cluster that is free, an index entry pointing at a free record, a broken tree).
 * Needs ntfsimg.h included first (NT_* sizes are not used: the geometry comes from the boot sector). */
#ifndef HARIS_TEST_NTFSCHECK_H
#define HARIS_TEST_NTFSCHECK_H

static int nck_errors;
#define NBAD(...) do { printf("  nfsck: " __VA_ARGS__); printf("\n"); nck_errors++; } while (0)

typedef unsigned char NU8;
static unsigned nk_rd16(const NU8 *p) { return p[0] | p[1] << 8; }
static unsigned nk_rd32(const NU8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned long long nk_rd64(const NU8 *p) { return nk_rd32(p) | (unsigned long long)nk_rd32(p + 4) << 32; }

typedef struct { unsigned long long vcn, len; long long lcn; } NkRun;
typedef struct { int n; NkRun r[256]; } NkRuns;

static const NU8 *nk_img;
static unsigned nk_cs, nk_rs, nk_is;
static unsigned long long nk_nclusters;
static NkRuns nk_mft;
static unsigned nk_nrec;
static const unsigned short *nk_upcase;   /* the test says what $UpCase holds */
static NU8 *nk_own;                        /* per cluster: owned by a file */
static int nk_strict;

static int nk_runs(const NU8 *p, const NU8 *end, unsigned long long vcn, NkRuns *rs)
{
    long long lcn = 0;
    rs->n = 0;
    while (p < end && *p) {
        unsigned nl = *p & 15, no = *p >> 4;
        p++;
        if (!nl || nl > 8 || no > 8 || p + nl + no > end) return -1;
        unsigned long long len = 0;
        for (unsigned i = 0; i < nl; i++) len |= (unsigned long long)p[i] << (8 * i);
        p += nl;
        long long delta = 0;
        for (unsigned i = 0; i < no; i++) delta |= (long long)((unsigned long long)p[i] << (8 * i));
        if (no && (p[no - 1] & 0x80)) for (unsigned i = no; i < 8; i++) delta |= (long long)((unsigned long long)0xFF << (8 * i));
        p += no;
        if (rs->n >= 256) return -1;
        rs->r[rs->n].vcn = vcn;
        rs->r[rs->n].len = len;
        if (no) { lcn += delta; rs->r[rs->n].lcn = lcn; } else rs->r[rs->n].lcn = -1;
        rs->n++;
        vcn += len;
    }
    return 0;
}

/* bytes of a stream described by runs */
static int nk_read(const NkRuns *rs, unsigned long long off, NU8 *out, unsigned len)
{
    while (len) {
        unsigned long long vcn = off / nk_cs;
        unsigned in = (unsigned)(off % nk_cs);
        int found = -1;
        for (int i = 0; i < rs->n; i++) if (vcn >= rs->r[i].vcn && vcn < rs->r[i].vcn + rs->r[i].len) { found = i; break; }
        if (found < 0 || rs->r[found].lcn < 0) return -1;
        unsigned long long left = (rs->r[found].vcn + rs->r[found].len - vcn) * nk_cs - in;
        unsigned n = left < len ? (unsigned)left : len;
        memcpy(out, nk_img + (unsigned long long)(rs->r[found].lcn + (long long)(vcn - rs->r[found].vcn)) * nk_cs + in, n);
        out += n; off += n; len -= n;
    }
    return 0;
}

static int nk_fix(NU8 *b, unsigned len)
{
    unsigned usa = nk_rd16(b + 4), n = nk_rd16(b + 6);
    if (n < 2 || usa + n * 2 > len || (n - 1) * 512 != len) return -1;
    unsigned usn = nk_rd16(b + usa);
    for (unsigned i = 1; i < n; i++) {
        NU8 *e = b + i * 512 - 2;
        if (nk_rd16(e) != usn) return -1;
        e[0] = b[usa + 2 * i]; e[1] = b[usa + 2 * i + 1];
    }
    return 0;
}

/* 0 = no FILE record (never used), 1 = good, -1 = damaged */
static int nk_rec(unsigned recno, NU8 *out)
{
    if (nk_read(&nk_mft, (unsigned long long)recno * nk_rs, out, nk_rs)) return -1;
    if (memcmp(out, "FILE", 4) != 0) return 0;
    return nk_fix(out, nk_rs) ? -1 : 1;
}

static unsigned nk_up(unsigned c) { return nk_upcase ? nk_upcase[c] : (c >= 'a' && c <= 'z' ? c - 32 : c); }

static int nk_cmp(const NU8 *a, const NU8 *b)
{
    unsigned la = a[64], lb = b[64], n = la < lb ? la : lb;
    for (unsigned i = 0; i < n; i++) {
        unsigned x = nk_up(nk_rd16(a + 66 + 2 * i)), y = nk_up(nk_rd16(b + 66 + 2 * i));
        if (x != y) return x < y ? -1 : 1;
    }
    return la == lb ? 0 : la < lb ? -1 : 1;
}

static const NU8 *nk_attr_find(const NU8 *rec, unsigned type, const char *name)
{
    unsigned o = nk_rd16(rec + 20);
    while (o + 8 <= nk_rs && nk_rd32(rec + o) != 0xFFFFFFFFu) {
        const NU8 *a = rec + o;
        unsigned len = nk_rd32(a + 4);
        if (len < 16 || (len & 7) || o + len > nk_rs) return 0;
        if (nk_rd32(a) == type) {
            unsigned nl = a[9];
            int ok = name ? nl == strlen(name) : nl == 0;
            for (unsigned i = 0; ok && name && i < nl; i++) if (nk_rd16(a + nk_rd16(a + 10) + 2 * i) != (NU8)name[i]) ok = 0;
            if (ok) return a;
        }
        o += len;
    }
    return 0;
}

/* ---- names found in the records, entries found in the indexes ---- */
#define NK_MAX 4096
typedef struct { unsigned rec, seq, nattr; NU8 key[600]; unsigned klen; } NkName;
typedef struct { unsigned dir; unsigned rec; unsigned seq; NU8 key[600]; unsigned klen; } NkEnt;
static NkName nk_names[NK_MAX];
static int nk_nnames;
static NkEnt nk_ents[NK_MAX];
static int nk_nents;
static NU8 *nk_inuse;     /* per record: 1 = in use */
static unsigned nk_seqs[NK_MAX];

typedef struct {
    unsigned dir;
    NkRuns ia;
    unsigned long long ia_size;
    const NU8 *bm;           /* the index bitmap (resident copy) */
    unsigned bm_len;
    NU8 visited[1024];
    int leaf_depth;
    const NU8 *prev;         /* previous key in order */
    NU8 prev_buf[600];
    int have_prev;
} NkDir;

static void nk_walk_node(NkDir *d, const NU8 *n, unsigned e0, unsigned end, int depth, int is_root);

static void nk_child(NkDir *d, unsigned long long vcn, int depth)
{
    unsigned unit = nk_cs <= nk_is ? nk_cs : 512;
    if ((vcn * unit) % nk_is) { NBAD("dir %u: sub-node VCN %llu is not on a block boundary", d->dir, vcn); return; }
    unsigned long long blk = vcn * unit / nk_is;
    if ((blk + 1) * nk_is > d->ia_size) { NBAD("dir %u: sub-node block %llu is beyond the allocation", d->dir, blk); return; }
    if (blk >= 8192) { NBAD("dir %u: block %llu too far for the checker", d->dir, blk); return; }
    if (d->visited[blk / 8] & (1 << (blk % 8))) { NBAD("dir %u: block %llu is reached twice", d->dir, blk); return; }
    d->visited[blk / 8] |= (NU8)(1 << (blk % 8));
    if (!(blk / 8 < d->bm_len && (d->bm[blk / 8] >> (blk % 8)) & 1)) NBAD("dir %u: block %llu is in the tree but free in the index bitmap", d->dir, blk);
    static NU8 buf[4][16384];
    if (depth >= 4) { NBAD("dir %u: tree too deep", d->dir); return; }
    NU8 *b = buf[depth];
    if (nk_read(&d->ia, blk * nk_is, b, nk_is)) { NBAD("dir %u: block %llu cannot be read", d->dir, blk); return; }
    if (memcmp(b, "INDX", 4) != 0) { NBAD("dir %u: block %llu has no INDX magic", d->dir, blk); return; }
    if (nk_fix(b, nk_is)) { NBAD("dir %u: block %llu fixups are wrong", d->dir, blk); return; }
    if (nk_rd64(b + 16) != vcn) NBAD("dir %u: block %llu says it is VCN %llu, not %llu", d->dir, blk, nk_rd64(b + 16), vcn);
    unsigned e0 = 24 + nk_rd32(b + 24), used = 24 + nk_rd32(b + 28), alloc = 24 + nk_rd32(b + 32);
    if (alloc != nk_is) NBAD("dir %u: block %llu allocated size %u", d->dir, blk, alloc);
    if (used > alloc || e0 >= used) { NBAD("dir %u: block %llu bad index lengths", d->dir, blk); return; }
    nk_walk_node(d, b, e0, used, depth, 0);
}

static void nk_walk_node(NkDir *d, const NU8 *n, unsigned e0, unsigned end, int depth, int is_root)
{
    (void)is_root;
    unsigned flags = n[(is_root ? 16 : 24) + 12];
    int kids = (flags & 1) != 0;
    for (unsigned p = e0;;) {
        if (p + 16 > end) { NBAD("dir %u: entry runs past the node", d->dir); return; }
        const NU8 *e = n + p;
        unsigned len = nk_rd16(e + 8), kl = nk_rd16(e + 10), fl = nk_rd16(e + 12);
        if (len < 16 || (len & 7) || p + len > end) { NBAD("dir %u: bad entry length %u", d->dir, len); return; }
        if (!!(fl & 1) != kids) { NBAD("dir %u: sub-node flag of an entry disagrees with its node", d->dir); return; }
        if (kids) nk_child(d, nk_rd64(e + len - 8), depth + 1);
        else {
            if (d->leaf_depth < 0) d->leaf_depth = depth;
            else if (d->leaf_depth != depth) NBAD("dir %u: leaves at depth %d and %d", d->dir, d->leaf_depth, depth);
        }
        if (fl & 2) {
            if (p + len != end) NBAD("dir %u: end entry is not last", d->dir);
            return;
        }
        if (kl < 66 || 16 + kl > len) { NBAD("dir %u: bad key length %u", d->dir, kl); return; }
        const NU8 *key = e + 16;
        if (d->have_prev && nk_cmp(d->prev_buf, key) >= 0) NBAD("dir %u: names out of order (or twice) at '%c'", d->dir, key[66]);
        memcpy(d->prev_buf, key, kl > 600 ? 600 : kl);
        d->have_prev = 1;
        if (nk_nents < NK_MAX) {
            NkEnt *x = &nk_ents[nk_nents++];
            x->dir = d->dir;
            x->rec = (unsigned)(nk_rd64(e) & 0xFFFFFFFFFFFFULL);
            x->seq = (unsigned)(nk_rd64(e) >> 48);
            x->klen = kl > 600 ? 600 : kl;
            memcpy(x->key, key, x->klen);
        }
        p += len;
    }
}

static int nk_stream_bytes(const NU8 *a, NU8 *out, unsigned long long off, unsigned len);

static void nk_dir(unsigned dirno, const NU8 *rec)
{
    NkDir *d = calloc(1, sizeof *d);
    NU8 *bmcopy = 0;
    d->dir = dirno;
    d->leaf_depth = -1;
    const NU8 *root = nk_attr_find(rec, 0x90, "$I30");
    if (!root || root[8]) { NBAD("dir %u: no resident $INDEX_ROOT", dirno); free(d); return; }
    const NU8 *v = root + nk_rd16(root + 20);
    unsigned vlen = nk_rd32(root + 16);
    if (nk_rd32(v) != 0x30 || nk_rd32(v + 4) != 1 || nk_rd32(v + 8) != nk_is) NBAD("dir %u: index root header (type/collation/block size)", dirno);
    unsigned e0 = 16 + nk_rd32(v + 16), used = 16 + nk_rd32(v + 20), alloc = 16 + nk_rd32(v + 24);
    if (used > vlen || alloc != used || e0 >= used) { NBAD("dir %u: index root lengths (value %u, used %u, alloc %u)", dirno, vlen, used, alloc); free(d); return; }
    const NU8 *ia = nk_attr_find(rec, 0xA0, "$I30"), *ib = nk_attr_find(rec, 0xB0, "$I30");
    int large = v[28] & 1;
    if (large && !ia) { NBAD("dir %u: large index without $INDEX_ALLOCATION", dirno); free(d); return; }
    if (ia) {
        if (!ia[8]) { NBAD("dir %u: resident $INDEX_ALLOCATION", dirno); free(d); return; }
        if (nk_runs(ia + nk_rd16(ia + 32), ia + nk_rd32(ia + 4), nk_rd64(ia + 16), &d->ia)) { NBAD("dir %u: bad runs of $INDEX_ALLOCATION", dirno); free(d); return; }
        d->ia_size = nk_rd64(ia + 48);
        if (d->ia_size % nk_is || nk_rd64(ia + 56) != d->ia_size) NBAD("dir %u: $INDEX_ALLOCATION sizes", dirno);
        if (!ib) { NBAD("dir %u: no index bitmap", dirno); free(d); return; }
        if (!ib[8]) { d->bm = ib + nk_rd16(ib + 20); d->bm_len = nk_rd32(ib + 16); }
        else {   /* non-resident: read it through its runs */
            d->bm_len = (unsigned)nk_rd64(ib + 48);
            NU8 *copy = calloc(1, d->bm_len + 8);
            if (nk_stream_bytes(ib, copy, 0, d->bm_len)) NBAD("dir %u: index bitmap cannot be read", dirno);
            d->bm = bmcopy = copy;
        }
        if (d->bm_len * 8 < d->ia_size / nk_is) NBAD("dir %u: index bitmap too short", dirno);
    }
    nk_walk_node(d, v, e0, used, 0, 1);
    unsigned long long nblocks = d->ia_size / nk_is;
    for (unsigned long long b = 0; b < nblocks && b < 8192; b++) {
        int marked = b / 8 < d->bm_len && ((d->bm[b / 8] >> (b % 8)) & 1);
        int seen = (d->visited[b / 8] >> (b % 8)) & 1;
        if (marked && !seen && nk_strict) NBAD("dir %u: block %llu is marked used but nothing points to it", dirno, b);
    }
    free(bmcopy);
    free(d);
}

/* ---- the whole volume ---- */
static void nk_claim(unsigned long long lcn, unsigned long long len, unsigned rec, const char *what)
{
    for (unsigned long long c = lcn; c < lcn + len; c++) {
        if (c >= nk_nclusters) { NBAD("record %u %s: cluster %llu is outside the volume", rec, what, c); return; }
        if (nk_own[c]) NBAD("record %u %s: cluster %llu is owned twice", rec, what, c);
        nk_own[c] = 1;
    }
}

static int nk_stream_bytes(const NU8 *a, NU8 *out, unsigned long long off, unsigned len)
{
    if (a[8] == 0) { if (off + len > nk_rd32(a + 16)) return -1; memcpy(out, a + nk_rd16(a + 20) + off, len); return 0; }
    NkRuns rs;
    if (nk_runs(a + nk_rd16(a + 32), a + nk_rd32(a + 4), nk_rd64(a + 16), &rs)) return -1;
    return nk_read(&rs, off, out, len);
}

/* returns the number of errors */
static int nck_check(const NU8 *img, unsigned long long nclusters, const unsigned long long (*reserved)[2], int nres, int strict)
{
    nck_errors = 0;
    nk_img = img;
    nk_strict = strict;
    nk_nclusters = nclusters;
    nk_nents = nk_nnames = 0;
    unsigned ss = nk_rd16(img + 11), spc = img[13];
    nk_cs = ss * spc;
    nk_rs = (signed char)img[64] > 0 ? (unsigned)(signed char)img[64] * nk_cs : 1u << -(signed char)img[64];
    nk_is = (signed char)img[68] > 0 ? (unsigned)(signed char)img[68] * nk_cs : 1u << -(signed char)img[68];
    unsigned long long mft_lcn = nk_rd64(img + 48), mirr_lcn = nk_rd64(img + 56);
    NU8 *rec = malloc(nk_rs);
    nk_mft.n = 1; nk_mft.r[0].vcn = 0; nk_mft.r[0].len = 1; nk_mft.r[0].lcn = (long long)mft_lcn;
    if (nk_read(&nk_mft, 0, rec, nk_rs) || memcmp(rec, "FILE", 4) || nk_fix(rec, nk_rs)) { NBAD("MFT record 0 is damaged"); free(rec); return nck_errors; }
    const NU8 *md = nk_attr_find(rec, 0x80, 0);
    if (!md || !md[8] || nk_runs(md + nk_rd16(md + 32), md + nk_rd32(md + 4), 0, &nk_mft)) { NBAD("$MFT has no runs"); free(rec); return nck_errors; }
    unsigned long long mft_bytes = nk_rd64(md + 48);
    nk_nrec = (unsigned)(mft_bytes / nk_rs);
    if (nk_nrec > NK_MAX) nk_nrec = NK_MAX;
    nk_own = calloc((size_t)nclusters + 1, 1);
    nk_inuse = calloc(nk_nrec + 1, 1);
    for (int i = 0; i < nres; i++) for (unsigned long long c = reserved[i][0]; c < reserved[i][1]; c++) nk_own[c] = 2;   /* 2 = reserved: not "owned twice" */

    /* MFT bitmap */
    NU8 *mbm = calloc(nk_nrec / 8 + 8, 1);
    const NU8 *mb = nk_attr_find(rec, 0xB0, 0);
    if (!mb) NBAD("$MFT has no $BITMAP");
    else {
        unsigned long long sz = mb[8] ? nk_rd64(mb + 48) : nk_rd32(mb + 16);
        if (sz < nk_nrec / 8) NBAD("MFT bitmap has %llu bytes for %u records", sz, nk_nrec);
        else if (nk_stream_bytes(mb, mbm, 0, nk_nrec / 8)) NBAD("MFT bitmap cannot be read");
    }

    NU8 *r = malloc(nk_rs);
    static NU8 *dirs_rec[NK_MAX];
    for (unsigned i = 0; i < nk_nrec; i++) {
        int st = nk_rec(i, r);
        if (st < 0) { NBAD("record %u: damaged (fixups)", i); continue; }
        int bit = (mbm[i / 8] >> (i % 8)) & 1;
        if (st == 0 || !(nk_rd16(r + 22) & 1)) {
            if (i < 24 && !bit) NBAD("record %u: reserved but free in the MFT bitmap", i);
            if (i >= 24 && bit && nk_strict) NBAD("record %u: not in use but marked in the MFT bitmap", i);
            continue;
        }
        nk_inuse[i] = 1;
        nk_seqs[i] = nk_rd16(r + 16);
        if (!bit) NBAD("record %u: in use but free in the MFT bitmap", i);
        if (nk_rd32(r + 44) != i && nk_rd16(r + 4) >= 0x30) NBAD("record %u: record number field says %u", i, nk_rd32(r + 44));
        unsigned used = nk_rd32(r + 24), alloc = nk_rd32(r + 28);
        if (used > alloc || alloc != nk_rs) NBAD("record %u: bytes in use %u / allocated %u", i, used, alloc);
        /* attributes */
        unsigned o = nk_rd16(r + 20), prev_type = 0, nfn_nodos = 0;
        int saw_list = 0;
        int ended = 0;
        while (o + 8 <= used) {
            const NU8 *a = r + o;
            unsigned type = nk_rd32(a);
            if (type == 0xFFFFFFFFu) { ended = 1; if (o + 8 != used) NBAD("record %u: end marker at %u but %u bytes in use", i, o, used); break; }
            unsigned len = nk_rd32(a + 4);
            if (len < 16 || (len & 7) || o + len > used) { NBAD("record %u: bad attribute length %u at %u", i, len, o); break; }
            if (type < prev_type) NBAD("record %u: attribute 0x%X after 0x%X", i, type, prev_type);
            prev_type = type;
            if (type == 0x20) saw_list = 1;
            if (a[8] == 0) {
                if (nk_rd16(a + 20) + nk_rd32(a + 16) > len) NBAD("record %u: resident value of 0x%X runs past its attribute", i, type);
                if (type == 0x30 && nk_rd32(a + 16) >= 66) {
                    const NU8 *k = a + nk_rd16(a + 20);
                    if (k[65] != 2) nfn_nodos++;
                    if (nk_nnames < NK_MAX) {
                        NkName *nm = &nk_names[nk_nnames++];
                        nm->rec = i; nm->seq = nk_seqs[i]; nm->klen = nk_rd32(a + 16) > 600 ? 600 : nk_rd32(a + 16);
                        memcpy(nm->key, k, nm->klen);
                    }
                }
            } else {
                NkRuns rs;
                unsigned long long start = nk_rd64(a + 16);
                if (nk_runs(a + nk_rd16(a + 32), a + len, start, &rs)) { NBAD("record %u: bad runs of 0x%X", i, type); }
                else {
                    unsigned long long total = 0;
                    for (int k = 0; k < rs.n; k++) { total += rs.r[k].len; if (rs.r[k].lcn >= 0) nk_claim((unsigned long long)rs.r[k].lcn, rs.r[k].len, i, "data"); }
                    int whole = start == 0 && nk_rd64(r + 32) == 0 && !saw_list;   /* one extent holds the whole stream */
                    if (whole && !(nk_rd16(a + 12) & 0x8000) && total * nk_cs != nk_rd64(a + 40))
                        NBAD("record %u: 0x%X has %llu clusters but allocated size %llu", i, type, total, nk_rd64(a + 40));
                    if (start == 0 && (nk_rd64(a + 48) > nk_rd64(a + 40) || nk_rd64(a + 56) > nk_rd64(a + 48)))
                        NBAD("record %u: 0x%X sizes alloc %llu real %llu init %llu", i, type, nk_rd64(a + 40), nk_rd64(a + 48), nk_rd64(a + 56));
                    if (total && nk_rd64(a + 24) != start + total - 1) NBAD("record %u: 0x%X highest VCN %llu for %llu clusters from %llu", i, type, nk_rd64(a + 24), total, start);
                }
            }
            o += len;
        }
        if (!ended) NBAD("record %u: attributes are not terminated", i);
        if (nk_strict && i >= 24 && nk_rd16(r + 18) != nfn_nodos) NBAD("record %u: link count %u but %u names", i, nk_rd16(r + 18), nfn_nodos);
        if (nk_rd16(r + 22) & 2) { dirs_rec[i] = malloc(nk_rs); memcpy(dirs_rec[i], r, nk_rs); }
    }

    /* the $MFTMirr copy */
    if (mirr_lcn && mirr_lcn < nclusters) {
        NU8 raw[4096];
        for (unsigned i = 0; i < 4 && (i + 1) * nk_rs <= 4096; i++) {
            nk_read(&nk_mft, (unsigned long long)i * nk_rs, raw, nk_rs);
            if (memcmp(raw, img + mirr_lcn * nk_cs + (unsigned long long)i * nk_rs, nk_rs) != 0 && nk_strict) NBAD("$MFTMirr: record %u differs from the MFT", i);
        }
    }

    /* the cluster bitmap */
    {
        int st = nk_rec(6, r);
        const NU8 *bd = st == 1 ? nk_attr_find(r, 0x80, 0) : 0;
        if (!bd || !bd[8]) NBAD("$Bitmap has no data runs");
        else {
            unsigned long long bytes = (nclusters + 7) / 8;
            NU8 *bm = malloc((size_t)bytes);
            if (nk_stream_bytes(bd, bm, 0, (unsigned)bytes)) NBAD("$Bitmap cannot be read");
            else for (unsigned long long c = 0; c < nclusters; c++) {
                int bit = (bm[c / 8] >> (c % 8)) & 1;
                if (nk_own[c] == 1 && !bit) NBAD("cluster %llu is owned by a file but free in $Bitmap", c);
                if (nk_own[c] == 0 && bit && nk_strict) NBAD("cluster %llu is marked used but nobody owns it", c);
            }
            free(bm);
        }
    }

    /* the folders */
    for (unsigned i = 0; i < nk_nrec; i++) if (dirs_rec[i]) { nk_dir(i, dirs_rec[i]); }

    /* names in records <-> entries in indexes */
    for (int e = 0; e < nk_nents; e++) {
        NkEnt *x = &nk_ents[e];
        if (x->rec >= nk_nrec || !nk_inuse[x->rec]) { NBAD("dir %u lists record %u which is not in use", x->dir, x->rec); continue; }
        if (x->seq != nk_seqs[x->rec]) NBAD("dir %u: entry for record %u has sequence %u, the record has %u", x->dir, x->rec, x->seq, nk_seqs[x->rec]);
        if ((unsigned)(nk_rd64(x->key) & 0xFFFFFFFFFFFFULL) != x->dir) NBAD("dir %u: entry for record %u names parent %llu", x->dir, x->rec, nk_rd64(x->key) & 0xFFFFFFFFFFFFULL);
        int match = -1;
        for (int n = 0; n < nk_nnames; n++) {
            NkName *nm = &nk_names[n];
            if (nm->rec == x->rec && nm->key[64] == x->key[64] && memcmp(nm->key + 65, x->key + 65, 1 + 2u * x->key[64]) == 0 && nm->klen == x->klen) { match = n; break; }
        }
        if (match < 0) { if (nk_strict) NBAD("dir %u: entry for record %u has no matching $FILE_NAME in the record", x->dir, x->rec); }
        else {
            NkName *nm = &nk_names[match];
            if (nk_strict && memcmp(nm->key, x->key, x->klen) != 0) NBAD("dir %u: entry for record %u differs from its $FILE_NAME (times / sizes / parent)", x->dir, x->rec);
            nm->nattr++;
        }
    }
    if (nk_strict) for (int n = 0; n < nk_nnames; n++) {
        NkName *nm = &nk_names[n];
        unsigned parent = (unsigned)(nk_rd64(nm->key) & 0xFFFFFFFFFFFFULL);
        if (!nm->nattr) NBAD("record %u: its name '%c...' is not in the index of folder %u", nm->rec, nm->key[66], parent);
        if (parent >= nk_nrec || !dirs_rec[parent]) NBAD("record %u: parent %u is not a folder", nm->rec, parent);
        else if ((nk_rd64(nm->key) >> 48) != nk_seqs[parent]) NBAD("record %u: parent reference has sequence %llu, folder %u has %u", nm->rec, nk_rd64(nm->key) >> 48, parent, nk_seqs[parent]);
    }

    /* the volume must be clean: dirty bit off */
    {
        int st = nk_rec(3, r);
        const NU8 *vi = st == 1 ? nk_attr_find(r, 0x70, 0) : 0;
        if (!vi) NBAD("$Volume has no $VOLUME_INFORMATION");
        else {
            unsigned fl = nk_rd16(vi + nk_rd16(vi + 20) + 10);
            if (strict && (fl & 1)) NBAD("the volume is marked dirty");
            if (!strict && !(fl & 1)) { /* may be clean: the change finished, or nothing was written */ }
        }
    }
    for (unsigned i = 0; i < nk_nrec; i++) free(dirs_rec[i]), dirs_rec[i] = 0;
    free(r); free(rec); free(mbm); free(nk_own); free(nk_inuse);
    return nck_errors;
}

#endif
