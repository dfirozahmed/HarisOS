/* HarisOS exFAT file system on a block device (blk.h): read, create, write, delete, rename, move.
 *
 * Big files (4 GiB and more) are listed with a size of 4 GiB - 1 and can be read up to that offset; files are
 * written up to 4 GiB - 1. Only ASCII letters of names are kept (other characters show as '_'; new names must be
 * ASCII). Names are compared as ASCII ignoring case. Reading does not check the entry checksums; writing
 * keeps them right (and the name hash, from the volume's up-case table). The volume dirty flag is not touched.
 *
 * Everything that changes the disk goes through blk_write(), so it fails with EXFAT_ERR_RO while the disk is
 * locked (the default). A changing call writes the bitmap, the FAT and the entries and flushes before it
 * returns (unless a batch is running). */
#ifndef HARIS_EXFAT_H
#define HARIS_EXFAT_H

#define EXFAT_NAME_MAX 256 /* 255 characters + 0 */
#define EXFAT_SECTOR_MAX 4096

/* error codes (negative), the same numbers as FAT_ERR_* and VFS_ERR_* */
#define EXFAT_ERR_IO -1
#define EXFAT_ERR_FULL -2
#define EXFAT_ERR_EXISTS -3
#define EXFAT_ERR_RO -4
#define EXFAT_ERR_NOTEMPTY -5
#define EXFAT_ERR_BADNAME -6
#define EXFAT_ERR_ARG -7

#define EXFAT_ATTR_RO 0x01
#define EXFAT_ATTR_HIDDEN 0x02
#define EXFAT_ATTR_SYSTEM 0x04
#define EXFAT_ATTR_DIR 0x10

typedef struct
{
    int blk;
    unsigned ss, spc;           /* bytes per sector, sectors per cluster */
    unsigned long long fat_lba;
    unsigned long long heap_lba; /* sector of cluster 2 */
    unsigned fat_sectors;
    unsigned clusters;           /* valid cluster numbers: 2 .. clusters + 1 */
    unsigned root_cluster;
    unsigned bitmap_cluster;     /* the allocation bitmap (0 = not found yet) */
    unsigned long long bitmap_bytes;
    unsigned long long free_clusters; /* ~0 = not counted */
    char label[24];
    unsigned upcase_cluster;     /* the up-case table (for name hashes) */
    unsigned upcase_bytes;
    unsigned alloc_hint;         /* where the next search for a free cluster starts */
    int batch;                   /* 1 while a batch of changes runs (see exfat_batch) */

    /* write caches (private) */
    unsigned char bm_buf[EXFAT_SECTOR_MAX];
    unsigned long long bm_lba;
    int bm_dirty, fat_dirty;

    /* caches (private) */
    unsigned char sec[EXFAT_SECTOR_MAX];     /* directory / data sector */
    unsigned long long sec_lba;               /* which sector is in sec, ~0 = none */
    unsigned char fat_buf[EXFAT_SECTOR_MAX];
    unsigned long long fat_buf_lba;
    unsigned cur_first, cur_index, cur_cluster; /* position in the cluster chain used last */
} ExFat;

/* A file or folder: where its data is. The name is not kept. */
typedef struct
{
    unsigned first_cluster; /* 0 for an empty file */
    unsigned size;          /* DataLength, at most 4 GiB - 1 */
    unsigned valid;         /* ValidDataLength (bytes after it read as zero), at most 4 GiB - 1 */
    unsigned set_index;     /* index of its File entry in the folder it is in */
    unsigned char attr;     /* EXFAT_ATTR_* */
    unsigned char nofat;    /* the clusters follow each other: no FAT chain */
    unsigned char is_root;  /* the root folder (no length: its chain ends in the FAT) */
    unsigned char secondary; /* SecondaryCount of its entry set: 1 + name entries */
} ExEntry;

/* Mount: 0 or -1 (not exFAT, damaged boot sector). */
int exfat_mount(ExFat *f, int blk);

/* 1 if the volume is not marked dirty and has no media failure flag (boot sector VolumeFlags). Changes nothing. */
int exfat_is_clean(ExFat *f);

/* The root folder as an entry. */
void exfat_root(const ExFat *f, ExEntry *out);

/* Call cb for each file and folder in `dir`; stops if cb returns non-zero. Returns how many were
 * reported, or -1 on a read error. cb must not call into this volume. */
typedef int (*ExFatListFn)(const char *name, const ExEntry *e, void *user);
int exfat_list(ExFat *f, const ExEntry *dir, ExFatListFn cb, void *user);

/* Read up to len bytes at offset: the count (0 at the end) or -1. */
int exfat_read(ExFat *f, const ExEntry *e, unsigned offset, void *buf, unsigned len);

/* Total and free bytes. Returns 1 if the free space is known (the allocation bitmap was read), else 0. */
int exfat_space(ExFat *f, unsigned long long *total, unsigned long long *free_bytes);

/* ---- changing the volume ---------------------------------------------------------------------------
 * `dir` is the folder an entry is in (an ExEntry, from exfat_root() or exfat_list()), and `dir_parent` the
 * folder `dir` itself is in (NULL when dir is the root). The calls update *dir (and the entry set of the
 * folder in dir_parent) when the folder grows, so the caller keeps its copy of *dir. */

/* The clock for timestamps: *date and *time in FAT format (see fat32.h). Without one: 1980-01-01. */
typedef void (*ExFatClockFn)(unsigned *date, unsigned *time);
void exfat_set_clock(ExFatClockFn fn);

/* Create an empty file or a folder. */
int exfat_create(ExFat *f, ExEntry *dir, const ExEntry *dir_parent, const char *name, int is_dir, ExEntry *out);

/* Write len bytes at offset (offset <= size: overwrite and/or append). Updates *e (and its entry set). Returns len
 * or an error. */
int exfat_write(ExFat *f, const ExEntry *dir, ExEntry *e, unsigned offset, const void *buf, unsigned len);

/* Delete a file or an empty folder. */
int exfat_delete(ExFat *f, const ExEntry *dir, const ExEntry *e);

/* New name, same folder. Updates *e. */
int exfat_rename(ExFat *f, ExEntry *dir, const ExEntry *dir_parent, ExEntry *e, const char *new_name);

/* Move an entry to another folder under the name `name`. Updates *e and *new_dir. */
int exfat_move(ExFat *f, const ExEntry *old_dir, ExEntry *new_dir, const ExEntry *new_dir_parent, ExEntry *e, const char *name);

/* fat_batch-like: between exfat_batch(f, 1) and exfat_batch(f, 0) changing calls do not flush after each step. */
void exfat_batch(ExFat *f, int on);
int exfat_sync(ExFat *f);

#endif
