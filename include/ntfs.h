/* HarisOS NTFS file system on a block device (blk.h): read, create, write, truncate, delete, rename, move.
 *
 * Reads: the boot sector, the MFT (also when it is fragmented or has an attribute list), folders (the
 * resident index and the allocated index blocks), file data (resident, non-resident with data runs, sparse).
 * Not supported: compressed and encrypted files (reading them fails with -2), alternate data streams,
 * links (they look like normal files and folders), anything on a volume that is not clean enough to find
 * its MFT. Names keep their ASCII letters; other characters show as '_'. Files of 4 GiB and more are
 * listed with a size of 4 GiB - 1.
 *
 * Writing keeps these structures consistent: the MFT record (fixups, the copy in $MFTMirr, the $MFT bitmap),
 * the folder index (a real B-tree: entries stay sorted by the $UpCase collation, full blocks split, empty
 * blocks are freed, the root moves into a block when it outgrows its MFT record), the cluster $Bitmap, and the
 * $FILE_NAME copies of the sizes and times (in the file's record and in the folder's index).
 *
 * The journal ($LogFile): this driver does not write log records. It does what ntfs-3g does instead:
 *   - it writes only to a volume that is clean: the dirty bit of $Volume is off and the $LogFile restart area
 *     says "clean" (or the log is empty). Otherwise every change fails with NTFS_ERR_DIRTY: Windows has to
 *     replay or check it first (chkdsk), because replaying a stale log over our changes would corrupt them.
 *   - while a change is in progress the $Volume dirty bit is ON, and the structures are written in an order that
 *     leaves a repairable volume if the power fails (bitmap first, then record, then index). The bit is turned
 *     OFF when the change is complete and flushed. After a crash the bit stays on, and Windows runs chkdsk.
 *   - the $LogFile itself is never touched, so it stays "clean".
 * If a disk write fails in the middle of a change, the volume is left dirty and refuses further changes.
 *
 * Limits of writing (refused with NTFS_ERR_UNSUPPORTED or NTFS_ERR_FULL):
 *   - files and folders that have an $ATTRIBUTE_LIST, compressed, encrypted or sparse files, the system files
 *     (records below 24), files with more than one hard link;
 *   - new names are ASCII only (existing non-ASCII names are kept and sorted correctly);
 *   - a new file takes the security id of the folder it is made in. A folder with the old 48-byte
 *     $STANDARD_INFORMATION has none (the root of many Windows volumes): then the id of an ordinary file (else
 *     folder) in the same folder is used, a valid but not necessarily Windows' own choice; a folder without such
 *     an entry refuses (NTFS_ERR_UNSUPPORTED, ntfs_unsupported_why());
 *   - the MFT does not grow: when its bitmap has no free record, creating fails with NTFS_ERR_FULL;
 *   - a record whose attributes do not fit in the MFT record (a very fragmented file) fails with NTFS_ERR_FULL;
 *   - a folder's index can only grow when a cluster is not bigger than an index block, and (with a non-resident index
 *     bitmap) up to the bitmap's size; a resident bitmap grows while the folder's record has room (64 bytes = 512 blocks).
 *   - a change that needs a free cluster for the index (a block split) is refused with NTFS_ERR_FULL on a full disk
 *     before anything is written; deleting works on a full disk (the file's clusters are freed first). */
#ifndef HARIS_NTFS_H
#define HARIS_NTFS_H

#define NTFS_NAME_MAX 256
#define NTFS_SECTOR_MAX 4096
#define NT_MAX_RUNS 128 /* data runs we can hold for one stream (a more fragmented file fails to open) */

#define NTFS_ERR_IO -1
#define NTFS_ERR_FULL -2        /* no free cluster / MFT record, or the record or the index has no room */
#define NTFS_ERR_EXISTS -3      /* the name is already used in that folder */
#define NTFS_ERR_RO -4          /* the disk is locked or cannot be written */
#define NTFS_ERR_NOTEMPTY -5    /* a folder with entries cannot be deleted */
#define NTFS_ERR_BADNAME -6     /* empty, too long, not ASCII, or has characters Windows does not allow */
#define NTFS_ERR_ARG -7
#define NTFS_ERR_UNSUPPORTED -8 /* see "Limits of writing" above */

/* The line of ntfs.c that gave the last NTFS_ERR_UNSUPPORTED (0 = none yet): says which limit was hit. */
int ntfs_unsupported_line(void);
/* A short sentence for the last NTFS_ERR_UNSUPPORTED when it is a limit people run into, else NULL. */
const char *ntfs_unsupported_why(void);
#define NTFS_ERR_DIRTY -9       /* the volume was not shut down cleanly (or a write failed): run chkdsk in Windows */

