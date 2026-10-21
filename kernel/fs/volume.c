/* Mounts the file systems found on the disks into the virtual file system. See include/volume.h.
 *
 * Three kinds of volume, each a VfsBackend:
 *   FAT32   read and write (fat32.c)
 *   exFAT   read and write (exfat.c)
 *   NTFS    read and write (ntfs.c)
 * A node of a volume keeps the file system's own locator (FatEntry / ExEntry / NtEntry: where the entry lives
 * on the disk) in node->fs. The volume's root folder is a child of "This PC"; its locator is not stored, the
 * root folder of the file system is used instead. */
#include "blk.h"
#include "exfat.h"
#include "fat32.h"
#include "ntfs.h"
#include "vfs.h"
#include "volume.h"

enum
{
    FS_FAT32,
    FS_EXFAT,
    FS_NTFS
};

typedef struct
{
    int blk;
    int kind;
    char fs_name[12];
    union
    {
        Fat fat;
        ExFat ex;
        Ntfs nt;
    } u;
} Drive;

static Drive DRIVES[VFS_MAX_VOLUMES];
static int NDRIVES;
static int UNSUPPORTED;

_Static_assert(sizeof(FatEntry) <= VFS_FS_BYTES, "FatEntry must fit in a node");
_Static_assert(sizeof(ExEntry) <= VFS_FS_BYTES, "ExEntry must fit in a node");
_Static_assert(sizeof(NtEntry) <= VFS_FS_BYTES, "NtEntry must fit in a node");

static void copy_bytes(void *dst, const void *src, unsigned long n)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
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

/* a node's locator out of / into its fs bytes */
#define GET(T, n, e) copy_bytes((e), (n)->fs, sizeof(T))
#define SET(T, n, e) copy_bytes((n)->fs, (e), sizeof(T))

static int is_volume_root(const VfsNode *n) { return n->parent == VFS_ROOT; }

/* the list callbacks hand each entry to the VFS */
static int attach(int dir, const char *name, int is_dir, unsigned size, const void *loc, unsigned long loc_len)
{
    unsigned char fs[VFS_FS_BYTES];
    for (int i = 0; i < VFS_FS_BYTES; i++)
        fs[i] = 0;
    copy_bytes(fs, loc, loc_len);
    return vfs_attach(dir, name, is_dir, size, fs) < 0; /* non-zero stops the listing: the node table is full */
}

/* ====================== FAT32 (read and write) ====================== */
static unsigned fat_dir_cluster(const Drive *v, const VfsNode *n)
{
    if (is_volume_root(n))
        return fat_root(&v->u.fat);
    FatEntry e;
    GET(FatEntry, n, &e);
    return e.first_cluster;
}

static int fat_list_cb(const char *name, const FatEntry *e, void *user)
{
    return attach(*(int *)user, name, (e->attr & FAT_ATTR_DIR) != 0, e->size, e, sizeof *e);
}

static int fat_be_list(void *ctx, int dir)
{
    Drive *v = (Drive *)ctx;
    int r = fat_list(&v->u.fat, fat_dir_cluster(v, vfs_node(dir)), fat_list_cb, &dir);
    return r < 0 ? r : 0;
}

static int fat_be_create(void *ctx, VfsNode *parent, const char *name, int is_dir, unsigned char *fs_out)
{
    Drive *v = (Drive *)ctx;
    FatEntry e;
    int r = fat_create(&v->u.fat, fat_dir_cluster(v, parent), name, is_dir, &e);
    if (r < 0)
        return r;
    for (int i = 0; i < VFS_FS_BYTES; i++)
        fs_out[i] = 0;
    copy_bytes(fs_out, &e, sizeof e);
    return 0;
}

static int fat_be_remove(void *ctx, const VfsNode *n)
{
    FatEntry e;
    GET(FatEntry, n, &e);
    return fat_delete(&((Drive *)ctx)->u.fat, &e);
}

