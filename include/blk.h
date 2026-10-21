/* HarisOS block devices: whole disks (NVMe, SATA) and their partitions, all read and written in sectors.
 *
 * A driver (ahci.c, nvme.c) registers each disk it finds with blk_register(). part_scan() then reads
 * the partition table (GPT or MBR) and registers every partition as a device of its own, so the file
 * system code only ever sees "a run of sectors" and does not care what is underneath.
 *
 * Writing is LOCKED by default: blk_write() refuses until blk_set_locked(dev, 0). A bug in a file system
 * or driver can destroy a real disk, so nothing is written unless the user unlocked that device. */
#ifndef HARIS_BLK_H
#define HARIS_BLK_H

#define BLK_MAX 32
#define BLK_SECTOR_MAX 4096 /* largest sector size supported (4Kn disks) */

enum
{
    BLK_NVME,
    BLK_SATA,
    BLK_RAM, /* tests */
    BLK_PART,
    BLK_USB /* USB mass storage (pen drive, external disk): can be unplugged, see blk_remove() */
};

/* what a partition holds, from its type (GPT type GUID or MBR type byte) */
enum
{
    PART_UNKNOWN,
    PART_ESP,   /* EFI System Partition (FAT) */
    PART_FAT,   /* FAT12/16/32 or Microsoft basic data (FAT, exFAT or NTFS: the file system code decides) */
    PART_NTFS_EXFAT,
    PART_LINUX,
    PART_OTHER
};

/* what kind of medium a disk is */
enum
{
    BLK_MEDIA_UNKNOWN,
    BLK_MEDIA_SSD,
    BLK_MEDIA_HDD
};

typedef struct BlkDev
{
    char name[16];  /* "nvme0", "sata0", "nvme0p1" */
    char model[48]; /* from the drive; partitions copy their disk's */
    int kind;       /* BLK_* (the bus: NVMe, SATA ...) */
    int media;      /* BLK_MEDIA_*: SSD or hard disk, as far as the drive says */
    int parent;     /* index of the disk a partition is on, -1 for a disk */
    unsigned sector_size;
    unsigned long long sectors;
    int writable; /* the driver can write at all */
    int locked;   /* 1 = blk_write() refuses (the default) */
    int removed;  /* 0 = present, 1 = unplugged (all I/O fails until blk_reap()), 2 = free slot */

    /* disks only: the driver's functions. `ctx` is the driver's own data. */
    int (*read)(struct BlkDev *d, unsigned long long lba, unsigned count, void *buf);
    int (*write)(struct BlkDev *d, unsigned long long lba, unsigned count, const void *buf);
    int (*flush)(struct BlkDev *d);
    void *ctx;

    /* partitions only */
    unsigned long long first_lba; /* on the parent disk */
    int part_kind;                /* PART_* */
    int part_no;                  /* 1, 2, 3 ... */
    char part_label[40];          /* GPT partition name (ASCII), "" for MBR */
} BlkDev;

/* Copy a BlkDev (a plain `*a = *b` may call memcpy, which does not exist in the kernel). */
void blk_copy(BlkDev *dst, const BlkDev *src);

/* Driver side. Copies *d; returns the index or -1 if the table is full. */
int blk_register(const BlkDev *d);

int blk_count(void);        /* table size, including unplugged devices (check BlkDev.removed) */
BlkDev *blk_get(int index); /* NULL if out of range */
void blk_reset(void);       /* forget everything (host tests) */

/* Hot-plug. blk_remove() marks a disk and its partitions as unplugged: reads, writes and flushes fail from then
 * on, but the entries stay (a mounted drive still points at them) until the file system layer has let go and
 * calls blk_reap(), which frees their slots for the next device. blk_generation() grows whenever a device is
 * registered or removed, so a polling loop can tell that the list changed. */
void blk_remove(int index);
void blk_reap(void);
unsigned blk_generation(void);

/* Sector I/O on a disk or a partition (a partition's sector 0 is its first sector). Return 0 or -1
 * (bad index, range outside the device, driver error, or - for writes - a locked / read-only device). */
int blk_read(int index, unsigned long long lba, unsigned count, void *buf);
int blk_write(int index, unsigned long long lba, unsigned count, const void *buf);
int blk_flush(int index);

/* Totals of all successful sector I/O since boot, over every device (for the Task Manager). A call on a
 * partition counts once, not again for its disk. blk_reset() zeroes them. */
typedef struct
{
    unsigned long long read_ops, write_ops;
    unsigned long long read_bytes, write_bytes;
} BlkStats;
void blk_get_stats(BlkStats *out);

/* Lock (1) or unlock (0) writing. The lock belongs to the device given: a partition has its own, so unlocking
 * one partition leaves the others (and the disk) locked. Giving a whole disk's index locks or unlocks its
 * partitions with it. */
void blk_set_locked(int index, int locked);

/* Read the partition table of disk `index` and register the partitions (GPT, else MBR primary
 * partitions). Returns how many were added; 0 if the disk has no table (it may hold a file system
 * from sector 0 - the caller tries that). */
int part_scan(int index);

const char *blk_kind_name(int kind);
const char *part_kind_name(int part_kind);

#endif