typedef struct
{
    unsigned long long vcn, len; /* virtual clusters [vcn, vcn + len) ... */
    long long lcn;               /* ... start at this cluster of the volume; -1 = a hole (reads as zeros) */
} NtRun;

typedef struct
{
    int n;
    NtRun r[NT_MAX_RUNS];
} NtRuns;

typedef struct
{
    int blk;
    unsigned ss;                    /* bytes per sector */
    unsigned csize;                 /* bytes per cluster */
    unsigned long long clusters;    /* clusters in the volume */
    unsigned long long mft_lcn;
    unsigned rec_size;              /* bytes per MFT record (1024 usually) */
    unsigned idx_size;              /* bytes per index block (4096 usually) */
    NtRuns mft;                     /* where the MFT is */
    unsigned long long free_clusters; /* ~0 = not counted */
    char label[40];

    unsigned char sec[NTFS_SECTOR_MAX]; /* sector cache (private) */
    unsigned long long sec_lba;

    /* writing (private) */
    unsigned long long mft_size;    /* bytes of the $MFT data */
    unsigned long long mirr_lcn;    /* $MFTMirr, from the boot sector (0 = none) */
    unsigned mirr_recs;             /* records it holds */
    unsigned long long hint;        /* where the cluster allocator looks first */
    int wstate;                     /* 0 = not checked yet, 1 = may write, < 0 = refuse with this error */
    int poisoned;                   /* a disk write failed in the middle of a change */
} Ntfs;

/* A file or folder: its MFT record number (the name is not kept). */
typedef struct
{
    unsigned recno;
    unsigned size; /* from the folder index (can be out of date); the real size is used when reading */
    unsigned char is_dir;
    unsigned char pad[3];
} NtEntry;

/* 0 or -1 (not NTFS, or the MFT cannot be read). */
int ntfs_mount(Ntfs *nt, int blk);

void ntfs_root(NtEntry *out);

/* Call cb for each file and folder in `dir` (not the system files of the root: $MFT, $Bitmap ...). Stops if cb
 * returns non-zero. Returns how many were reported, or -1. cb must not call into the volume. */
typedef int (*NtfsListFn)(const char *name, const NtEntry *e, void *user);
int ntfs_list(Ntfs *nt, const NtEntry *dir, NtfsListFn cb, void *user);

/* Read up to len bytes at offset: the count (0 at the end), -1 on an error, -2 if the file is compressed or
 * encrypted. */
int ntfs_read(Ntfs *nt, const NtEntry *e, unsigned offset, void *buf, unsigned len);

/* 1 if the volume is safe to write to: not dirty, $LogFile clean, and Windows is not hibernated (Fast Startup).
 * Changes nothing on the disk. */
int ntfs_is_clean(Ntfs *nt);

/* 1 if this looks like the volume Windows is installed on: a folder "Windows" in the root, and "Users" or
 * "Program Files" next to it (names in any case). Changes nothing. */
int ntfs_is_windows(Ntfs *nt);

/* Total and free bytes. 1 if the free space is known (the $Bitmap was read), else 0. */
int ntfs_space(Ntfs *nt, unsigned long long *total, unsigned long long *free_bytes);

/* ---- changing the volume. Every call writes everything and flushes before it returns; each returns >= 0 or a
 * negative NTFS_ERR_*. ---- */

/* Time source: 100 ns ticks since 1601-01-01 (a FILETIME). Without one, files get a fixed time. */
typedef unsigned long long (*NtfsClockFn)(void);
void ntfs_set_clock(NtfsClockFn fn);

/* Create an empty file or folder in `dir`. */
int ntfs_create(Ntfs *nt, const NtEntry *dir, const char *name, int is_dir, NtEntry *out);

/* Write len bytes at offset (offset <= size: overwrite and/or append). Updates e->size. Returns len or an error. */
int ntfs_write(Ntfs *nt, NtEntry *e, unsigned offset, const void *buf, unsigned len);

/* Make the file `size` bytes: shorter frees clusters, longer reads as zeros. Updates e->size. */
int ntfs_truncate(Ntfs *nt, NtEntry *e, unsigned size);

/* Delete a file or an empty folder (frees its record and clusters, removes its names from the folder). */
int ntfs_delete(Ntfs *nt, const NtEntry *e);

/* Rename inside the same folder, or move to new_dir under new_name. Folders can be moved, but not into
 * themselves. */
int ntfs_rename(Ntfs *nt, NtEntry *e, const char *new_name);
int ntfs_move(Ntfs *nt, NtEntry *e, const NtEntry *new_dir, const char *new_name);

#endif