static int fat_be_rename(void *ctx, VfsNode *n, const char *new_name)
{
    FatEntry e;
    GET(FatEntry, n, &e);
    int r = fat_rename(&((Drive *)ctx)->u.fat, &e, new_name);
    if (r == 0)
        SET(FatEntry, n, &e);
    return r;
}

static int fat_be_move(void *ctx, VfsNode *n, const char *name, VfsNode *new_parent)
{
    Drive *v = (Drive *)ctx;
    FatEntry e;
    GET(FatEntry, n, &e);
    int r = fat_move(&v->u.fat, &e, name, fat_dir_cluster(v, new_parent));
    if (r == 0)
        SET(FatEntry, n, &e);
    return r;
}

static int fat_be_read(void *ctx, const VfsNode *n, unsigned offset, void *buf, unsigned len)
{
    FatEntry e;
    GET(FatEntry, n, &e);
    return fat_read(&((Drive *)ctx)->u.fat, &e, offset, buf, len);
}

static int fat_be_write(void *ctx, VfsNode *n, unsigned offset, const void *buf, unsigned len)
{
    FatEntry e;
    GET(FatEntry, n, &e);
    int r = fat_write(&((Drive *)ctx)->u.fat, &e, offset, buf, len);
    if (r >= 0)
    {
        SET(FatEntry, n, &e);
        n->size = e.size;
    }
    return r;
}

static void be_batch(void *ctx, int on) { fat_batch(&((Drive *)ctx)->u.fat, on); }

/* the part of info() every file system shares: its name and the medium it is on */
static const BlkDev *disk_of(const Drive *v)
{
    const BlkDev *p = blk_get(v->blk);
    return (p && p->parent >= 0) ? blk_get(p->parent) : p;
}

static void common_info(const Drive *v, VfsVolInfo *out)
{
    const BlkDev *disk = disk_of(v);
    put(out->fs_name, sizeof out->fs_name, v->fs_name);
    const char *medium = (disk && disk->media == BLK_MEDIA_SSD)   ? "SSD"
                         : (disk && disk->media == BLK_MEDIA_HDD) ? "HDD"
                                                                  : "Disk";
    const char *bus = (disk && disk->kind == BLK_NVME) ? "NVMe" : (disk && disk->kind == BLK_SATA) ? "SATA" : "RAM";
    char dev[24];
    int n = 0;
    for (const char *s = medium; *s && n < 20; s++)
        dev[n++] = *s;
    dev[n++] = ' ';
    dev[n++] = '(';
    for (const char *s = bus; *s && n < 22; s++)
        dev[n++] = *s;
    dev[n++] = ')';
    dev[n] = 0;
    put(out->device, sizeof out->device, dev);
}

static void fat_be_info(void *ctx, VfsVolInfo *out)
{
    Drive *v = (Drive *)ctx;
    const BlkDev *disk = disk_of(v);
    common_info(v, out);
    if (!fat_space(&v->u.fat, &out->total, &out->free))
        out->free = (unsigned long long)-1;
    out->writable = disk && disk->writable;
    out->locked = disk ? disk->locked : 1;
    out->lockable = 1;
}

static int be_set_locked(void *ctx, int locked)
{
    blk_set_locked(((Drive *)ctx)->blk, locked);
    return 0;
}

static const VfsBackend FAT_BACKEND = {fat_be_list, fat_be_create, fat_be_remove, fat_be_rename, fat_be_move,
                                       fat_be_read, fat_be_write,   fat_be_info,   be_set_locked, be_batch};

/* ====================== exFAT (read and write) ====================== */
static void ex_dir(const Drive *v, const VfsNode *n, ExEntry *e)
{
    if (is_volume_root(n))
        exfat_root(&v->u.ex, e);
    else
        GET(ExEntry, n, e);
}

/* The folder `dir_node` and the folder it is in (NULL for a volume's root): exfat.c needs both, because a folder that
 * grows changes its own entry in its parent. Returns 1 if there is a parent. */
