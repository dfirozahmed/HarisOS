/* NTFS write side: records of files (names, sizes) and creating files and folders. */
#include "ntfs_internal.h"

/* ---- records of files: names, sizes ----------------------------------------------- */
u8 KNEW[600], ENT[640], ATMP[1024];
u8 KOLD[MAX_NAMES][600];
u8 RESV[4096], RESN[4096];
NtRuns FREED, DSR;

u32 make_entry(u8 *out, u64 ref, const u8 *key, u32 klen)
{
    u32 len = al8(16 + klen);
    zero(out, len);
    wr64(out, ref);
    wr16(out + 8, len);
    wr16(out + 10, klen);
    copy(out + 16, key, klen);
    return len;
}

/* the resident $FILE_NAME attributes of a record: how many there are; the first `max` are put in out */
int fn_attrs(u8 *rec, u32 rs, u8 **out, int max)
{
    int n = 0;
    for (const u8 *a = attr_first(rec, rs); a && rd32(a) != AT_END; a = attr_next(rec, rs, a))
        if (rd32(a) == AT_FILE_NAME && a[8] == 0 && rd32(a + 16) >= 66 && (u32)rd16(a + 20) + rd32(a + 16) <= rd32(a + 4))
        {
            if (n < max)
                out[n] = (u8 *)a;
            n++;
        }
    return n;
}
u8 *attr_val(u8 *a) { return a + rd16(a + 20); }

/* allocated and real size of what the record holds: a file's $DATA, a folder's $INDEX_ALLOCATION */
static void data_sizes(u8 *rec, u32 rs, u64 *alloc, u64 *real)
{
    u8 *a = (rd16(rec + 22) & 2) ? rec_find(rec, rs, AT_INDEX_ALLOCATION, "$I30") : rec_find(rec, rs, AT_DATA, 0);
    *alloc = *real = 0;
    if (!a)
        return;
    if (a[8] == 0)
    {
        *real = rd32(a + 16);
        *alloc = al8((u32)*real);
    }
    else
    {
        *alloc = rd64(a + 40);
        *real = rd64(a + 48);
    }
}

/* copy times and sizes into every $FILE_NAME of the record */
void names_refresh(Ntfs *nt, u8 *rec)
{
    u32 rs = nt->rec_size;
    u8 *si = rec_find(rec, rs, AT_STANDARD_INFORMATION, 0);
    u64 alloc, real;
    data_sizes(rec, rs, &alloc, &real);
    u8 *fa[MAX_NAMES];
    int n = fn_attrs(rec, rs, fa, MAX_NAMES);
    for (int i = 0; i < n && i < MAX_NAMES; i++)
    {
        u8 *v = attr_val(fa[i]);
        if (si && si[8] == 0 && rd32(si + 16) >= 32)
        {
            const u8 *s = attr_val(si);
            copy(v + 8, s, 8);       /* created */
            copy(v + 16, s + 8, 8);  /* modified */
            copy(v + 24, s + 16, 8); /* MFT changed */
            copy(v + 32, s + 24, 8); /* accessed */
        }
        wr64(v + 40, alloc);
        wr64(v + 48, real);
    }
}

/* the same times and sizes in the index entries of the folders that list the file */
int names_index_sync(Ntfs *nt, u8 *rec)
{
    u8 *fa[MAX_NAMES];
    int n = fn_attrs(rec, nt->rec_size, fa, MAX_NAMES);
    for (int i = 0; i < n && i < MAX_NAMES; i++)
    {
        u8 *v = attr_val(fa[i]);
        int r = idx_open(nt, (u32)(rd64(v) & REF_MASK));
        if (r == 0)
            r = idx_update(nt, v, v);
        if (r == 0)
            r = idx_close(nt, 1, 0);
        if (r)
            return r;
    }
    return 0;
}

/* free the clusters of every non-resident attribute of the record */
int free_record_clusters(Ntfs *nt, u8 *rec)
{
    static NtRuns rs;
    for (const u8 *a = attr_first(rec, nt->rec_size); a && rd32(a) != AT_END; a = attr_next(rec, nt->rec_size, a))
        if (a[8] != 0 && rd64(a + 16) == 0)
        {
            rs.n = 0;
            if (parse_runs(a + rd16(a + 32), a + rd32(a + 4), 0, &rs))
                return -1;
            if (free_runs(nt, &rs, 0))
                return -1;
        }
    return 0;
}

/* Enough free clusters for the blocks one index change can need: a split takes a block per level (levels), and a
 * folder without allocation takes one when its root overflows. 0 or an error. */
