/* Block device registry and sector I/O. See include/blk.h. */
#include "blk.h"

static BlkDev DEVS[BLK_MAX];
static int NDEVS;

/* Copying a struct with `=` could compile to a call to memcpy, which this freestanding kernel does not have. */
void blk_copy(BlkDev *dst, const BlkDev *src)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    for (unsigned long i = 0; i < sizeof *dst; i++)
        d[i] = s[i];
}

void blk_reset(void) { NDEVS = 0; }

int blk_register(const BlkDev *d)
{
    if (NDEVS >= BLK_MAX)
        return -1;
    blk_copy(&DEVS[NDEVS], d);
    DEVS[NDEVS].locked = 1; /* nothing is written until the user says so */
    return NDEVS++;
}

int blk_count(void) { return NDEVS; }

BlkDev *blk_get(int index) { return (index >= 0 && index < NDEVS) ? &DEVS[index] : (BlkDev *)0; }

/* the disk a device is on (itself for a disk) */
static BlkDev *disk_of(int index)
{
    BlkDev *d = blk_get(index);
    if (d && d->parent >= 0)
        d = blk_get(d->parent);
    return d;
}

static int in_range(const BlkDev *d, unsigned long long lba, unsigned count)
{
    return count > 0 && lba < d->sectors && count <= d->sectors - lba;
}

int blk_read(int index, unsigned long long lba, unsigned count, void *buf)
{
    BlkDev *d = blk_get(index);
    BlkDev *disk = disk_of(index);
    if (!d || !disk || !disk->read || !buf || !in_range(d, lba, count))
        return -1;
    unsigned long long abs = lba + (d->parent >= 0 ? d->first_lba : 0);
    return disk->read(disk, abs, count, buf);
}

int blk_write(int index, unsigned long long lba, unsigned count, const void *buf)
{
    BlkDev *d = blk_get(index);
    BlkDev *disk = disk_of(index);
    if (!d || !disk || !disk->write || !disk->writable || d->locked || !buf || !in_range(d, lba, count))
        return -1;
    unsigned long long abs = lba + (d->parent >= 0 ? d->first_lba : 0);
    return disk->write(disk, abs, count, buf);
}

int blk_flush(int index)
{
    BlkDev *disk = disk_of(index);
    if (!disk || !disk->flush)
        return 0; /* nothing to flush */
    return disk->flush(disk);
}

void blk_set_locked(int index, int locked)
{
    BlkDev *d = blk_get(index);
    if (!d)
        return;
    d->locked = locked != 0;
    if (d->parent < 0) /* a whole disk: its partitions follow it; a partition on its own does not touch the others */
        for (int i = 0; i < NDEVS; i++)
            if (DEVS[i].parent == index)
                DEVS[i].locked = d->locked;
}

const char *blk_kind_name(int kind)
{
    switch (kind)
    {
    case BLK_NVME:
        return "NVMe SSD";
    case BLK_SATA:
        return "SATA disk";
    case BLK_RAM:
        return "RAM disk";
    case BLK_PART:
        return "Partition";
    default:
        return "Disk";
    }
}

const char *part_kind_name(int part_kind)
{
    switch (part_kind)
    {
    case PART_ESP:
        return "EFI System";
    case PART_FAT:
        return "Data";
    case PART_NTFS_EXFAT:
        return "NTFS / exFAT";
    case PART_LINUX:
        return "Linux";
    case PART_OTHER:
        return "Other";
    default:
        return "Unknown";
    }
}