static int ex_container(const Drive *v, const VfsNode *dir_node, ExEntry *dir, ExEntry *dir_parent)
{
    ex_dir(v, dir_node, dir);
    if (is_volume_root(dir_node))
        return 0;
    ex_dir(v, vfs_node(dir_node->parent), dir_parent);
    return 1;
}

/* store a folder's changed locator (it grew); a volume's root has none */
static void ex_store_dir(VfsNode *dir_node, const ExEntry *dir)
{
    if (!is_volume_root(dir_node))
        SET(ExEntry, dir_node, dir);
}

static int ex_be_create(void *ctx, VfsNode *parent, const char *name, int is_dir, unsigned char *fs_out)
{
    Drive *v = (Drive *)ctx;
    ExEntry dir, dp, e;
    int has_parent = ex_container(v, parent, &dir, &dp);
    int r = exfat_create(&v->u.ex, &dir, has_parent ? &dp : 0, name, is_dir, &e);
    ex_store_dir(parent, &dir);
    if (r < 0)
        return r;
    for (int i = 0; i < VFS_FS_BYTES; i++)
        fs_out[i] = 0;
    copy_bytes(fs_out, &e, sizeof e);
    return 0;
}

static int ex_be_remove(void *ctx, const VfsNode *n)
{
    Drive *v = (Drive *)ctx;
    ExEntry dir, dp, e;
    ex_container(v, vfs_node(n->parent), &dir, &dp);
    GET(ExEntry, n, &e);
    return exfat_delete(&v->u.ex, &dir, &e);
}

static int ex_be_rename(void *ctx, VfsNode *n, const char *new_name)
{
    Drive *v = (Drive *)ctx;
    VfsNode *pn = (VfsNode *)vfs_node(n->parent);
    ExEntry dir, dp, e;
    int has_parent = ex_container(v, pn, &dir, &dp);
    GET(ExEntry, n, &e);
    int r = exfat_rename(&v->u.ex, &dir, has_parent ? &dp : 0, &e, new_name);
    ex_store_dir(pn, &dir);
    if (r == 0)
        SET(ExEntry, n, &e);
    return r;
}

static int ex_be_move(void *ctx, VfsNode *n, const char *name, VfsNode *new_parent)
{
    Drive *v = (Drive *)ctx;
    ExEntry old_dir, old_dp, new_dir, new_dp, e;
    ex_container(v, vfs_node(n->parent), &old_dir, &old_dp);
    int has_parent = ex_container(v, new_parent, &new_dir, &new_dp);
    GET(ExEntry, n, &e);
    int r = exfat_move(&v->u.ex, &old_dir, &new_dir, has_parent ? &new_dp : 0, &e, name);
    ex_store_dir(new_parent, &new_dir);
    if (r == 0)
        SET(ExEntry, n, &e);
    return r;
}

static int ex_be_write(void *ctx, VfsNode *n, unsigned offset, const void *buf, unsigned len)
{
    Drive *v = (Drive *)ctx;
    ExEntry dir, dp, e;
    ex_container(v, vfs_node(n->parent), &dir, &dp);
    GET(ExEntry, n, &e);
    int r = exfat_write(&v->u.ex, &dir, &e, offset, buf, len);
    if (r >= 0)
    {
        SET(ExEntry, n, &e);
        n->size = e.size;
    }
    return r;
}

static void ex_be_batch(void *ctx, int on) { exfat_batch(&((Drive *)ctx)->u.ex, on); }

static int ex_list_cb(const char *name, const ExEntry *e, void *user)
{
    return attach(*(int *)user, name, (e->attr & EXFAT_ATTR_DIR) != 0, e->size, e, sizeof *e);
}

