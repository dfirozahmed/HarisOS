/* Virtual file system: the RAM Disk, rename, remove, copy and move. */
#include "vfs_internal.h"

/* ---- demo content (the RAM Disk) ---------------------------------- */
static void ram_info(void *ctx, VfsVolInfo *out)
{
    (void)ctx;
    const char *fs = "RAM", *dev = "Demo (memory)";
    int i = 0;
    for (; fs[i]; i++)
        out->fs_name[i] = fs[i];
    out->fs_name[i] = 0;
    for (i = 0; dev[i]; i++)
        out->device[i] = dev[i];
    out->device[i] = 0;
    out->writable = 1;
}

static const VfsBackend RAM_BACKEND = {0, 0, 0, 0, 0, 0, 0, ram_info, 0, 0, 0};

void vfs_init(void)
{
    vfs_reset();
    int v = vfs_mount(&RAM_BACKEND, 0, "RAM Disk");
    int ram = vfs_volume_node(v);
    int docs = vfs_add(ram, "Documents", 1, 0);
    int pics = vfs_add(ram, "Pictures", 1, 0);
    int music = vfs_add(ram, "Music", 1, 0);
    int sys = vfs_add(ram, "System", 1, 0);
    vfs_add(ram, "readme.txt", 0, 1284);

    vfs_add(docs, "notes.txt", 0, 612);
    vfs_add(docs, "todo.md", 0, 238);
    vfs_add(docs, "learning-log.txt", 0, 4410);
    int proj = vfs_add(docs, "Projects", 1, 0);
    vfs_add(proj, "haris.c", 0, 18342);
    vfs_add(proj, "haris.h", 0, 2096);
    vfs_add(proj, "Makefile", 0, 904);

    vfs_add(pics, "wallpaper.bmp", 0, 3145782);
    vfs_add(pics, "logo.png", 0, 48210);
    vfs_add(pics, "screenshot.png", 0, 912044);

    vfs_add(music, "intro.wav", 0, 5292044);
    vfs_add(music, "boot-chime.mp3", 0, 162304);

    int efi = vfs_add(sys, "EFI", 1, 0);
    int boot = vfs_add(efi, "BOOT", 1, 0);
    vfs_add(boot, "BOOTX64.EFI", 0, 11264);
    vfs_add(sys, "KERNEL.EFI", 0, 262144);
    vfs_add(sys, "config.ini", 0, 128);
}

/* ---- rename / remove ------------------------------------------- */
int vfs_rename(int id, const char *name)
{
    const VfsNode *n = vfs_node(id);
    if (!n || id == VFS_ROOT || vfs_is_volume_root(id))
        return fail(VFS_ERR_ARG);
    if (name_len(name) < 0)
        return fail(VFS_ERR_BADNAME);
    const VfsBackend *b = ops_of(id);
    if (b)
        ensure_loaded(n->parent);
    int dup = find_child(n->parent, name);
    if (dup >= 0 && dup != id) /* a case-only change of its own name is fine */
        return fail(VFS_ERR_EXISTS);
    if (b)
    {
        if (!b->rename)
            return fail(VFS_ERR_NOWRITE);
        int r = b->rename(ctx_of(id), &NODES[id], name);
        if (r < 0)
            return fail(r);
    }
    copy_name(NODES[id].name, name);
    return 0;
}

static int subtree_count(int id)
{
    int n = 0;
    ensure_loaded(id);
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == id)
            n += 1 + subtree_count(i);
    return n;
}

int vfs_subtree_count(int id)
{
    if (!vfs_node(id))
        return 0;
    return subtree_count(id);
}

/* delete on a drive: the entries below first (the drive only deletes empty folders), then the node itself */
static int remove_on_drive(int id)
{
    const VfsBackend *b = ops_of(id);
    int removed = 0;
    if (NODES[id].is_dir)
    {
        ensure_loaded(id);
        for (int i = 1; i < VFS_MAX_NODES; i++)
            if (NODES[i].used && NODES[i].parent == id)
            {
                int r = remove_on_drive(i);
                if (r < 0)
                    return -1;
                removed += r;
            }
    }
    int r = b->remove(ctx_of(id), &NODES[id]);
    if (r < 0)
        return fail(r);
    NODES[id].used = 0;
    NODES[id].name[0] = 0;
    return removed + 1;
}

