/* Virtual file system: state, error texts, name helpers, the node table. */
#include "vfs_internal.h"

/* Virtual file system: the node tree, the RAM Disk and the glue to drive backends. See include/vfs.h. */

VfsNode NODES[VFS_MAX_NODES];


Volume VOLS[VFS_MAX_VOLUMES];
int NVOLS;
unsigned GEN; /* grows whenever a volume is mounted or unmounted */
int LAST_ERR;

int fail(int code)
{
    LAST_ERR = code;
    return -1;
}

int vfs_last_error(void) { return LAST_ERR; }

const char *vfs_error_text(int code)
{
    switch (code)
    {
    case VFS_ERR_IO:
        return "The drive could not be read or written.";
    case VFS_ERR_FULL:
        return "There is not enough room on the drive.";
    case VFS_ERR_EXISTS:
        return "That name is already used here.";
    case VFS_ERR_RO:
        return "This drive is locked (read-only). Click the lock in the status bar to unlock it.";
    case VFS_ERR_NOTEMPTY:
        return "The folder is not empty.";
    case VFS_ERR_BADNAME:
        return "That name is empty, too long or has characters that are not allowed.";
    case VFS_ERR_ARG:
        return "That cannot be done here.";
    case VFS_ERR_UNSUPPORTED:
        return "Copying between the RAM Disk and a drive is not supported.";
    case VFS_ERR_NOTREADABLE:
        return "This file is compressed or encrypted: HarisOS cannot read it yet.";
    case VFS_ERR_NOWRITE:
        return "HarisOS can only read this drive, not change it.";
    case VFS_ERR_DIRTY:
        return "This NTFS drive was not shut down cleanly (or Windows fast startup is on). Run chkdsk in Windows first.";
    default:
        return "That did not work.";
    }
}

int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int same_name(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
        a++, b++;
    return *a == *b;
}

int same_exact(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

void copy_name(char *dst, const char *src)
{
    int i = 0;
    for (; src[i] && i < VFS_NAME_MAX - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

void copy_fs(unsigned char *dst, const unsigned char *src)
{
    for (int i = 0; i < VFS_FS_BYTES; i++)
        dst[i] = src ? src[i] : 0;
}

/* ---- the node table --------------------------------------------- */
void vfs_reset(void)
{
    for (int i = 0; i < VFS_MAX_NODES; i++)
    {
        NODES[i].used = 0;
        NODES[i].name[0] = 0;
    }
    NODES[VFS_ROOT].used = 1;
    NODES[VFS_ROOT].is_dir = 1;
    NODES[VFS_ROOT].size = 0;
    NODES[VFS_ROOT].parent = -1;
    NODES[VFS_ROOT].vol = -1;
    NODES[VFS_ROOT].loaded = 1;
    NODES[VFS_ROOT].truncated = 0;
    NODES[VFS_ROOT].seen = 0;
    copy_fs(NODES[VFS_ROOT].fs, 0);
    NVOLS = 0;
    LAST_ERR = 0;
}

const VfsNode *vfs_node(int id)
{
    return (id >= 0 && id < VFS_MAX_NODES && NODES[id].used) ? &NODES[id] : (const VfsNode *)0;
}

int vfs_count(void)
{
    int n = 0;
    for (int i = 0; i < VFS_MAX_NODES; i++)
        n += NODES[i].used;
    return n;
}

int free_slot(void)
{
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (!NODES[i].used)
            return i;
    return -1;
}

/* a node in memory only: no drive is asked */
int new_node(int parent, const char *name, int is_dir, unsigned size, const unsigned char *fs)
{
    int i = free_slot();
    if (i < 0)
        return -1;
    copy_name(NODES[i].name, name);
    NODES[i].used = 1;
    NODES[i].is_dir = is_dir != 0;
    NODES[i].size = is_dir ? 0 : size;
    NODES[i].parent = parent;
    NODES[i].vol = NODES[parent].vol;
    NODES[i].loaded = 1;
    NODES[i].truncated = 0;
    NODES[i].seen = 0;
    copy_fs(NODES[i].fs, fs);
    return i;
}

void drop_subtree(int id)
{
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == id)
            drop_subtree(i);
    NODES[id].used = 0;
    NODES[id].name[0] = 0;
}

/* the backend that does the work for this node, or NULL for the RAM Disk and the root */
const VfsBackend *ops_of(int id)
{
    int v = NODES[id].vol;
    if (v < 0 || v >= NVOLS)
        return (const VfsBackend *)0;
    return (VOLS[v].ops && VOLS[v].ops->list) ? VOLS[v].ops : (const VfsBackend *)0;
}

void *ctx_of(int id) { return VOLS[NODES[id].vol].ctx; }

void batch(int id, int on)
{
    const VfsBackend *b = ops_of(id);
    if (b && b->batch)
        b->batch(ctx_of(id), on);
}

int vfs_is_volume_root(int id)
{
    for (int v = 0; v < NVOLS; v++)
        if (VOLS[v].root == id)
            return 1;
    return 0;
}

int find_child(int dir, const char *name)
{
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == dir && same_name(NODES[i].name, name))
            return i;
    return -1;
}

/* length of a usable name, or -1 (empty, too long, contains '/') */
int name_len(const char *name)
{
    if (!name || !name[0])
        return -1;
    int len = 0;
    while (name[len])
    {
        if (name[len] == '/' || len >= VFS_NAME_MAX - 1)
            return -1;
        len++;
    }
    return len;
}
