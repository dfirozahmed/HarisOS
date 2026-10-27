/* Virtual file system: reading folders from a drive, attaching, mounting volumes. */
#include "vfs_internal.h"

/* ---- reading folders from a drive ------------------------------- */
static void load_dir(int dir)
{
    const VfsBackend *b = ops_of(dir);
    NODES[dir].truncated = 0;
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == dir)
            NODES[i].seen = 0;
    int r = b->list ? b->list(ctx_of(dir), dir) : 0;
    if (r < 0)
    {
        LAST_ERR = r;
        return; /* not marked loaded: the next call tries again */
    }
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == dir && !NODES[i].seen)
            drop_subtree(i); /* gone from the drive */
    NODES[dir].loaded = 1;
}

void ensure_loaded(int dir)
{
    if (NODES[dir].is_dir && !NODES[dir].loaded && ops_of(dir))
        load_dir(dir);
}

int vfs_attach(int dir, const char *name, int is_dir, unsigned size, const unsigned char *fs)
{
    if (!vfs_node(dir) || !name || !name[0])
        return -1;
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == dir && NODES[i].is_dir == (is_dir != 0) && same_exact(NODES[i].name, name))
        {
            NODES[i].size = is_dir ? 0 : size;
            copy_fs(NODES[i].fs, fs);
            NODES[i].seen = 1;
            return i;
        }
    int id = new_node(dir, name, is_dir, size, fs);
    if (id < 0)
    {
        NODES[dir].truncated = 1;
        return -1;
    }
    NODES[id].loaded = !is_dir; /* a folder on a drive is read when it is opened */
    NODES[id].seen = 1;
    return id;
}

int vfs_refresh(int dir)
{
    if (!vfs_node(dir) || !NODES[dir].is_dir)
        return fail(VFS_ERR_ARG);
    if (!ops_of(dir))
        return 0;
    NODES[dir].loaded = 0;
    load_dir(dir);
    return NODES[dir].loaded ? 0 : -1;
}

/* ---- volumes ----------------------------------------------------- */
void unique_name(int dir, const char *name, int is_dir, char *out);

int vfs_mount(const VfsBackend *ops, void *ctx, const char *name)
{
    if (NVOLS >= VFS_MAX_VOLUMES || name_len(name) < 0)
        return -1;
    char unique[VFS_NAME_MAX];
    unique_name(VFS_ROOT, name, 1, unique);
    int id = new_node(VFS_ROOT, unique, 1, 0, 0);
    if (id < 0)
        return -1;
    int v = NVOLS++;
    VOLS[v].ops = ops;
    VOLS[v].ctx = ctx;
    VOLS[v].root = id;
    NODES[id].vol = v;
    GEN++;
    NODES[id].loaded = !(ops && ops->list); /* a drive's root is read on first use */
    return v;
}

/* A drive went away (a USB stick was pulled out): its folder and everything below it leave the tree, and the
 * volumes after it move up one place (node.vol numbers follow). Ids of other nodes do not change. */
int vfs_unmount(void *ctx)
{
    int v = -1;
    for (int i = 0; i < NVOLS && v < 0; i++)
        if (VOLS[i].ctx == ctx && VOLS[i].ops && VOLS[i].ops->list)
            v = i;
    if (v < 0)
        return -1;
    drop_subtree(VOLS[v].root);
    for (int i = v + 1; i < NVOLS; i++)
    {
        VOLS[i - 1].ops = VOLS[i].ops;
        VOLS[i - 1].ctx = VOLS[i].ctx;
        VOLS[i - 1].root = VOLS[i].root;
    }
    NVOLS--;
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].vol > v)
            NODES[i].vol--;
    GEN++;
    return 0;
}

unsigned vfs_generation(void) { return GEN; }

int vfs_volume_count(void) { return NVOLS; }
int vfs_volume_node(int vol) { return (vol >= 0 && vol < NVOLS) ? VOLS[vol].root : -1; }
int vfs_volume_of(int id) { return vfs_node(id) ? NODES[id].vol : -1; }

int vfs_volume_info(int vol, VfsVolInfo *out)
{
    if (vol < 0 || vol >= NVOLS)
        return -1;
    out->node = VOLS[vol].root;
    copy_name(out->name, NODES[VOLS[vol].root].name);
    out->fs_name[0] = 0;
    out->device[0] = 0;
    out->total = out->free = 0;
    out->writable = 0;
    out->locked = 0;
    out->lockable = 0;
    if (VOLS[vol].ops && VOLS[vol].ops->info)
        VOLS[vol].ops->info(VOLS[vol].ctx, out);
    return 0;
}

int vfs_set_locked(int vol, int locked)
{
    if (vol < 0 || vol >= NVOLS || !VOLS[vol].ops || !VOLS[vol].ops->set_locked)
        return -1;
    return VOLS[vol].ops->set_locked(VOLS[vol].ctx, locked) == 0 ? 0 : -1;
}