static int remove_in_memory(int id)
{
    int removed = 1;
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == id)
            removed += remove_in_memory(i);
    NODES[id].used = 0;
    NODES[id].name[0] = 0;
    return removed;
}

int vfs_remove(int id)
{
    if (!vfs_node(id) || id == VFS_ROOT || vfs_is_volume_root(id))
        return fail(VFS_ERR_ARG);
    if (!ops_of(id))
        return remove_in_memory(id);
    if (!ops_of(id)->remove)
        return fail(VFS_ERR_NOWRITE);
    batch(id, 1);
    int r = remove_on_drive(id);
    batch(id, 0);
    return r;
}

/* ---- copy / move ----------------------------------------------- */
/* A name for a new entry in `dir` that is not taken yet: `name` itself, else "stem (2).ext", "stem (3).ext" ...
 * (a folder has no extension). The stem is shortened if the result would not fit. */
void unique_name(int dir, const char *name, int is_dir, char *out)
{
    int len = 0;
    while (name[len])
        len++;
    if (find_child(dir, name) < 0)
    {
        for (int k = 0; k <= len; k++)
            out[k] = name[k];
        return;
    }
    int dot = len; /* where the extension starts */
    for (int i = len - 1; i > 0 && !is_dir; i--)
        if (name[i] == '.')
        {
            dot = i;
            break;
        }
    int ext = len - dot;
    for (int n = 2; n < 1000; n++)
    {
        char suffix[8];
        int sl = 0;
        suffix[sl++] = ' ';
        suffix[sl++] = '(';
        if (n >= 100)
            suffix[sl++] = (char)('0' + n / 100);
        if (n >= 10)
            suffix[sl++] = (char)('0' + n / 10 % 10);
        suffix[sl++] = (char)('0' + n % 10);
        suffix[sl++] = ')';
        int stem = dot;
        if (stem + sl + ext > VFS_NAME_MAX - 1)
            stem = VFS_NAME_MAX - 1 - sl - ext;
        if (stem < 1)
            break;
        int o = 0;
        for (int k = 0; k < stem; k++)
            out[o++] = name[k];
        for (int k = 0; k < sl; k++)
            out[o++] = suffix[k];
        for (int k = dot; k < len; k++)
            out[o++] = name[k];
        out[o] = 0;
        if (find_child(dir, out) < 0)
            return;
    }
    for (int k = 0; k <= len; k++) /* cannot happen with a table this size, but never leave out[] unset */
        out[k] = name[k];
}

/* bytes of all files below (and including) a node, for the free-space check before a copy */
static unsigned long long tree_bytes(int id)
{
    unsigned long long n = NODES[id].is_dir ? 0 : NODES[id].size;
    if (NODES[id].is_dir)
    {
        ensure_loaded(id);
        for (int i = 1; i < VFS_MAX_NODES; i++)
            if (NODES[i].used && NODES[i].parent == id)
                n += tree_bytes(i);
    }
    return n;
}

static unsigned char COPY_BUF[65536];

/* copy between drives (also within one): create the entry, then stream the data */
static int copy_on_drives(int src, int dest, const char *name)
{
    int id = vfs_add(dest, name, NODES[src].is_dir, 0);
    if (id < 0)
        return -1;
    if (NODES[src].is_dir)
    {
        ensure_loaded(src);
        for (int i = 1; i < VFS_MAX_NODES; i++)
            if (NODES[i].used && NODES[i].parent == src)
            {
                char child[VFS_NAME_MAX];
                copy_name(child, NODES[i].name);
                if (copy_on_drives(i, id, child) < 0)
                {
                    int err = LAST_ERR;
                    remove_on_drive(id); /* nothing half-copied stays behind */
                    LAST_ERR = err;
                    return -1;
                }
            }
        return id;
    }
    const VfsBackend *rb = ops_of(src), *wb = ops_of(id);
    unsigned off = 0;
    for (;;)
    {
        int got = rb->read ? rb->read(ctx_of(src), &NODES[src], off, COPY_BUF, sizeof COPY_BUF) : VFS_ERR_UNSUPPORTED;
        if (got < 0)
        {
            remove_on_drive(id);
            return fail(got);
        }
        if (got == 0)
            return id;
        int put = wb->write ? wb->write(ctx_of(id), &NODES[id], off, COPY_BUF, (unsigned)got) : VFS_ERR_UNSUPPORTED;
        if (put < 0)
        {
            remove_on_drive(id);
            return fail(put);
        }
        off += (unsigned)got;
    }
}

