/* Virtual file system: the node tree, the RAM Disk and the glue to drive backends. See include/vfs.h. */
#include "vfs.h"

static VfsNode NODES[VFS_MAX_NODES];

typedef struct
{
    const VfsBackend *ops;
    void *ctx;
    int root; /* the volume's folder node */
} Volume;

static Volume VOLS[VFS_MAX_VOLUMES];
static int NVOLS;
static int LAST_ERR;

static int fail(int code)
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
        return "HarisOS can only read this kind of drive (NTFS / exFAT), not change it.";
    default:
        return "That did not work.";
    }
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int same_name(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
        a++, b++;
    return *a == *b;
}

static int same_exact(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void copy_name(char *dst, const char *src)
{
    int i = 0;
    for (; src[i] && i < VFS_NAME_MAX - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static void copy_fs(unsigned char *dst, const unsigned char *src)
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

static int free_slot(void)
{
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (!NODES[i].used)
            return i;
    return -1;
}

/* a node in memory only: no drive is asked */
static int new_node(int parent, const char *name, int is_dir, unsigned size, const unsigned char *fs)
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

static void drop_subtree(int id)
{
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == id)
            drop_subtree(i);
    NODES[id].used = 0;
    NODES[id].name[0] = 0;
}

/* the backend that does the work for this node, or NULL for the RAM Disk and the root */
static const VfsBackend *ops_of(int id)
{
    int v = NODES[id].vol;
    if (v < 0 || v >= NVOLS)
        return (const VfsBackend *)0;
    return (VOLS[v].ops && VOLS[v].ops->list) ? VOLS[v].ops : (const VfsBackend *)0;
}

static void *ctx_of(int id) { return VOLS[NODES[id].vol].ctx; }

static void batch(int id, int on)
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

static int find_child(int dir, const char *name)
{
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == dir && same_name(NODES[i].name, name))
            return i;
    return -1;
}

/* length of a usable name, or -1 (empty, too long, contains '/') */
static int name_len(const char *name)
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

static void ensure_loaded(int dir)
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
static void unique_name(int dir, const char *name, int is_dir, char *out);

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
    NODES[id].loaded = !(ops && ops->list); /* a drive's root is read on first use */
    return v;
}

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

/* ---- adding ------------------------------------------------------ */
int vfs_add(int parent, const char *name, int is_dir, unsigned size)
{
    const VfsNode *p = vfs_node(parent);
    if (!p || !p->is_dir)
        return fail(VFS_ERR_ARG);
    if (name_len(name) < 0)
        return fail(VFS_ERR_BADNAME);
    const VfsBackend *b = ops_of(parent);
    if (b)
    {
        ensure_loaded(parent);
        if (find_child(parent, name) >= 0)
            return fail(VFS_ERR_EXISTS);
        if (!b->create)
            return fail(VFS_ERR_NOWRITE);
        if (free_slot() < 0)
            return fail(VFS_ERR_FULL);
        unsigned char fs[VFS_FS_BYTES];
        int r = b->create(ctx_of(parent), &NODES[parent], name, is_dir, fs);
        if (r < 0)
            return fail(r);
        int id = new_node(parent, name, is_dir, 0, fs);
        NODES[id].loaded = 1; /* a new folder is empty */
        return id;
    }
    if (find_child(parent, name) >= 0)
        return fail(VFS_ERR_EXISTS);
    int id = new_node(parent, name, is_dir, size, 0);
    return id < 0 ? fail(VFS_ERR_FULL) : id;
}

/* ---- sorted lists ------------------------------------------------ */
/* compare two texts ignoring case: <0, 0, >0 */
static int cmp_text(const char *x, const char *y)
{
    while (*x && lower((unsigned char)*x) == lower((unsigned char)*y))
        x++, y++;
    return lower((unsigned char)*x) - lower((unsigned char)*y);
}

/* the part of a file name after its last dot ("" if none; a leading dot is not an extension) */
static const char *ext_of(const char *name)
{
    const char *dot = 0;
    for (const char *p = name + 1; *name && *p; p++)
        if (*p == '.')
            dot = p + 1;
    return dot ? dot : "";
}

