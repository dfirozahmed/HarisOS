/* Block device registry and sector I/O. See include/blk.h. */
#include "blk.h"

static BlkDev DEVS[BLK_MAX];
static int NDEVS;
static unsigned GEN;
static BlkStats STATS;

/* Copying a struct with `=` could compile to a call to memcpy, which this freestanding kernel does not have. */
void blk_copy(BlkDev *dst, const BlkDev *src)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    for (unsigned long i = 0; i < sizeof *dst; i++)
        d[i] = s[i];
}

void blk_reset(void)
{
    NDEVS = 0;
    GEN = 0;
    STATS.read_ops = STATS.write_ops = STATS.read_bytes = STATS.write_bytes = 0;
}

void blk_get_stats(BlkStats *out)
{
    *out = STATS;
}

int blk_register(const BlkDev *d)
{
    int slot = -1;
    for (int i = 0; i < NDEVS && slot < 0; i++)
        if (DEVS[i].removed == 2)
            slot = i; /* a slot freed by blk_reap() */
    if (slot < 0)
    {
        if (NDEVS >= BLK_MAX)
            return -1;
        slot = NDEVS++;
    }
    blk_copy(&DEVS[slot], d);
    DEVS[slot].locked = 1; /* nothing is written until the user says so */
    DEVS[slot].removed = 0;
    GEN++;
    return slot;
}

void blk_remove(int index)
{
    BlkDev *d = blk_get(index);
    if (!d || d->removed)
        return;
    d->removed = 1;
    for (int i = 0; i < NDEVS; i++)
        if (DEVS[i].parent == index && !DEVS[i].removed)
            DEVS[i].removed = 1;
    GEN++;
}

void blk_reap(void)
{
    for (int i = 0; i < NDEVS; i++)
        if (DEVS[i].removed == 1)
        {
            DEVS[i].removed = 2;
            DEVS[i].parent = -1; /* a free slot is nobody's partition */
        }
    while (NDEVS > 0 && DEVS[NDEVS - 1].removed == 2)
        NDEVS--;
}

unsigned blk_generation(void) { return GEN; }

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
    if (!d || !disk || d->removed || disk->removed || !disk->read || !buf || !in_range(d, lba, count))
        return -1;
    unsigned long long abs = lba + (d->parent >= 0 ? d->first_lba : 0);
    int rc = disk->read(disk, abs, count, buf);
    if (rc == 0)
    {
        STATS.read_ops++;
        STATS.read_bytes += (unsigned long long)count * d->sector_size;
    }
    return rc;
}

int blk_write(int index, unsigned long long lba, unsigned count, const void *buf)
{
    BlkDev *d = blk_get(index);
    BlkDev *disk = disk_of(index);
    if (!d || !disk || d->removed || disk->removed || !disk->write || !disk->writable || d->locked || !buf ||
        !in_range(d, lba, count))
        return -1;
    unsigned long long abs = lba + (d->parent >= 0 ? d->first_lba : 0);
    int rc = disk->write(disk, abs, count, buf);
    if (rc == 0)
    {
        STATS.write_ops++;
        STATS.write_bytes += (unsigned long long)count * d->sector_size;
    }
    return rc;
}

int blk_flush(int index)
{
    BlkDev *disk = disk_of(index);
    if (disk && disk->removed)
        return -1;
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
    case BLK_USB:
        return "USB drive";
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
