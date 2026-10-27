/* exFAT write side: clusters of a file, the up-case table. */
#include "exfat_internal.h"

/* ---- clusters of a file: count, chain, append, trim ---- */
u32 exf_clusters_of(const ExFat *f, u32 size) { return (u32)(((u64)size + f->spc * f->ss - 1) / (f->spc * f->ss)); }

/* give a "NoFatChain" run of n clusters a FAT chain (so that it can grow or shrink anywhere) */
static int to_chain(ExFat *f, u32 first, u32 n)
{
    for (u32 i = 0; i < n; i++)
    {
        int r = exf_fat_put(f, first + i, i + 1 < n ? first + i + 1 : 0xFFFFFFFFu);
        if (r)
            return r;
    }
    exf_forget_cursor(f);
    return 0;
}

static int zero_cluster(ExFat *f, u32 cl)
{
    static const u8 z[EXFAT_SECTOR_MAX]; /* all zeros (static: a 4 KiB array on the stack would need __chkstk) */
    for (u32 s = 0; s < f->spc; s++)
    {
        int r = exf_wr_sector(f, cluster_lba(f, cl) + s, z);
        if (r)
            return r;
    }
    f->sec_lba = NONE;
    return 0;
}

/* One more cluster at the end of a stream of *n clusters. Updates *first / *nofat / *n. */
int exf_append_cluster(ExFat *f, u32 *first, u8 *nofat, u32 *n, int zero_it)
{
    u32 c;
    int r;
    if (*n == 0)
    {
        r = exf_alloc_cluster(f, 0, &c);
        if (r)
            return r;
        r = exf_fat_put(f, c, 0xFFFFFFFFu);
        if (r)
            return r;
        *first = c;
        *nofat = 0;
    }
    else
    {
        u32 last;
        r = exf_cluster_at(f, *first, *nofat, *n - 1, &last);
        if (r <= 0)
            return EXFAT_ERR_IO;
        int next_used = 1;
        if (*nofat && valid_cluster(f, last + 1) && exf_bm_get(f, last + 1, &next_used) == 0 && !next_used)
        {
            r = exf_bm_put(f, last + 1, 1); /* the clusters after it are free: it grows in place */
            if (r)
                return r;
            c = last + 1;
        }
        else
        {
            if (*nofat)
            {
                r = to_chain(f, *first, *n);
                if (r)
                    return r;
                *nofat = 0;
            }
            r = exf_alloc_cluster(f, last + 1, &c);
            if (r)
                return r;
            r = exf_fat_put(f, last, c);
            if (r)
                return r;
            r = exf_fat_put(f, c, 0xFFFFFFFFu);
            if (r)
                return r;
        }
    }
    (*n)++;
    exf_forget_cursor(f);
    if (zero_it)
        return zero_cluster(f, c);
    return 0;
}

/* free the clusters of a stream from index `keep` on (keep = 0: all of them) */
int exf_trim_clusters(ExFat *f, u32 *first, u8 nofat, u32 n, u32 keep)
{
    if (n <= keep || !valid_cluster(f, *first))
        return 0;
    int r;
    if (nofat)
    {
        for (u32 i = keep; i < n; i++)
        {
            r = exf_bm_put(f, *first + i, 0);
            if (r)
                return r;
        }
    }
    else
    {
        u32 c, tail = 0;
        if (keep)
        {
            r = exf_cluster_at(f, *first, 0, keep - 1, &tail);
            if (r <= 0)
                return EXFAT_ERR_IO;
            if (exf_fat_get(f, tail, &c))
                return EXFAT_ERR_IO;
            r = exf_fat_put(f, tail, 0xFFFFFFFFu);
            if (r)
                return r;
        }
        else
            c = *first;
        for (u32 guard = 0; valid_cluster(f, c) && guard <= f->clusters; guard++)
        {
            u32 next;
            if (exf_fat_get(f, c, &next))
                return EXFAT_ERR_IO;
            r = exf_bm_put(f, c, 0);
            if (r)
                return r;
            r = exf_fat_put(f, c, 0);
            if (r)
                return r;
            if (next >= EOC)
                break;
            c = next;
        }
    }
    if (keep == 0)
        *first = 0;
    exf_forget_cursor(f);
    return 0;
}

/* ---- the up-case table (for the name hash) ---- */
static u16 UPCASE[65536];
static const ExFat *UPCASE_FOR;

static void load_upcase(ExFat *f)
{
    if (UPCASE_FOR == f)
        return;
    for (u32 i = 0; i < 65536; i++)
        UPCASE[i] = (u16)(i >= 'a' && i <= 'z' ? i - 32 : i); /* the ASCII fallback */
    UPCASE_FOR = f;
    if (!valid_cluster(f, f->upcase_cluster) || f->upcase_bytes < 4 || f->upcase_bytes > 131072 + 16)
        return;
    u32 csize = f->spc * f->ss, idx = 0, done = 0;
    u16 pending_skip = 0;
    int have_ff = 0;
    u8 buf[512];
    while (done < f->upcase_bytes && idx < 65536)
    {
        u32 n = f->upcase_bytes - done < sizeof buf ? f->upcase_bytes - done : (u32)sizeof buf;
        n &= ~1u;
        if (!n)
            break;
        u32 cl, ci = done / csize;
        if (exf_cluster_at(f, f->upcase_cluster, 1, ci, &cl) <= 0 || exf_cluster_read(f, cl, done % csize, buf, n))
            return;
        for (u32 i = 0; i + 1 < n && idx < 65536; i += 2)
        {
            u16 v = rd16(buf + i);
            if (have_ff)
            {
                have_ff = 0;
                pending_skip = v; /* that many characters map to themselves */
                idx += pending_skip;
            }
            else if (v == 0xFFFF)
                have_ff = 1;
            else
                UPCASE[idx++] = v;
        }
        done += n;
    }
}

u16 exf_name_hash(ExFat *f, const char *name)
{
    load_upcase(f);
    u32 h = 0;
    for (; *name; name++)
    {
        u16 u = UPCASE[(u8)*name];
        h = ((h << 15) | (h >> 1)) & 0xFFFF;
        h = (h + (u & 0xFF)) & 0xFFFF;
        h = ((h << 15) | (h >> 1)) & 0xFFFF;
        h = (h + (u >> 8)) & 0xFFFF;
    }
    return (u16)h;
}

u16 exf_set_checksum(const u8 *set, u32 n)
{
    u32 cs = 0;
    for (u32 i = 0; i < n * 32; i++)
    {
        if (i == 2 || i == 3)
            continue;
        cs = (((cs & 1) ? 0x8000 : 0) + (cs >> 1) + set[i]) & 0xFFFF;
    }
    return (u16)cs;
}

void exf_now(u32 *stamp)
{
    unsigned d = (1u << 5) | 1u, t = 0; /* 1980-01-01 00:00:00 */
    if (exf_CLOCK)
        exf_CLOCK(&d, &t);
    *stamp = ((u32)d << 16) | (t & 0xFFFF);
}
