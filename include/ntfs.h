/* HarisOS NTFS file system, READ ONLY, on a block device (blk.h).
 *
 * Reads: the boot sector, the MFT (also when it is fragmented or has an attribute list), folders (the
 * resident index and the allocated index blocks), file data (resident, non-resident with data runs, sparse).
 * Not supported: compressed and encrypted files (reading them fails with -2), alternate data streams,
 * links (they look like normal files and folders), anything on a volume that is not clean enough to find
 * its MFT. Names keep their ASCII letters; other characters show as '_'. Files of 4 GiB and more are
 * listed with a size of 4 GiB - 1. */
#ifndef HARIS_NTFS_H
#define HARIS_NTFS_H

#define NTFS_NAME_MAX 256
#define NTFS_SECTOR_MAX 4096
#define NT_MAX_RUNS 128 /* data runs we can hold for one stream (a more fragmented file fails to open) */

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

/* Total and free bytes. 1 if the free space is known (the $Bitmap was read), else 0. */
int ntfs_space(Ntfs *nt, unsigned long long *total, unsigned long long *free_bytes);

#endif
