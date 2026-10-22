/* HarisOS virtual file system: one tree of folders and files (no malloc) that the File Explorer browses.
 *
 * The root is "This PC". Its children are the volumes (drives): the RAM Disk with demo content, and every
 * file system a driver mounts with vfs_mount() (FAT32 volumes on SSDs, hard disks and USB sticks: volume.c).
 * A node of a RAM-disk folder lives only in memory. A folder on a drive is read from the drive the first time
 * it is listed (the drive's "backend" fills in the entries), and every change - new folder, rename, delete,
 * copy, move - is done on the drive first and then shown in the tree.
 *
 * Nodes are identified by an id; the root is VFS_ROOT. Functions that fail return -1 and remember why:
 * vfs_last_error() / vfs_error_text(). */
#ifndef HARIS_VFS_H
#define HARIS_VFS_H

#define VFS_MAX_NODES 2048
#define VFS_NAME_MAX 96 /* including the final 0; longer names of a drive are cut when shown */
#define VFS_FS_BYTES 24 /* room for a drive's own locator in a node */
#define VFS_ROOT 0
#define VFS_MAX_VOLUMES 8

/* error codes (negative): the same numbers as FAT_ERR_* in fat32.h */
#define VFS_ERR_IO -1
#define VFS_ERR_FULL -2
#define VFS_ERR_EXISTS -3
#define VFS_ERR_RO -4
#define VFS_ERR_NOTEMPTY -5
#define VFS_ERR_BADNAME -6
#define VFS_ERR_ARG -7
#define VFS_ERR_UNSUPPORTED -8 /* e.g. copying between the RAM Disk and a drive */
#define VFS_ERR_NOWRITE -9     /* the drive cannot be written at all (no write driver for this disk) */
#define VFS_ERR_NOTREADABLE -10 /* a compressed or encrypted file */
#define VFS_ERR_DIRTY -11      /* an NTFS drive that was not shut down cleanly: Windows has to check it first */

typedef struct
{
    char name[VFS_NAME_MAX]; /* "" for the root */
    int used;
    int is_dir;
    unsigned size; /* bytes; always 0 for a folder */
    int parent;    /* -1 for the root */
    int vol;       /* volume this node is on, -1 for the root */
    int loaded;    /* folder on a drive: its entries have been read */
    int truncated; /* ... but not all of them fitted in the node table */
    unsigned char seen;
    unsigned char fs[VFS_FS_BYTES]; /* the drive's own locator for this entry */
} VfsNode;

/* What a volume tells the UI. */
typedef struct
{
    int node;               /* the volume's folder in the tree */
    char name[64];          /* "HARISOS (nvme0p1)" */
    char fs_name[12];       /* "FAT32", "RAM" */
    char device[24];        /* "SSD (NVMe)", "HDD (SATA)", "RAM" */
    unsigned long long total; /* bytes, 0 = unknown */
    unsigned long long free;  /* bytes; (unsigned long long)-1 = not known (too big to count quickly) */
    int writable;           /* the drive can be written to at all */
    int locked;             /* ... but writing is locked */
    int lockable;           /* the user can lock / unlock it (vfs_set_locked) */
} VfsVolInfo;

/* A drive's file system code. `list` marks a real drive (the RAM Disk has none). A read-only file system
 * leaves create / remove / rename / move / write NULL: the VFS then refuses changes with VFS_ERR_NOWRITE.
 * Functions return 0 / a byte count, or a negative VFS_ERR_* code. Nodes are passed as pointers into the
 * node table: `fs` holds the backend's locator and the backend updates it when an entry changes. */
typedef struct VfsBackend
{
    /* call vfs_attach() for every entry of folder node `dir` */
    int (*list)(void *ctx, int dir);
    int (*create)(void *ctx, VfsNode *parent, const char *name, int is_dir, unsigned char *fs_out); /* may update parent->fs (a folder that grew) */
    int (*remove)(void *ctx, const VfsNode *n); /* a file or an EMPTY folder */
    int (*rename)(void *ctx, VfsNode *n, const char *new_name);
    int (*move)(void *ctx, VfsNode *n, const char *name, VfsNode *new_parent); /* updates n->fs and maybe new_parent->fs */
    int (*read)(void *ctx, const VfsNode *n, unsigned offset, void *buf, unsigned len);
    int (*write)(void *ctx, VfsNode *n, unsigned offset, const void *buf, unsigned len); /* updates n->size */
    void (*info)(void *ctx, VfsVolInfo *out);
    int (*set_locked)(void *ctx, int locked);
    void (*batch)(void *ctx, int on); /* on = 1: a run of changes starts (skip flushing), 0: it ends (flush) */
    int (*truncate)(void *ctx, VfsNode *n, unsigned size); /* shorten a file (never grows it); updates n->size; may be NULL */
} VfsBackend;

/* Empty tree (just the root, no volumes), then the RAM Disk with demo content. vfs_reset() alone gives
 * only the root. */
void vfs_reset(void);
void vfs_init(void);

/* ---- volumes ---------------------------------------------------- */
/* Add a volume under the root, named `name` (made unique). Returns the volume index or -1. */
int vfs_mount(const VfsBackend *ops, void *ctx, const char *name);
/* Remove the volume that was mounted with this `ctx` (a drive that was unplugged), with everything in it.
 * Later volumes move up one place, so volume numbers are not stable. 0, or -1 if there is no such volume. */
