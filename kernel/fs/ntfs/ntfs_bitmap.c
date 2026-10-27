/* NTFS write side: the cluster bitmap and the MFT bitmap. */
#include "ntfs_internal.h"

/* ---- the cluster bitmap ----------------------------------------------- */
static u8 BW[4096];
static u64 BW_BASE = NONE;

int bm_load(Ntfs *nt)
{
    BW_BASE = NONE;
    if (load_stream(nt, 6, AT_DATA, 0, &S_BM) != 0 || !S_BM.found || S_BM.resident || S_BM.size * 8 < nt->clusters)
        return -1;
    return 0;
}

/* bit of cluster c: 0, 1 or -1 */
int bm_get(Ntfs *nt, u64 c)
{
    u64 byte = c / 8, base = byte & ~4095ULL;
    if (BW_BASE != base)
    {
        u64 n = S_BM.size - base;
        if (n > sizeof BW)
            n = sizeof BW;
        if (runs_read(nt, &S_BM.runs, base, BW, (u32)n))
            return -1;
        BW_BASE = base;
    }
    return (BW[byte - base] >> (c & 7)) & 1;
}

/* set clusters [c, c + n) to val */
static int bm_range(Ntfs *nt, u64 c, u64 n, int val)
{
    BW_BASE = NONE;
    u8 buf[512];
    while (n)
    {
        u64 first = c / 8, last = (c + n - 1) / 8;
        u32 nb = (u32)(last - first + 1 > sizeof buf ? sizeof buf : last - first + 1);
        if (runs_read(nt, &S_BM.runs, first, buf, nb))
            return -1;
        while (n && c / 8 < first + nb)
        {
            if (val)
                buf[c / 8 - first] |= (u8)(1u << (c & 7));
            else
                buf[c / 8 - first] &= (u8)~(1u << (c & 7));
            c++;
            n--;
        }
        if (runs_write(nt, &S_BM.runs, first, buf, nb))
        {
            nt->poisoned = 1;
            return -1;
        }
    }
    return 0;
}

/* Take `want` clusters, in as few runs as possible, starting the search at `hint`. The runs are appended to out
 * (joined to its last run when they touch it) and marked used. On failure nothing is taken: NTFS_ERR_FULL. */
int alloc_clusters(Ntfs *nt, u64 want, u64 hint, NtRuns *out)
{
    static NtRuns got;
    got.n = 0;
    u64 need = want, c = hint < nt->clusters ? hint : 0, passed = 0;
    while (need && passed < nt->clusters)
    {
        if (c >= nt->clusters)
            c = 0;
        int bit = bm_get(nt, c);
        if (bit < 0)
            return NTFS_ERR_IO;
        if (bit)
        {
            c++;
            passed++;
            continue;
        }
        u64 s = c, len = 0;
        while (len < need && c < nt->clusters && passed < nt->clusters)
        {
            bit = bm_get(nt, c);
            if (bit < 0)
                return NTFS_ERR_IO;
            if (bit)
                break;
            c++;
            len++;
            passed++;
        }
        if (got.n >= NT_MAX_RUNS)
            return NTFS_ERR_FULL;
        got.r[got.n].vcn = 0;
        got.r[got.n].len = len;
        got.r[got.n].lcn = (s64)s;
        got.n++;
        need -= len;
    }
    if (need)
        return NTFS_ERR_FULL;
    if (out->n + got.n > NT_MAX_RUNS) /* would not fit in the list: nothing is marked yet, so nothing to undo */
        return NTFS_ERR_FULL;
    for (int i = 0; i < got.n; i++)
        runs_append(out, (u64)got.r[i].lcn, got.r[i].len);
    for (int i = 0; i < got.n; i++)
        if (bm_range(nt, (u64)got.r[i].lcn, got.r[i].len, 1))
            return NTFS_ERR_IO;
    nt->hint = (u64)got.r[got.n - 1].lcn + got.r[got.n - 1].len;
    if (nt->free_clusters != NONE)
        nt->free_clusters -= want;
    return 0;
}

/* give the clusters of runs [from, n) back */
int free_runs(Ntfs *nt, const NtRuns *rs, int from)
{
    for (int i = from; i < rs->n; i++)
        if (rs->r[i].lcn >= 0)
        {
            if (bm_range(nt, (u64)rs->r[i].lcn, rs->r[i].len, 0))
                return -1;
            if (nt->free_clusters != NONE)
                nt->free_clusters += rs->r[i].len;
        }
    return 0;
}

/* ---- the MFT bitmap ------------------------------------------------------ */
int mb_load(Ntfs *nt)
{
    if (load_stream(nt, 0, AT_BITMAP, 0, &S_MB) != 0 || !S_MB.found)
        return -1;
    return 0;
}

static int mb_get(Ntfs *nt, u32 recno)
{
    u8 b;
    if ((u64)recno / 8 >= S_MB.size || stream_read(nt, &S_MB, recno / 8, &b, 1) != 1)
        return -1;
    return (b >> (recno & 7)) & 1;
}

int mb_set(Ntfs *nt, u32 recno, int val)
{
    u8 b;
    if ((u64)recno / 8 >= S_MB.size || stream_read(nt, &S_MB, recno / 8, &b, 1) != 1)
        return -1;
    if (val)
        b |= (u8)(1u << (recno & 7));
    else
        b &= (u8)~(1u << (recno & 7));
    if (!S_MB.resident)
    {
        if (runs_write(nt, &S_MB.runs, recno / 8, &b, 1))
        {
            nt->poisoned = 1;
            return -1;
        }
        return 0;
    }
    /* resident in record 0: patch it there */
    if (record_read(nt, 0, MREC))
        return -1;
    u8 *a = rec_find(MREC, nt->rec_size, AT_BITMAP, 0);
    if (!a || a[8] != 0 || recno / 8 >= rd32(a + 16))
        return -1;
    a[rd16(a + 20) + recno / 8] = b;
    S_MB.res[recno / 8] = b;
    return rec_store(nt, 0, MREC);
}

/* the first free record from 24 up (0..23 are the system's) */
int mb_find_free(Ntfs *nt, u32 *recno)
{
    u64 total = nt->mft_size / nt->rec_size;
    if (total > S_MB.size * 8)
        total = S_MB.size * 8;
    for (u64 r = 24; r < total; r++)
    {
        int bit = mb_get(nt, (u32)r);
        if (bit < 0)
            return NTFS_ERR_IO;
        if (!bit)
        {
            *recno = (u32)r;
            return 0;
        }
    }
    return NTFS_ERR_FULL;
}
