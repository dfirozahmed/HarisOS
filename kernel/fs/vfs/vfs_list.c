/* Virtual file system: adding nodes, sorted lists, paths. */
#include "vfs_internal.h"

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
