/* Mounts the FAT32 volumes found on the disks into the virtual file system. See include/volume.h.
 *
 * Each volume is a VfsBackend whose functions call fat32.c. A node of a FAT volume keeps its FatEntry (where
 * the entry lives on the disk) in node->fs. The volume's root folder is the child of "This PC"; its
 * FatEntry is not used, the root cluster comes from the volume instead. */
#include "blk.h"
#include "fat32.h"
#include "vfs.h"
#include "volume.h"

typedef struct
{
    Fat fat;
    int blk;
    char fs_name[12];
} FatVolume;

static FatVolume VOLUMES[VFS_MAX_VOLUMES];
static int NVOLUMES;
static int UNSUPPORTED;

_Static_assert(sizeof(FatEntry) <= VFS_FS_BYTES, "FatEntry must fit in a node");

static void copy_bytes(void *dst, const void *src, unsigned long n)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
}

static void get_entry(const VfsNode *n, FatEntry *e) { copy_bytes(e, n->fs, sizeof *e); }
static void set_entry(VfsNode *n, const FatEntry *e) { copy_bytes(n->fs, e, sizeof *e); }

/* the folder's cluster: a volume's root folder is a child of the top-level root */
static unsigned dir_cluster(const FatVolume *v, const VfsNode *n)
{
    if (n->parent == VFS_ROOT)
        return fat_root(&v->fat);
    FatEntry e;
    get_entry(n, &e);
    return e.first_cluster;
}

/* ---- the backend ------------------------------------------------ */
static int list_cb(const char *name, const FatEntry *e, void *user)
{
    int dir = *(int *)user;
    unsigned char fs[VFS_FS_BYTES];
    for (int i = 0; i < VFS_FS_BYTES; i++)
        fs[i] = 0;
    copy_bytes(fs, e, sizeof *e);
    return vfs_attach(dir, name, (e->attr & FAT_ATTR_DIR) != 0, e->size, fs) < 0; /* stop when the table is full */
}

static int be_list(void *ctx, int dir)
{
    FatVolume *v = (FatVolume *)ctx;
    int r = fat_list(&v->fat, dir_cluster(v, vfs_node(dir)), list_cb, &dir);
    return r < 0 ? r : 0;
}

static int be_create(void *ctx, const VfsNode *parent, const char *name, int is_dir, unsigned char *fs_out)
{
    FatVolume *v = (FatVolume *)ctx;
    FatEntry e;
    int r = fat_create(&v->fat, dir_cluster(v, parent), name, is_dir, &e);
    if (r < 0)
        return r;
    for (int i = 0; i < VFS_FS_BYTES; i++)
        fs_out[i] = 0;
    copy_bytes(fs_out, &e, sizeof e);
    return 0;
}

static int be_remove(void *ctx, const VfsNode *n)
{
    FatEntry e;
    get_entry(n, &e);
    return fat_delete(&((FatVolume *)ctx)->fat, &e);
}

static int be_rename(void *ctx, VfsNode *n, const char *new_name)
{
    FatEntry e;
    get_entry(n, &e);
    int r = fat_rename(&((FatVolume *)ctx)->fat, &e, new_name);
    if (r == 0)
        set_entry(n, &e);
    return r;
}

static int be_move(void *ctx, VfsNode *n, const char *name, const VfsNode *new_parent)
{
    FatVolume *v = (FatVolume *)ctx;
    FatEntry e;
    get_entry(n, &e);
    int r = fat_move(&v->fat, &e, name, dir_cluster(v, new_parent));
    if (r == 0)
        set_entry(n, &e);
    return r;
}

static int be_read(void *ctx, const VfsNode *n, unsigned offset, void *buf, unsigned len)
{
    FatEntry e;
    get_entry(n, &e);
    return fat_read(&((FatVolume *)ctx)->fat, &e, offset, buf, len);
}

static int be_write(void *ctx, VfsNode *n, unsigned offset, const void *buf, unsigned len)
{
    FatEntry e;
    get_entry(n, &e);
    int r = fat_write(&((FatVolume *)ctx)->fat, &e, offset, buf, len);
    if (r >= 0)
    {
        set_entry(n, &e);
        n->size = e.size;
    }
    return r;
}