static int ex_be_list(void *ctx, int dir)
{
    Drive *v = (Drive *)ctx;
    ExEntry d;
    ex_dir(v, vfs_node(dir), &d);
    int r = exfat_list(&v->u.ex, &d, ex_list_cb, &dir);
    return r < 0 ? VFS_ERR_IO : 0;
}

static int ex_be_read(void *ctx, const VfsNode *n, unsigned offset, void *buf, unsigned len)
{
    ExEntry e;
    GET(ExEntry, n, &e);
    int r = exfat_read(&((Drive *)ctx)->u.ex, &e, offset, buf, len);
    return r < 0 ? VFS_ERR_IO : r;
}

static void ex_be_info(void *ctx, VfsVolInfo *out)
{
    Drive *v = (Drive *)ctx;
    common_info(v, out);
    const BlkDev *disk = disk_of(v);
    if (!exfat_space(&v->u.ex, &out->total, &out->free))
        out->free = (unsigned long long)-1;
    out->writable = disk && disk->writable;
    out->locked = disk ? disk->locked : 1;
    out->lockable = 1;
}

static const VfsBackend EXFAT_BACKEND = {ex_be_list,  ex_be_create, ex_be_remove, ex_be_rename, ex_be_move,
                                         ex_be_read,  ex_be_write,  ex_be_info,   be_set_locked, ex_be_batch};

/* ====================== NTFS (read and write) ====================== */
/* NTFS_ERR_* are the same numbers as VFS_ERR_*, except that a dirty volume has a code of its own */
static int nt_err(int r) { return r == NTFS_ERR_DIRTY ? VFS_ERR_DIRTY : r; }

static void nt_dir(const VfsNode *n, NtEntry *e)
{
    if (is_volume_root(n))
        ntfs_root(e);
    else
        GET(NtEntry, n, e);
}

static int nt_list_cb(const char *name, const NtEntry *e, void *user)
{
    return attach(*(int *)user, name, e->is_dir != 0, e->size, e, sizeof *e);
}

static int nt_be_list(void *ctx, int dir)
{
    Drive *v = (Drive *)ctx;
    NtEntry d;
    nt_dir(vfs_node(dir), &d);
    int r = ntfs_list(&v->u.nt, &d, nt_list_cb, &dir);
    return r < 0 ? VFS_ERR_IO : 0;
}

static int nt_be_create(void *ctx, VfsNode *parent, const char *name, int is_dir, unsigned char *fs_out)
{
    NtEntry d, e;
    nt_dir(parent, &d);
    int r = ntfs_create(&((Drive *)ctx)->u.nt, &d, name, is_dir, &e);
    if (r < 0)
        return nt_err(r);
    for (int i = 0; i < VFS_FS_BYTES; i++)
        fs_out[i] = 0;
    copy_bytes(fs_out, &e, sizeof e);
    return 0;
}

static int nt_be_remove(void *ctx, const VfsNode *n)
{
    NtEntry e;
    GET(NtEntry, n, &e);
    return nt_err(ntfs_delete(&((Drive *)ctx)->u.nt, &e));
}

static int nt_be_rename(void *ctx, VfsNode *n, const char *new_name)
{
    NtEntry e;
    GET(NtEntry, n, &e);
    int r = ntfs_rename(&((Drive *)ctx)->u.nt, &e, new_name);
    if (r == 0)
        SET(NtEntry, n, &e);
    return nt_err(r);
}

static int nt_be_move(void *ctx, VfsNode *n, const char *name, VfsNode *new_parent)
{
    NtEntry e, d;
    GET(NtEntry, n, &e);
    nt_dir(new_parent, &d);
    int r = ntfs_move(&((Drive *)ctx)->u.nt, &e, &d, name);
    if (r == 0)
        SET(NtEntry, n, &e);
    return nt_err(r);
}

static int nt_be_read(void *ctx, const VfsNode *n, unsigned offset, void *buf, unsigned len)
{
    NtEntry e;
    GET(NtEntry, n, &e);
    int r = ntfs_read(&((Drive *)ctx)->u.nt, &e, offset, buf, len);
    return r == -2 ? VFS_ERR_NOTREADABLE : r < 0 ? VFS_ERR_IO : r;
}