int index_room(Ntfs *nt, int levels)
{
    u64 need = (u64)(levels + 1) * (nt->idx_size >= nt->csize ? nt->idx_size / nt->csize : 1);
    u64 found = 0, c = nt->hint < nt->clusters ? nt->hint : 0;
    for (u64 seen = 0; seen < nt->clusters && found < need; seen++, c++)
    {
        if (c >= nt->clusters)
            c = 0;
        int bit = bm_get(nt, c);
        if (bit < 0)
            return NTFS_ERR_IO;
        if (!bit)
            found++;
    }
    return found >= need ? 0 : NTFS_ERR_FULL;
}

/* open folder `dir` and see whether changing the name `key` in it has the room it may need */
int parent_room(Ntfs *nt, u32 dir, const u8 *key)
{
    int r = idx_open(nt, dir);
    if (r)
        return r;
    int found;
    r = idx_locate(nt, key, &found);
    if (r)
        return r;
    return index_room(nt, IX.has_ia ? PDEPTH + 1 : 0);
}

/* ---- the security id of a new file in a folder whose record has none ----------------------------
 * Every file points to a security descriptor (who may read / write it) by a number, the "security id" in its
 * $STANDARD_INFORMATION; a new file gets the id of the folder it is made in. The $STANDARD_INFORMATION of some
 * folders - the root of many Windows volumes - is the old 48-byte form without that field. Such a folder's
 * descriptor lives elsewhere and finding or making an entry for it in $Secure is not done here. Instead the new
 * file gets the id of an ordinary file (else folder) that already lies in the same folder: Windows gave that
 * one the permissions it inherits from this folder. System, hidden, encrypted and link entries are skipped
 * (they often have special permissions). The result is a plausible, valid id - but not necessarily the exact
 * descriptor Windows itself would have chosen for the new file. */
#define SIB_MAX 48
static u32 SIB_REC[SIB_MAX];
static u8 SIB_DIR[SIB_MAX];
static int SIB_N;
static u8 SIBREC[4096];

static int sib_collect(const char *name, const NtEntry *e, void *user)
{
    (void)user;
    if (name[0] == '$' || e->recno < 24)
        return 0;
    SIB_REC[SIB_N] = e->recno;
    SIB_DIR[SIB_N++] = e->is_dir;
    return SIB_N >= SIB_MAX; /* enough candidates: stop listing */
}

static int sibling_sid(Ntfs *nt, const NtEntry *dir, int is_dir, u32 *sid)
{
    SIB_N = 0;
    ntfs_list(nt, dir, sib_collect, 0); /* it only collects: the volume is not touched from inside the callback */
    u32 other = 0;
    for (int i = 0; i < SIB_N; i++)
    {
        if (record_read(nt, SIB_REC[i], SIBREC) || rd64(SIBREC + 32)) /* unreadable, or not a base record */
            continue;
        u8 *si = rec_find(SIBREC, nt->rec_size, AT_STANDARD_INFORMATION, 0);
        if (!si || si[8] != 0 || rd32(si + 16) < 72)
            continue;
        u32 attrs = rd32(attr_val(si) + 32), s = rd32(attr_val(si) + 52);
        if (!s || (attrs & (0x2 | 0x4 | 0x400 | 0x4000))) /* hidden, system, reparse point, encrypted */
            continue;
        if ((SIB_DIR[i] != 0) == (is_dir != 0))
        {
            *sid = s;
            return 0;
        }
        if (!other)
            other = s;
    }
    if (other)
    {
        *sid = other;
        return 0;
    }
    return unsup_why(__LINE__, "no ordinary file here to copy permissions from");
}