/* ---- RAM Disk copy: nodes only ----- */
static int copy_tree(int src, int dest, const char *name)
{
    int id = new_node(dest, name, NODES[src].is_dir, NODES[src].size, 0);
    if (id < 0 || !NODES[src].is_dir)
        return id;
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == src)
        {
            char child[VFS_NAME_MAX];
            copy_name(child, NODES[i].name);
            copy_tree(i, id, child);
        }
    return id;
}

int vfs_copy(int id, int dest)
{
    const VfsNode *n = vfs_node(id);
    const VfsNode *d = vfs_node(dest);
    if (!n || id == VFS_ROOT || vfs_is_volume_root(id) || !d || !d->is_dir || vfs_is_under(dest, id))
        return fail(VFS_ERR_ARG);
    const VfsBackend *sb = ops_of(id), *db = ops_of(dest);
    if (!sb != !db)
        return fail(VFS_ERR_UNSUPPORTED);
    if (!db)
    {
        if (vfs_subtree_count(id) + 1 > VFS_MAX_NODES - vfs_count()) /* all or nothing */
            return fail(VFS_ERR_FULL);
        char name[VFS_NAME_MAX];
        unique_name(dest, n->name, n->is_dir, name);
        return copy_tree(id, dest, name);
    }
    if (!db->create || !db->write || !sb->read) /* to a read-only drive; or from a drive that cannot be read */
        return fail(!sb->read ? VFS_ERR_UNSUPPORTED : VFS_ERR_NOWRITE);
    ensure_loaded(dest);
    VfsVolInfo info;
    vfs_volume_info(NODES[dest].vol, &info);
    if (info.total && tree_bytes(id) > info.free) /* all or nothing, as far as can be told in advance */
        return fail(VFS_ERR_FULL);
    if (vfs_subtree_count(id) + 1 > VFS_MAX_NODES - vfs_count())
        return fail(VFS_ERR_FULL);
    char name[VFS_NAME_MAX];
    unique_name(dest, n->name, n->is_dir, name);
    batch(dest, 1);
    int r = copy_on_drives(id, dest, name);
    batch(dest, 0);
    return r;
}

int vfs_move(int id, int dest)
{
    const VfsNode *n = vfs_node(id);
    const VfsNode *d = vfs_node(dest);
    if (!n || id == VFS_ROOT || vfs_is_volume_root(id) || !d || !d->is_dir || vfs_is_under(dest, id))
        return fail(VFS_ERR_ARG);
    const VfsBackend *sb = ops_of(id), *db = ops_of(dest);
    if (!sb != !db)
        return fail(VFS_ERR_UNSUPPORTED);
    if (n->parent == dest)
        return id;
    if (!sb) /* RAM Disk: just relink */
    {
        char name[VFS_NAME_MAX];
        unique_name(dest, n->name, n->is_dir, name);
        copy_name(NODES[id].name, name);
        NODES[id].parent = dest;
        NODES[id].vol = NODES[dest].vol;
        return id;
    }
    if (!sb->remove || !db->create) /* moving = copy + delete: both sides must be writable */
        return fail(VFS_ERR_NOWRITE);
    ensure_loaded(dest);
    if (NODES[id].vol == NODES[dest].vol) /* same drive: only the folder entries change */
    {
        char name[VFS_NAME_MAX];
        unique_name(dest, n->name, n->is_dir, name);
        int r = sb->move ? sb->move(ctx_of(id), &NODES[id], name, &NODES[dest]) : VFS_ERR_UNSUPPORTED;
        if (r < 0)
            return fail(r);
        copy_name(NODES[id].name, name);
        NODES[id].parent = dest;
        return id;
    }
    /* another drive: copy, then delete the original */
    int copy = vfs_copy(id, dest);
    if (copy < 0)
        return -1;
    if (vfs_remove(id) < 0)
        return -1; /* the copy is there, the original stays: nothing is lost */
    return copy;
}