static int nt_be_write(void *ctx, VfsNode *n, unsigned offset, const void *buf, unsigned len)
{
    NtEntry e;
    GET(NtEntry, n, &e);
    int r = ntfs_write(&((Drive *)ctx)->u.nt, &e, offset, buf, len);
    if (r >= 0)
    {
        SET(NtEntry, n, &e);
        n->size = e.size;
    }
    return nt_err(r);
}

static void nt_be_info(void *ctx, VfsVolInfo *out)
{
    Drive *v = (Drive *)ctx;
    const BlkDev *disk = disk_of(v);
    common_info(v, out);
    if (!ntfs_space(&v->u.nt, &out->total, &out->free))
        out->free = (unsigned long long)-1;
    out->writable = disk && disk->writable;
    out->locked = disk ? disk->locked : 1;
    out->lockable = 1;
}

static const VfsBackend NTFS_BACKEND = {nt_be_list, nt_be_create, nt_be_remove, nt_be_rename, nt_be_move,
                                        nt_be_read, nt_be_write,  nt_be_info,   be_set_locked, 0};

/* ====================== finding the volumes ====================== */
/* "HARISOS (nvme0p1)": the volume label, else the partition's name, else "Local Disk" */
static void volume_name(const char *label, const BlkDev *d, char *out, int cap)
{
    const char *base = label[0] ? label : (d->part_label[0] ? d->part_label : "Local Disk");
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

static unsigned char BOOT[4096];

/* which file system does sector 0 of this device look like? -1 = none we know */
static int sniff(int blk)
{
    const BlkDev *d = blk_get(blk);
    if (!d || d->sector_size > sizeof BOOT || blk_read(blk, 0, 1, BOOT) != 0)
        return -1;
    static const char NTFS[8] = {'N', 'T', 'F', 'S', ' ', ' ', ' ', ' '};
    static const char EXFAT[8] = {'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};
    int ntfs = 1, exfat = 1;
    for (int i = 0; i < 8; i++)
    {
        ntfs &= BOOT[3 + i] == (unsigned char)NTFS[i];
        exfat &= BOOT[3 + i] == (unsigned char)EXFAT[i];
    }
    return ntfs ? FS_NTFS : exfat ? FS_EXFAT : FS_FAT32; /* anything else: let the FAT code decide */
}

int volume_mount_all(void)
{
    NDRIVES = 0;
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
        if (NDRIVES >= VFS_MAX_VOLUMES)
            break;
        Drive *v = &DRIVES[NDRIVES];
        int kind = sniff(i);
        const char *label = "";
        const VfsBackend *be = 0;
        v->blk = i;
        v->kind = kind;
        if (kind == FS_EXFAT && exfat_mount(&v->u.ex, i) == 0)
        {
            put(v->fs_name, sizeof v->fs_name, "exFAT");
            label = v->u.ex.label;
            be = &EXFAT_BACKEND;
        }
        else if (kind == FS_NTFS && ntfs_mount(&v->u.nt, i) == 0)
        {
            put(v->fs_name, sizeof v->fs_name, "NTFS");
            label = v->u.nt.label;
            be = &NTFS_BACKEND;
        }
        else if (kind == FS_FAT32 && fat_mount(&v->u.fat, i) == 0)
        {
            put(v->fs_name, sizeof v->fs_name, "FAT32");
            label = v->u.fat.label;
            be = &FAT_BACKEND;
        }
        if (!be)
        {
            UNSUPPORTED++; /* FAT16, ext4, BitLocker, damaged, empty ... */
            continue;
        }
        char name[64];
        volume_name(label, d, name, sizeof name);
        if (vfs_mount(be, v, name) < 0)
            break;
        NDRIVES++;
    }
    return NDRIVES;
}