/* ---- create ------------------------------------------------------------------------- */
static int create_inner(Ntfs *nt, const NtEntry *dir, const char *name, int is_dir, NtEntry *out)
{
    u32 rs = nt->rec_size;
    int r = idx_open(nt, dir->recno);
    if (r)
        return r;
    u8 *si = rec_find(DREC, rs, AT_STANDARD_INFORMATION, 0);
    if (!si || si[8] != 0)
        return unsup(__LINE__);
    u32 sid;
    if (rd32(si + 16) >= 72)
        sid = rd32(attr_val(si) + 52);
    else if ((r = sibling_sid(nt, dir, is_dir, &sid)) != 0) /* the old 48-byte form: see above */
        return r;
    u64 now = now_ft();
    u32 klen = make_key(KNEW, (u64)dir->recno | (u64)rd16(DREC + 16) << 48, name, now, 0, 0, is_dir ? 0x10000000u : 0x20u, 1);
    int found;
    r = idx_locate(nt, KNEW, &found);
    if (r)
        return r;
    if (found)
        return NTFS_ERR_EXISTS;
    r = index_room(nt, IX.has_ia ? PDEPTH : 0);
    if (r)
        return r;
    u32 recno;
    r = mb_find_free(nt, &recno);
    if (r)
        return r;
    u32 seq = rec_slot_seq(nt, recno);
    if (!seq)
        seq = 1;

    /* the new record: header, $STANDARD_INFORMATION, $FILE_NAME, and an empty $DATA or an empty index */
    u32 cnt = rs / 512 + 1, first = al8(0x30 + 2 * cnt);
    zero(FREC, rs);
    copy(FREC, "FILE", 4);
    wr16(FREC + 4, 0x30);
    wr16(FREC + 6, cnt);
    wr16(FREC + 16, seq);
    wr16(FREC + 18, 1);
    wr16(FREC + 20, first);
    wr16(FREC + 22, is_dir ? 3 : 1);
    wr32(FREC + 24, first + 8);
    wr32(FREC + 28, rs);
    wr32(FREC + 44, recno);
    wr16(FREC + 0x30, 1);
    wr32(FREC + first, AT_END);
    u8 v[72];
    zero(v, sizeof v);
    for (int i = 0; i < 4; i++)
        wr64(v + 8 * i, now);
    wr32(v + 32, is_dir ? 0x10 : 0x20);
    wr32(v + 52, sid); /* the security descriptor of the folder it is made in */
    build_res(ATMP, AT_STANDARD_INFORMATION, 0, v, sizeof v, 0);
    if (!rec_insert(FREC, rs, ATMP))
        return NTFS_ERR_FULL;
    build_res(ATMP, AT_FILE_NAME, 0, KNEW, klen, 1); /* 1 = indexed */
    if (!rec_insert(FREC, rs, ATMP))
        return NTFS_ERR_FULL;
    if (is_dir)
    {
        u8 root[48];
        zero(root, sizeof root);
        wr32(root, AT_FILE_NAME);
        wr32(root + 4, 1); /* collation: file names */
        wr32(root + 8, nt->idx_size);
        u32 sh = 0;
        while ((1u << sh) < nt->idx_size)
            sh++;
        root[12] = nt->idx_size >= nt->csize ? (u8)(nt->idx_size / nt->csize) : (u8)(256 - sh);
        wr32(root + 16, 16); /* index header: entries at +16, used 32, allocated 32, no sub-nodes */
        wr32(root + 20, 32);
        wr32(root + 24, 32);
        wr16(root + 32 + 8, 16); /* the end entry */
        wr16(root + 32 + 12, 2);
        build_res(ATMP, AT_INDEX_ROOT, "$I30", root, sizeof root, 0);
    }
    else
        build_res(ATMP, AT_DATA, 0, 0, 0, 0);
    if (!rec_insert(FREC, rs, ATMP))
        return NTFS_ERR_FULL;

    /* bitmap, record, then the index */
    if (mb_set(nt, recno, 1) || rec_store(nt, recno, FREC))
        return NTFS_ERR_IO;
    u32 elen = make_entry(ENT, (u64)recno | (u64)seq << 48, KNEW, klen);
    IDX_WROTE = 0;
    r = idx_add(nt, ENT, elen);
    if (r == 0)
        r = idx_close(nt, 1, now);
    if (r)
    {
        if (IDX_WROTE || nt->poisoned)
        {
            nt->poisoned = 1; /* the record exists but is not listed: chkdsk has to look */
            return r;
        }
        /* a refusal before anything of the index was written (no room to grow the folder ...): take the new record back */
        u32 s = (rd16(FREC + 16) + 1u) & 0xFFFF;
        wr16(FREC + 22, 0);
        wr16(FREC + 16, s ? s : 1);
        if (rec_store(nt, recno, FREC) || mb_set(nt, recno, 0))
            nt->poisoned = 1;
        return r;
    }
    out->recno = recno;
    out->size = 0;
    out->is_dir = is_dir != 0;
    out->pad[0] = out->pad[1] = out->pad[2] = 0;
    return 0;
}

int ntfs_create(Ntfs *nt, const NtEntry *dir, const char *name, int is_dir, NtEntry *out)
{
    if (!dir || !out)
        return NTFS_ERR_ARG;
    int r = name_ok(name);
    if (r)
        return r;
    r = wr_begin(nt);
    if (r)
        return r;
    return wr_end(nt, create_inner(nt, dir, name, is_dir, out));
}
