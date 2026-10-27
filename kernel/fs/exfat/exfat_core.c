/* exFAT: sectors. */
#include "exfat_internal.h"

/* exFAT file system, read only. See include/exfat.h.
 *
 * Layout (sectors counted from the start of the partition):
 *   boot region    main boot sector with all the sizes, 8 extended boot sectors, checksum; then a backup
 *   FAT            one 32-bit entry per cluster (only used by files that are not stored in one piece)
 *   cluster heap   cluster 2 on: the allocation bitmap, the up-case table, the root folder, all the data
 * A folder is a list of 32-byte entries. One file = an "entry set": a File entry (0x85) saying how many
 * secondary entries follow, then a Stream Extension (0xC0: size, first cluster, flags) and one or more File Name
 * entries (0xC1: 15 UTF-16 characters each). A file whose "NoFatChain" flag is set has its clusters one after
 * the other, without chain in the FAT. */
/* ---- sectors ------------------------------------------------------ */
int exf_load_sec(ExFat *f, u64 lba)
{
    if (f->sec_lba == lba)
        return 0;
    if (blk_read(f->blk, lba, 1, f->sec) != 0)
    {
        f->sec_lba = NONE;
        return -1;
    }
    f->sec_lba = lba;
    return 0;
}

int exf_fat_flush(ExFat *f);

int exf_fat_get(ExFat *f, u32 cl, u32 *out)
{
    u64 off = (u64)cl * 4;
    u64 lba = f->fat_lba + off / f->ss;
    if (f->fat_buf_lba != lba)
    {
        if (f->fat_dirty && exf_fat_flush(f))
            return -1;
        if (blk_read(f->blk, lba, 1, f->fat_buf) != 0)
        {
            f->fat_buf_lba = NONE;
            return -1;
        }
        f->fat_buf_lba = lba;
    }
    *out = rd32(f->fat_buf + off % f->ss);
    return 0;
}

/* The index-th cluster of a file or folder. 1 and the cluster; 0 if the data ends before it; -1 on an error.
 * A cursor remembers the last chain position so that reading in order stays quick. */
int exf_cluster_at(ExFat *f, u32 first, int nofat, u32 index, u32 *out)
{
    if (!valid_cluster(f, first))
        return 0;
    if (nofat)
    {
        u64 c = (u64)first + index;
        if (c >= (u64)f->clusters + 2)
            return 0;
        *out = (u32)c;
        return 1;
    }
    u32 c = first, i = 0;
    if (f->cur_first == first && f->cur_index <= index && valid_cluster(f, f->cur_cluster))
    {
        c = f->cur_cluster;
        i = f->cur_index;
    }
    while (i < index)
    {
        u32 next;
        if (exf_fat_get(f, c, &next))
            return -1;
        if (next >= EOC)
            return 0;
        if (!valid_cluster(f, next))
            return -1;
        c = next;
        i++;
    }
    f->cur_first = first;
    f->cur_index = i;
    f->cur_cluster = c;
    *out = c;
    return 1;
}
