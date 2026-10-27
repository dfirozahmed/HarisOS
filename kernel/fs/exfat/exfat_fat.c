/* exFAT write side: the FAT and the allocation bitmap, cached by sector. */
#include "exfat_internal.h"

/* ====================== changing the volume ====================== */
/* Everything below writes. A changing call ends with finish(): the cached FAT and bitmap sectors are written
 * and the disk is flushed (unless a batch of changes runs). Clusters of new data come from the allocation bitmap
 * (bit n = cluster n + 2); a file gets a FAT chain unless it already is stored in one piece and the clusters after
 * it are free (then it just grows and stays "NoFatChain"). */
ExFatClockFn exf_CLOCK;
void exfat_set_clock(ExFatClockFn fn) { exf_CLOCK = fn; }

void exf_wr16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
void exf_wr32(u8 *p, u32 v)
{
    exf_wr16(p, v);
    exf_wr16(p + 2, v >> 16);
}
void exf_wr64(u8 *p, u64 v)
{
    exf_wr32(p, (u32)v);
    exf_wr32(p + 4, (u32)(v >> 32));
}

int exf_writable(const ExFat *f)
{
    const BlkDev *d = blk_get(f->blk);
    if (!d)
        return 0;
    const BlkDev *disk = d->parent >= 0 ? blk_get(d->parent) : d;
    return disk && disk->writable && !d->locked; /* the lock is the partition's own (see blk_set_locked) */
}

int exf_wr_sector(ExFat *f, u64 lba, const void *buf)
{
    if (!exf_writable(f))
        return EXFAT_ERR_RO;
    return blk_write(f->blk, lba, 1, buf) == 0 ? 0 : EXFAT_ERR_IO;
}

/* ---- the FAT and the allocation bitmap, cached by sector ---- */
int exf_fat_flush(ExFat *f)
{
    if (!f->fat_dirty)
        return 0;
    int r = exf_wr_sector(f, f->fat_buf_lba, f->fat_buf);
    if (r)
        return r;
    f->fat_dirty = 0;
    return 0;
}

int exf_fat_put(ExFat *f, u32 cl, u32 value)
{
    u32 dummy;
    int r = exf_fat_get(f, cl, &dummy); /* loads (and, if needed, flushes) the sector */
    if (r)
        return EXFAT_ERR_IO;
    exf_wr32(f->fat_buf + ((u64)cl * 4) % f->ss, value);
    f->fat_dirty = 1;
    return 0;
}

static int bm_flush(ExFat *f)
{
    if (!f->bm_dirty)
        return 0;
    int r = exf_wr_sector(f, f->bm_lba, f->bm_buf);
    if (r)
        return r;
    f->bm_dirty = 0;
    return 0;
}

static int bm_load(ExFat *f, u64 lba)
{
    if (f->bm_lba == lba)
        return 0;
    int r = bm_flush(f);
    if (r)
        return r;
    if (blk_read(f->blk, lba, 1, f->bm_buf) != 0)
    {
        f->bm_lba = NONE;
        return EXFAT_ERR_IO;
    }
    f->bm_lba = lba;
    return 0;
}

int exf_bm_get(ExFat *f, u32 cl, int *used)
{
    if (!f->bitmap_cluster)
        return EXFAT_ERR_IO;
    u64 bit = cl - 2, byte = bit / 8;
    int r = bm_load(f, cluster_lba(f, f->bitmap_cluster) + byte / f->ss);
    if (r)
        return r;
    *used = (f->bm_buf[byte % f->ss] >> (bit % 8)) & 1;
    return 0;
}

int exf_bm_put(ExFat *f, u32 cl, int used)
{
    u64 bit = cl - 2, byte = bit / 8;
    int r = bm_load(f, cluster_lba(f, f->bitmap_cluster) + byte / f->ss);
    if (r)
        return r;
    u8 *p = &f->bm_buf[byte % f->ss];
    int was = (*p >> (bit % 8)) & 1;
    if (used)
        *p |= (u8)(1u << (bit % 8));
    else
        *p &= (u8)~(1u << (bit % 8));
    f->bm_dirty = 1;
    if (f->free_clusters != NONE && was != used)
        f->free_clusters += used ? (u64)-1 : 1;
    return 0;
}

int exfat_sync(ExFat *f)
{
    int r = exf_fat_flush(f);
    if (r)
        return r;
    r = bm_flush(f);
    if (r)
        return r;
    return blk_flush(f->blk) == 0 ? 0 : EXFAT_ERR_IO;
}

int exf_finish(ExFat *f) { return f->batch ? 0 : exfat_sync(f); }

void exfat_batch(ExFat *f, int on)
{
    f->batch = on != 0;
    if (!on)
        exfat_sync(f);
}

/* a free cluster: marked used in the bitmap, found from `hint` on (wrapping) */
int exf_alloc_cluster(ExFat *f, u32 hint, u32 *out)
{
    if (!valid_cluster(f, hint))
        hint = f->alloc_hint;
    if (!valid_cluster(f, hint))
        hint = 2;
    for (u32 i = 0; i < f->clusters; i++)
    {
        u32 c = 2 + (hint - 2 + i) % f->clusters;
        int used;
        int r = exf_bm_get(f, c, &used);
        if (r)
            return r;
        if (!used)
        {
            r = exf_bm_put(f, c, 1);
            if (r)
                return r;
            f->alloc_hint = c + 1 < f->clusters + 2 ? c + 1 : 2;
            *out = c;
            return 0;
        }
    }
    return EXFAT_ERR_FULL;
}

void exf_forget_cursor(ExFat *f) { f->cur_first = 0; }