int vfs_unmount(void *ctx);
unsigned vfs_generation(void);               /* grows with every vfs_mount() / vfs_unmount(): "the list of drives changed" */
int vfs_volume_count(void);
int vfs_volume_node(int vol);                /* the volume's folder, -1 if no such volume */
int vfs_volume_of(int id);                   /* volume of a node, -1 for the root */
int vfs_volume_info(int vol, VfsVolInfo *out); /* 0, or -1 */
int vfs_set_locked(int vol, int locked);     /* lock / unlock writing; 0 or -1 */
int vfs_is_volume_root(int id);

/* For backends, from inside list(): add (or update) one entry of folder `dir`. Returns the node, or -1 when
 * the table is full (the folder is then marked truncated). */
int vfs_attach(int dir, const char *name, int is_dir, unsigned size, const unsigned char *fs);

/* Add a file or folder under `parent`. Returns the new id, or -1: parent is not a folder, the name is
 * empty / too long / contains '/', the name already exists there, the table is full, or the drive refused
 * (locked, full ...: see vfs_last_error()). `size` only counts for RAM-disk files. */
int vfs_add(int parent, const char *name, int is_dir, unsigned size);

const VfsNode *vfs_node(int id); /* NULL if id is not a used node */
int vfs_count(void);             /* used nodes, including the root */

/* Read the folder again from the drive (new, changed and vanished entries; ids of entries that are still
 * there stay the same). Does nothing for RAM-disk folders. */
int vfs_refresh(int dir);

/* The children of `dir` into out[] (at most max), folders first, then by name (ignoring case).
 * Returns how many there are in total (may be more than max). */
int vfs_list(int dir, int *out, int max);

#define VFS_SORT_NAME 0
#define VFS_SORT_SIZE 1
#define VFS_SORT_TYPE 2 /* by extension */

/* Same as vfs_list(), in another order. Folders always come first. `descending` reverses the order
 * inside the folder and file groups; with SIZE or TYPE, entries that tie (and all folders) go by name. */
int vfs_list_by(int dir, int *out, int max, int key, int descending);

/* "/Documents/readme.txt" for a node ("/" for the root). Always 0-terminated, cut if cap is too small.
 * Returns the length written. */
int vfs_path(int id, char *out, int cap);

/* Find a node by absolute path ("/", "/RAM Disk", "/RAM Disk/Documents/readme.txt"). -1 if not found. */
int vfs_lookup(const char *path);

/* Rename a node. -1 if the name is empty / too long / has '/', a sibling already has it (ignoring case),
 * the node is the root or a volume, or the drive refuses. Giving a node the same name with other capitals
 * is allowed. */
int vfs_rename(int id, const char *name);

/* Delete a node and everything below it. Returns how many nodes were removed, -1 for the root, a volume or
 * a bad id, or when the drive refuses (some of it may be gone then). */
int vfs_remove(int id);

/* How many nodes lie below `id` (0 for a file or an empty folder); reads folders on drives as needed. */
int vfs_subtree_count(int id);

/* Copy a node (a folder with everything in it) into folder `dest`. A name already used there becomes
 * "name (2).ext". Returns the id of the copy, or -1: not enough room for all of it (nothing is left
 * behind), `dest` is not a folder, `dest` is the node itself or lies below it, or the RAM Disk is on one
 * side and a drive on the other. */
int vfs_copy(int id, int dest);

/* Move a node into folder `dest`. A name already used there becomes "name (2).ext". Returns the node's id
 * (the same id inside one volume; between two drives the entry is copied and the old one deleted, so the id
 * changes), also when it is already in `dest`. -1 for the root, a volume, a bad id, a `dest` that is not a
 * folder or lies inside the node itself, or a refusal of the drive. */
int vfs_move(int id, int dest);

/* Read bytes of a file on a drive. Returns the count (0 at the end) or -1 (RAM-disk files have no content). */
int vfs_read(int id, unsigned offset, void *buf, unsigned len);

/* Write bytes of a file on a drive at `offset` (the file grows when written past its end; it never shrinks).
 * Returns the count written or -1 (RAM-disk files have no content: VFS_ERR_UNSUPPORTED). */
int vfs_write(int id, unsigned offset, const void *buf, unsigned len);

/* Make file `id` (on a drive) `size` bytes long by cutting off the end. 0, or -1 (VFS_ERR_UNSUPPORTED if the
 * drive cannot, VFS_ERR_ARG if `size` is not smaller than the file). */
int vfs_truncate(int id, unsigned size);

/* Create file `name` in folder `parent` (on a drive) with exactly buf[0,len) as its content, or replace the
 * file of that name. Returns the file's id or -1. A replaced file is only deleted after the new content was
 * written in full (to "name.tmp" first), so a failure keeps the old file; if that last step fails the text is
 * left in "name.tmp". A drive that cannot delete the old file (NTFS: hard links ...) but can shorten it gets the
 * new text written over the old file instead (the id of the old file is returned). */
int vfs_save(int parent, const char *name, const void *buf, unsigned len);
/* After vfs_save returned -1: the step that failed ("create the file", "write the text", "delete the old file",
 * "rename the new file"), "" if it failed before any (bad name, not a folder ...). With vfs_last_error() this says
 * exactly what a drive refused. */
const char *vfs_save_step(void);

/* 1 if `id` is `ancestor` or lies below it. */
int vfs_is_under(int id, int ancestor);

/* Why the last call that returned -1 failed (VFS_ERR_*), and a sentence for it. */
int vfs_last_error(void);
const char *vfs_error_text(int code);

#endif