static void put(char *dst, int cap, const char *src)
{
    int i = 0;
    while (src[i] && i < cap - 1)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void be_info(void *ctx, VfsVolInfo *out)
{
    FatVolume *v = (FatVolume *)ctx;
    const BlkDev *p = blk_get(v->blk);
    const BlkDev *disk = (p && p->parent >= 0) ? blk_get(p->parent) : p;
    put(out->fs_name, sizeof out->fs_name, v->fs_name);
    char dev[24];
    int n = 0;
    const char *medium = (disk && disk->media == BLK_MEDIA_SSD)   ? "SSD"
                         : (disk && disk->media == BLK_MEDIA_HDD) ? "HDD"
                                                                  : "Disk";
    const char *bus = (disk && disk->kind == BLK_NVME) ? "NVMe" : (disk && disk->kind == BLK_SATA) ? "SATA" : "RAM";
    for (const char *s = medium; *s && n < 20; s++)
        dev[n++] = *s;
    dev[n++] = ' ';
    dev[n++] = '(';
    for (const char *s = bus; *s && n < 22; s++)
        dev[n++] = *s;
    dev[n++] = ')';
    dev[n] = 0;
    put(out->device, sizeof out->device, dev);
    if (!fat_space(&v->fat, &out->total, &out->free))
        out->free = (unsigned long long)-1;
    out->writable = disk && disk->writable;
    out->locked = disk ? disk->locked : 1;
    out->lockable = 1;
}

static int be_set_locked(void *ctx, int locked)
{
    FatVolume *v = (FatVolume *)ctx;
    if (!locked)
        v->fat.cur_first = 0; /* start from a clean cursor after a pause */
    blk_set_locked(v->blk, locked);
    return 0;
}

static void be_batch(void *ctx, int on) { fat_batch(&((FatVolume *)ctx)->fat, on); }

static const VfsBackend FAT_BACKEND = {be_list, be_create, be_remove, be_rename, be_move, be_read, be_write, be_info,
                                       be_set_locked, be_batch};

/* ---- finding the volumes ------------------------------------------ */
/* "HARISOS (nvme0p1)": the volume label, else the partition's name, else "Local Disk" */
static void volume_name(const FatVolume *v, const BlkDev *d, char *out, int cap)
{
    const char *base = v->fat.label[0] ? v->fat.label : (d->part_label[0] ? d->part_label : "Local Disk");
    int n = 0;
    for (const char *s = base; *s && n < cap - 16; s++)
        out[n++] = *s;
    out[n++] = ' ';
    out[n++] = '(';
    for (const char *s = d->name; *s && n < cap - 2; s++)
        out[n++] = *s;
    out[n++] = ')';
    out[n] = 0;
}

int volume_unsupported_count(void) { return UNSUPPORTED; }

int volume_mount_all(void)
{
    NVOLUMES = 0;
    UNSUPPORTED = 0;
    int n = blk_count();
    for (int i = 0; i < n; i++)
    {
        const BlkDev *d = blk_get(i);
        if (d->kind != BLK_PART) /* a whole disk: only when it has no partitions */
        {
            int has_parts = 0;
            for (int j = 0; j < n; j++)
                if (blk_get(j)->parent == i)
                    has_parts = 1;
            if (has_parts)
                continue;
        }
        if (d->part_kind == PART_LINUX || d->part_kind == PART_OTHER)
        {
            UNSUPPORTED++;
            continue;
        }
        if (NVOLUMES >= VFS_MAX_VOLUMES)
            break;
        FatVolume *v = &VOLUMES[NVOLUMES];
        if (fat_mount(&v->fat, i) != 0)
        {
            UNSUPPORTED++; /* NTFS, exFAT, FAT16, damaged, ... */
            continue;
        }
        v->blk = i;
        put(v->fs_name, sizeof v->fs_name, "FAT32");
        char name[64];
        volume_name(v, d, name, sizeof name);
        if (vfs_mount(&FAT_BACKEND, v, name) < 0)
            break;
        NVOLUMES++;
    }
    return NVOLUMES;
}
