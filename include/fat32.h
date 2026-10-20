/* HarisOS FAT32 file system on a block device (blk.h): read, create, write, delete, rename, move.
 *
 * Long file names are supported (stored as VFAT entries). Only ASCII letters are kept: a name with
 * other characters (Bangla, Chinese ...) shows those characters as '_'. FAT12 / FAT16 volumes are not
 * supported (mount fails), nor are exFAT and NTFS.
 *
 * Everything that changes the disk goes through blk_write(), so it fails with FAT_ERR_RO while the disk is
 * locked (the default). A mutating call writes the FATs and the FSInfo sector and flushes before it returns. */
#ifndef HARIS_FAT32_H
#define HARIS_FAT32_H

#define FAT_NAME_MAX 256 /* longest name incl. the final 0 */
#define FAT_SECTOR_MAX 4096

/* error codes (negative) */
#define FAT_ERR_IO -1
#define FAT_ERR_FULL -2     /* no free cluster, or no room in the directory */
#define FAT_ERR_EXISTS -3   /* the name is already used in that folder */
#define FAT_ERR_RO -4       /* the disk is locked or cannot be written */
#define FAT_ERR_NOTEMPTY -5 /* a folder with entries cannot be deleted */
#define FAT_ERR_BADNAME -6  /* empty, too long, or has characters FAT does not allow */
#define FAT_ERR_ARG -7

#define FAT_ATTR_RO 0x01
#define FAT_ATTR_HIDDEN 0x02
#define FAT_ATTR_SYSTEM 0x04
#define FAT_ATTR_VOLUME 0x08
#define FAT_ATTR_DIR 0x10
#define FAT_ATTR_ARCHIVE 0x20

typedef struct
{
    int blk;                    /* the partition (or disk) the volume is on */
    unsigned ss;                /* bytes per sector */
    unsigned spc;               /* sectors per cluster */
    unsigned long long fat0_lba; /* start of the first FAT */
    unsigned long long fat_lba;  /* start of the FAT that is read (the active one) */
    unsigned fat_sectors;
    unsigned nfats;
    int mirror;                 /* write every FAT copy (1) or only the active one (0) */
    unsigned long long data_lba;
    unsigned root_cluster;
    unsigned clusters;          /* data clusters: valid numbers are 2 .. clusters + 1 */
    unsigned fsinfo_sector;     /* 0 = none */
    unsigned next_free;         /* where the next search for a free cluster starts */
    unsigned free_count;        /* 0xFFFFFFFF = not counted yet */
    char label[12];

    int batch;                   /* 1 while a batch of changes runs: changing calls do not flush (see fat_batch) */

    /* caches (private) */
    unsigned char fat_buf[FAT_SECTOR_MAX];
    unsigned long long fat_buf_lba; /* sector of the FAT held in fat_buf, ~0 = none */
    int fat_dirty;
    unsigned char sec[FAT_SECTOR_MAX]; /* scratch sector for directories / partial data sectors */
    unsigned cur_first, cur_index, cur_cluster; /* cluster-chain position of the file used last */
} Fat;

/* One file or folder: where it is on the volume. The name is not kept (the caller has it). */
typedef struct
{
    unsigned first_cluster; /* 0 for an empty file */
    unsigned size;          /* bytes; 0 for a folder */
    unsigned dir_cluster;   /* the folder it is in */
    unsigned index;         /* index (32-byte slots) of its short entry in that folder */
    unsigned char attr;
    unsigned char nlfn;     /* long-name entries just before the short one */
} FatEntry;

/* The clock for timestamps of new and changed entries: *date and *time in FAT format. Without one,
 * everything gets 1980-01-01. */
typedef void (*FatClockFn)(unsigned *date, unsigned *time);
void fat_set_clock(FatClockFn fn);
unsigned fat_make_date(unsigned year, unsigned month, unsigned day);
unsigned fat_make_time(unsigned hour, unsigned minute, unsigned second);

/* Mount the FAT32 volume on block device `blk`. 0 or -1 (not FAT32, damaged boot sector). */
int fat_mount(Fat *f, int blk);

unsigned fat_root(const Fat *f); /* cluster of the root folder */

/* Call cb for every file and folder in the folder at `dir_cluster` (not "." and "..", not the volume label).
 * Stops early if cb returns non-zero. Returns how many entries were reported, or FAT_ERR_IO. */
typedef int (*FatListFn)(const char *name, const FatEntry *e, void *user);
int fat_list(Fat *f, unsigned dir_cluster, FatListFn cb, void *user);

/* Read up to len bytes at offset. Returns the bytes read (0 at the end) or FAT_ERR_IO. */
int fat_read(Fat *f, const FatEntry *e, unsigned offset, void *buf, unsigned len);

/* Create an empty file or a folder (with "." and "..") in the folder at dir_cluster. */
int fat_create(Fat *f, unsigned dir_cluster, const char *name, int is_dir, FatEntry *out);

/* Write len bytes at offset (offset <= size: overwrite and/or append). Updates *e. Returns len or an error. */
int fat_write(Fat *f, FatEntry *e, unsigned offset, const void *buf, unsigned len);

/* Make a file shorter (frees the clusters it no longer needs). new_size must be <= the size. */
int fat_shrink(Fat *f, FatEntry *e, unsigned new_size);

/* Delete a file or an empty folder. */
int fat_delete(Fat *f, const FatEntry *e);

/* Give an entry a new name in the same folder. Updates *e. */
int fat_rename(Fat *f, FatEntry *e, const char *new_name);

/* Move an entry to another folder, keeping its name `name`. Updates *e. Moving a folder into itself is the
 * caller's job to prevent. */
int fat_move(Fat *f, FatEntry *e, const char *name, unsigned new_dir_cluster);

/* Sizes in bytes. The free space comes from the FSInfo sector, else from counting the FAT (the first call
 * reads it all; a FAT bigger than 64 MiB is not counted). Returns 1 if *free_bytes is known, 0 if not (then 0). */
int fat_space(Fat *f, unsigned long long *total, unsigned long long *free_bytes);

/* Write pending changes (FAT cache, FSInfo) and flush the disk. Mutating calls do this themselves. */
int fat_sync(Fat *f);

/* fat_batch(f, 1) ... many changing calls ... fat_batch(f, 0): the calls in between do not flush after each
 * step; the end does. Copying a big file needs this to be fast. If the power fails inside a batch the FATs
 * may be behind the data: files being written are lost, older ones are not touched. */
void fat_batch(Fat *f, int on);

#endif
