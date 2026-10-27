/* FAT32: reading and writing file data. */
#include "fat32_internal.h"

/* ---- reading and writing file data ------------------------------ */
/* bytes [pos, pos+n) of one cluster, in sectors: partial first / last sector through f->sec, the middle directly */
static int cluster_io(Fat *f, u32 cl, u32 pos, u8 *buf, u32 n, int write)
{
    u64 lba = cluster_lba(f, cl) + pos / f->ss;
    u32 head = pos % f->ss;
    while (n)
    {
        if (head == 0 && n >= f->ss)
        {
            u32 whole = n / f->ss;
            int r = write ? f32_wr_sectors(f, lba, whole, buf) : f32_rd_sectors(f, lba, whole, buf);
            if (r)
                return r;
            buf += (u64)whole * f->ss;
            n -= whole * f->ss;
            lba += whole;
            continue;
        }
        u32 take = f->ss - head < n ? f->ss - head : n;
        if (write)
        {
            if (f32_rd_sectors(f, lba, 1, f->sec))
                return FAT_ERR_IO;
            copy(f->sec + head, buf, take);
            int r = f32_wr_sectors(f, lba, 1, f->sec);
            if (r)
                return r;
        }
        else
        {
            if (f32_rd_sectors(f, lba, 1, f->sec))
                return FAT_ERR_IO;
            copy(buf, f->sec + head, take);
        }
        buf += take;
        n -= take;
        lba++;
        head = 0;
    }
    return 0;
}

/* read or write `len` bytes of a chain starting at byte `offset`; the chain must be long enough */
int f32_chain_io(Fat *f, u32 first, u32 offset, u8 *buf, u32 len, int write)
{
    u32 csize = f->spc * f->ss;
    u32 cl;
    int r = f32_chain_at(f, first, offset / csize, &cl);
    if (r <= 0)
        return r < 0 ? r : FAT_ERR_IO;
    u32 pos = offset % csize;
    for (;;)
    {
        u32 n = csize - pos < len ? csize - pos : len;
        r = cluster_io(f, cl, pos, buf, n, write);
        if (r)
            return r;
        buf += n;
        len -= n;
        if (!len)
            return 0;
        pos = 0;
        u32 next;
        r = f32_fat_get(f, cl, &next);
        if (r)
            return r;
        if (!valid_cluster(f, next))
            return FAT_ERR_IO;
        cl = next;
        f->cur_first = first; /* keep the cursor with us */
        f->cur_index = f->cur_index + 1;
        f->cur_cluster = cl;
    }
}

int fat_read(Fat *f, const FatEntry *e, unsigned offset, void *buf, unsigned len)
{
    if (offset >= e->size || len == 0)
        return 0;
    if (len > e->size - offset)
        len = e->size - offset;
    if (e->first_cluster < 2)
        return 0;
    int r = f32_chain_io(f, e->first_cluster, offset, (u8 *)buf, len, 0);
    return r ? r : (int)len;
}
