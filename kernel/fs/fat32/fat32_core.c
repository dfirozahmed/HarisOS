/* FAT32: the clock, date and time values, block device access. */
#include "fat32_internal.h"

/* FAT32 file system. See include/fat32.h.
 *
 * Layout of a FAT32 volume (all in sectors, counted from the start of the partition):
 *   reserved area (boot sector "BPB", FSInfo, backup boot sector ...)
 *   FAT #1, FAT #2   one 32-bit entry per cluster: 0 = free, next cluster number, or >= 0x0FFFFFF8 = last of the chain
 *   data area        cluster 2, 3, 4 ...; the root folder is a chain of clusters too
 * A folder is a list of 32-byte entries. A file with a long name has some "long name" entries (attribute
 * 0x0F, 13 UCS-2 characters each, last piece first) right before its short 8.3 entry. */
FatClockFn f32_CLOCK;

void fat_set_clock(FatClockFn fn) { f32_CLOCK = fn; }
unsigned fat_make_date(unsigned year, unsigned month, unsigned day)
{
    if (year < 1980)
        year = 1980;
    return ((year - 1980) << 9) | (month << 5) | day;
}
unsigned fat_make_time(unsigned hour, unsigned minute, unsigned second) { return (hour << 11) | (minute << 5) | (second / 2); }

/* ---- block device access --------------------------------------- */
int f32_writable(const Fat *f)
{
    const BlkDev *d = blk_get(f->blk);
    if (!d)
        return 0;
    const BlkDev *disk = d->parent >= 0 ? blk_get(d->parent) : d;
    return disk && disk->writable && !d->locked; /* the lock is the partition's own (see blk_set_locked) */
}

int f32_rd_sectors(Fat *f, u64 lba, unsigned n, void *buf) { return blk_read(f->blk, lba, n, buf) == 0 ? 0 : FAT_ERR_IO; }

int f32_wr_sectors(Fat *f, u64 lba, unsigned n, const void *buf)
{
    if (!f32_writable(f))
        return FAT_ERR_RO;
    return blk_write(f->blk, lba, n, buf) == 0 ? 0 : FAT_ERR_IO;
}
