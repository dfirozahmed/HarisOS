/* exFAT: reading data. */
#include "exfat_internal.h"

/* ---- reading data ------------------------------------------------- */
/* bytes [pos, pos+n) of one cluster: the partial first / last sector through the cache, whole sectors directly */
int exf_cluster_read(ExFat *f, u32 cl, u32 pos, u8 *buf, u32 n)
{
    u64 lba = cluster_lba(f, cl) + pos / f->ss;
    u32 head = pos % f->ss;
    while (n)
    {
        if (head == 0 && n >= f->ss)
        {
            u32 whole = n / f->ss;
            if (blk_read(f->blk, lba, whole, buf) != 0)
                return -1;
            buf += (u64)whole * f->ss;
            n -= whole * f->ss;
            lba += whole;
            continue;
        }
        if (exf_load_sec(f, lba))
            return -1;
        u32 take = f->ss - head < n ? f->ss - head : n;
        copy(buf, f->sec + head, take);
        buf += take;
        n -= take;
        lba++;
        head = 0;
    }
    return 0;
}

/* read `len` bytes of a cluster run starting at byte `offset` of the data (the data must be long enough) */
static int data_read(ExFat *f, u32 first, int nofat, u32 offset, u8 *buf, u32 len)
{
    u32 csize = f->spc * f->ss;
    u32 cl, index = offset / csize, pos = offset % csize;
    int r = exf_cluster_at(f, first, nofat, index, &cl);
    if (r <= 0)
        return -1;
    for (;;)
    {
        u32 n = csize - pos < len ? csize - pos : len;
        if (exf_cluster_read(f, cl, pos, buf, n))
            return -1;
        buf += n;
        len -= n;
        if (!len)
            return 0;
        pos = 0;
        index++;
        r = exf_cluster_at(f, first, nofat, index, &cl);
        if (r <= 0)
            return -1;
    }
}

int exfat_read(ExFat *f, const ExEntry *e, unsigned offset, void *buf, unsigned len)
{
    if (offset >= e->size || len == 0 || e->first_cluster < 2)
        return 0;
    if (len > e->size - offset)
        len = e->size - offset;
    u32 real = offset < e->valid ? (e->valid - offset < len ? e->valid - offset : len) : 0;
    if (real && data_read(f, e->first_cluster, e->nofat, offset, (u8 *)buf, real))
        return -1;
    zero((u8 *)buf + real, len - real); /* beyond ValidDataLength: zeros */
    return (int)len;
}