/* a sorts before b. Folders always come first. Within each group:
 *   NAME: by name (descending reverses folders too); SIZE / TYPE: files by size / extension
 *   (descending reverses that), ties and folders by name ascending. */
static int before_by(int a, int b, int key, int desc)
{
    if (NODES[a].is_dir != NODES[b].is_dir)
        return NODES[a].is_dir;
    int c = cmp_text(NODES[a].name, NODES[b].name);
    if (key == VFS_SORT_NAME)
        return desc ? c > 0 : c < 0;
    if (!NODES[a].is_dir)
    {
        int k = 0;
        if (key == VFS_SORT_SIZE)
            k = NODES[a].size < NODES[b].size ? -1 : NODES[a].size > NODES[b].size;
        else
            k = cmp_text(ext_of(NODES[a].name), ext_of(NODES[b].name));
        if (k)
            return desc ? k > 0 : k < 0;
    }
    return c < 0;
}

int vfs_list(int dir, int *out, int max)
{
    return vfs_list_by(dir, out, max, VFS_SORT_NAME, 0);
}

int vfs_list_by(int dir, int *out, int max, int key, int descending)
{
    int total = 0;
    if (!vfs_node(dir) || !NODES[dir].is_dir)
        return 0;
    ensure_loaded(dir);
    for (int i = 1; i < VFS_MAX_NODES; i++)
    {
        if (!NODES[i].used || NODES[i].parent != dir)
            continue;
        /* insertion sort into out[] (the table is small); entries past max only count */
        int pos = total < max ? total : max;
        while (pos > 0 && before_by(i, out[pos - 1], key, descending))
            pos--;
        if (pos < max)
        {
            int last = total < max ? total : max - 1;
            for (int k = last; k > pos; k--)
                out[k] = out[k - 1];
            out[pos] = i;
        }
        total++;
    }
    return total;
}

/* ---- paths -------------------------------------------------------- */
int vfs_path(int id, char *out, int cap)
{
    if (cap <= 0)
        return 0;
    out[0] = 0;
    if (!vfs_node(id))
        return 0;
    if (id == VFS_ROOT)
    {
        if (cap < 2)
            return 0;
        out[0] = '/';
        out[1] = 0;
        return 1;
    }
    /* collect ids from the node up to the root, then write them root first */
    int chain[64], depth = 0;
    for (int n = id; n > VFS_ROOT && depth < 64; n = NODES[n].parent)
        chain[depth++] = n;
    int len = 0;
    for (int d = depth - 1; d >= 0; d--)
    {
        if (len + 1 < cap)
            out[len++] = '/';
        for (const char *s = NODES[chain[d]].name; *s && len + 1 < cap; s++)
            out[len++] = *s;
    }
    out[len] = 0;
    return len;
}

int vfs_lookup(const char *path)
{
    if (!path || path[0] != '/')
        return -1;
    int cur = VFS_ROOT;
    while (*path)
    {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        char part[VFS_NAME_MAX];
        int n = 0;
        while (*path && *path != '/')
        {
            if (n >= VFS_NAME_MAX - 1)
                return -1;
            part[n++] = *path++;
        }
        part[n] = 0;
        if (!NODES[cur].is_dir)
            return -1;
        ensure_loaded(cur);
        cur = find_child(cur, part);
        if (cur < 0)
            return -1;
    }
    return cur;
}

int vfs_is_under(int id, int ancestor)
{
    for (int guard = 0; vfs_node(id) && guard < VFS_MAX_NODES; guard++)
    {
        if (id == ancestor)
            return 1;
        id = NODES[id].parent;
    }
    return 0;
}

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

static const VfsBackend RAM_BACKEND = {0, 0, 0, 0, 0, 0, 0, ram_info, 0, 0};

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
static void unique_name(int dir, const char *name, int is_dir, char *out)
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

int vfs_read(int id, unsigned offset, void *buf, unsigned len)
{
    const VfsNode *n = vfs_node(id);
    if (!n || n->is_dir)
        return fail(VFS_ERR_ARG);
    const VfsBackend *b = ops_of(id);
    if (!b || !b->read)
        return fail(VFS_ERR_UNSUPPORTED);
    int r = b->read(ctx_of(id), n, offset, buf, len);
    return r < 0 ? fail(r) : r;
}
