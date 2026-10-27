/* FAT32: the FAT (cluster chains, allocation). */
#include "fat32_internal.h"

/* ---- the FAT ---------------------------------------------------- */
static int fat_flush_cache(Fat *f)
{
    if (!f->fat_dirty)
        return 0;
    u32 rel = (u32)f->fat_buf_lba;
    for (unsigned k = 0; k < f->nfats; k++)
    {
        u64 lba = f->fat0_lba + (u64)k * f->fat_sectors + rel;
        if (!f->mirror && lba != f->fat_lba + rel)
            continue; /* only the active FAT is kept up to date */
        int r = f32_wr_sectors(f, lba, 1, f->fat_buf);
        if (r)
            return r;
    }
    f->fat_dirty = 0;
    return 0;
}

static int fat_load(Fat *f, u32 rel)
{
    if (f->fat_buf_lba == rel)
        return 0;
    int r = fat_flush_cache(f);
    if (r)
        return r;
    r = f32_rd_sectors(f, f->fat_lba + rel, 1, f->fat_buf);
    if (r)
    {
        f->fat_buf_lba = ~0ULL;
        return r;
    }
    f->fat_buf_lba = rel;
    return 0;
}

int f32_fat_get(Fat *f, u32 cl, u32 *out)
{
    u32 off = cl * 4;
    int r = fat_load(f, off / f->ss);
    if (r)
        return r;
    *out = rd32(f->fat_buf + off % f->ss) & FAT_MASK;
    return 0;
}

int f32_fat_set(Fat *f, u32 cl, u32 value)
{
    u32 off = cl * 4;
    int r = fat_load(f, off / f->ss);
    if (r)
        return r;
    u8 *p = f->fat_buf + off % f->ss;
    wr32(p, (rd32(p) & ~FAT_MASK) | (value & FAT_MASK)); /* the top 4 bits are reserved: keep them */
    f->fat_dirty = 1;
    return 0;
}

/* The index-th cluster of a chain (0 = the first). Returns 1 and the cluster, 0 if the chain ends before it,
 * or an error. A cursor remembers the last position so that reading or writing a file in order is quick. */
int f32_chain_at(Fat *f, u32 first, u32 index, u32 *out)
{
    if (!valid_cluster(f, first))
        return 0;
    u32 c = first, i = 0;
    if (f->cur_first == first && f->cur_index <= index && valid_cluster(f, f->cur_cluster))
    {
        c = f->cur_cluster;
        i = f->cur_index;
    }
    while (i < index)
    {
        u32 next;
        int r = f32_fat_get(f, c, &next);
        if (r)
            return r;
        if (next >= EOC)
            return 0;
        if (!valid_cluster(f, next))
            return FAT_ERR_IO; /* free (0) or bad cluster in the middle of a chain: damaged */
        c = next;
        i++;
    }
    f->cur_first = first;
    f->cur_index = i;
    f->cur_cluster = c;
    *out = c;
    return 1;
}

void f32_forget_cursor(Fat *f) { f->cur_first = 0; }

int f32_alloc_cluster(Fat *f, u32 *out)
{
    for (u32 i = 0; i < f->clusters; i++)
    {
        u32 c = 2 + (f->next_free - 2 + i) % f->clusters;
        u32 v;
        int r = f32_fat_get(f, c, &v);
        if (r)
            return r;
        if (v == 0)
        {
            r = f32_fat_set(f, c, 0x0FFFFFFF);
            if (r)
                return r;
            f->next_free = c + 1 < f->clusters + 2 ? c + 1 : 2;
            if (f->free_count != 0xFFFFFFFFu && f->free_count)
                f->free_count--;
            *out = c;
            return 0;
        }
    }
    return FAT_ERR_FULL;
}

/* free a whole chain starting at `c` */
int f32_free_chain(Fat *f, u32 c)
{
    f32_forget_cursor(f);
    for (u32 guard = 0; valid_cluster(f, c) && guard <= f->clusters; guard++)
    {
        u32 next;
        int r = f32_fat_get(f, c, &next);
        if (r)
            return r;
        r = f32_fat_set(f, c, 0);
        if (r)
            return r;
        if (f->free_count != 0xFFFFFFFFu)
            f->free_count++;
        if (c < f->next_free)
            f->next_free = c; /* the next search can start here */
        if (next >= EOC)
            break;
        c = next;
    }
    return 0;
}

int f32_zero_cluster(Fat *f, u32 cl)
{
    zero(f->sec, f->ss);
    for (unsigned s = 0; s < f->spc; s++)
    {
        int r = f32_wr_sectors(f, cluster_lba(f, cl) + s, 1, f->sec);
        if (r)
            return r;
    }
    return 0;
}

int fat_sync(Fat *f)
{
    int r = fat_flush_cache(f);
    if (r)
        return r;
    if (f->fsinfo_sector && f->free_count != 0xFFFFFFFFu && f32_writable(f))
    {
        r = f32_rd_sectors(f, f->fsinfo_sector, 1, f->sec);
        if (r == 0 && rd32(f->sec) == 0x41615252u)
        {
            wr32(f->sec + 488, f->free_count);
            wr32(f->sec + 492, f->next_free);
            r = f32_wr_sectors(f, f->fsinfo_sector, 1, f->sec);
        }
        if (r)
            return r;
    }
    return blk_flush(f->blk) == 0 ? 0 : FAT_ERR_IO;
}

/* the end of a changing call: write everything out, unless a batch of changes is running */
int f32_finish(Fat *f) { return f->batch ? 0 : fat_sync(f); }

void fat_batch(Fat *f, int on)
{
    f->batch = on != 0;
    if (!on)
        fat_sync(f);
}
