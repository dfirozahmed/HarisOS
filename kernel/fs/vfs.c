/* In-memory virtual file system. See include/vfs.h. */
#include "vfs.h"

static VfsNode NODES[VFS_MAX_NODES];

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int same_name(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
        a++, b++;
    return *a == *b;
}

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

int vfs_add(int parent, const char *name, int is_dir, unsigned size)
{
    const VfsNode *p = vfs_node(parent);
    int len = name_len(name);
    if (!p || !p->is_dir || len < 0)
        return -1;
    if (find_child(parent, name) >= 0)
        return -1;
    for (int i = 1; i < VFS_MAX_NODES; i++)
    {
        if (NODES[i].used)
            continue;
        for (int k = 0; k <= len; k++)
            NODES[i].name[k] = name[k];
        NODES[i].used = 1;
        NODES[i].is_dir = is_dir != 0;
        NODES[i].size = is_dir ? 0 : size;
        NODES[i].parent = parent;
        return i;
    }
    return -1;
}

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
    for (int i = 1; i < VFS_MAX_NODES; i++)
    {
        if (!NODES[i].used || NODES[i].parent != dir)
            continue;
        /* insertion sort into out[] (the pool is small); entries past max only count */
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
    int chain[VFS_MAX_NODES], depth = 0;
    for (int n = id; n > VFS_ROOT && depth < VFS_MAX_NODES; n = NODES[n].parent)
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

/* ---- demo content ---------------------------------------------- */
void vfs_init(void)
{
    vfs_reset();
    int docs = vfs_add(VFS_ROOT, "Documents", 1, 0);
    int pics = vfs_add(VFS_ROOT, "Pictures", 1, 0);
    int music = vfs_add(VFS_ROOT, "Music", 1, 0);
    int sys = vfs_add(VFS_ROOT, "System", 1, 0);
    vfs_add(VFS_ROOT, "readme.txt", 0, 1284);

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
    int len = name_len(name);
    if (!n || id == VFS_ROOT || len < 0)
        return -1;
    int dup = find_child(n->parent, name);
    if (dup >= 0 && dup != id) /* a case-only change of its own name is fine */
        return -1;
    for (int k = 0; k <= len; k++)
        NODES[id].name[k] = name[k];
    return 0;
}

int vfs_subtree_count(int id)
{
    int n = 0;
    if (!vfs_node(id))
        return 0;
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == id)
            n += 1 + vfs_subtree_count(i);
    return n;
}

int vfs_remove(int id)
{
    if (!vfs_node(id) || id == VFS_ROOT)
        return -1;
    int removed = 1;
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == id)
            removed += vfs_remove(i);
    NODES[id].used = 0;
    NODES[id].name[0] = 0;
    return removed;
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
    for (int k = 0; k <= len; k++) /* cannot happen with a 96-entry pool, but never leave out[] unset */
        out[k] = name[k];
}

static int copy_tree(int src, int dest, const char *name)
{
    int id = vfs_add(dest, name, NODES[src].is_dir, NODES[src].size);
    if (id < 0 || !NODES[src].is_dir)
        return id;
    for (int i = 1; i < VFS_MAX_NODES; i++)
        if (NODES[i].used && NODES[i].parent == src)
        {
            char child[VFS_NAME_MAX];
            for (int k = 0; k < VFS_NAME_MAX; k++)
                child[k] = NODES[i].name[k];
            copy_tree(i, id, child);
        }
    return id;
}

int vfs_copy(int id, int dest)
{
    const VfsNode *n = vfs_node(id);
    const VfsNode *d = vfs_node(dest);
    if (!n || id == VFS_ROOT || !d || !d->is_dir || vfs_is_under(dest, id))
        return -1;
    if (vfs_subtree_count(id) + 1 > VFS_MAX_NODES - vfs_count()) /* all or nothing */
        return -1;
    char name[VFS_NAME_MAX];
    unique_name(dest, n->name, n->is_dir, name);
    return copy_tree(id, dest, name);
}

int vfs_move(int id, int dest)
{
    const VfsNode *n = vfs_node(id);
    const VfsNode *d = vfs_node(dest);
    if (!n || id == VFS_ROOT || !d || !d->is_dir || vfs_is_under(dest, id))
        return -1;
    if (n->parent == dest)
        return 0;
    char name[VFS_NAME_MAX];
    unique_name(dest, n->name, n->is_dir, name);
    for (int k = 0; k < VFS_NAME_MAX; k++)
        NODES[id].name[k] = name[k];
    NODES[id].parent = dest;
    return 0;
}
