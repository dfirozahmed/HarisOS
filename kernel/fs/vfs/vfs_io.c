/* Virtual file system: read, write, truncate, save. */
#include "vfs_internal.h"

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

int vfs_write(int id, unsigned offset, const void *buf, unsigned len)
{
    const VfsNode *n = vfs_node(id);
    if (!n || n->is_dir)
        return fail(VFS_ERR_ARG);
    const VfsBackend *b = ops_of(id);
    if (!b)
        return fail(VFS_ERR_UNSUPPORTED); /* RAM-disk files have no content */
    if (!b->write)
        return fail(VFS_ERR_NOWRITE);
    int r = b->write(ctx_of(id), &NODES[id], offset, buf, len);
    return r < 0 ? fail(r) : r;
}

int vfs_truncate(int id, unsigned size)
{
    const VfsNode *n = vfs_node(id);
    if (!n || n->is_dir || size >= n->size)
        return fail(VFS_ERR_ARG);
    const VfsBackend *b = ops_of(id);
    if (!b || !b->truncate)
        return fail(VFS_ERR_UNSUPPORTED);
    int r = b->truncate(ctx_of(id), &NODES[id], size);
    return r < 0 ? fail(r) : 0;
}

/* write all of buf into file `id` from offset 0; 0 or -1 */
static int write_all(int id, const void *buf, unsigned len)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned done = 0;
    while (done < len)
    {
        int r = vfs_write(id, done, p + done, len - done);
        if (r < 0)
            return -1;
        if (r == 0)
            return fail(VFS_ERR_IO); /* no progress: do not loop forever */
        done += (unsigned)r;
    }
    return 0;
}

static const char *SAVE_STEP = ""; /* what vfs_save was doing when it failed */

/* Put buf[0,len) into the existing file `id` itself. A longer old file is cut to the new length FIRST: if the drive
 * refuses that, nothing has been changed yet; writing first would leave the head of the old file overwritten and
 * its tail still there. (A failure while writing after the cut is still possible: the caller keeps the complete
 * new text in its temporary file.) */
static int overwrite_in_place(int id, const void *buf, unsigned len)
{
    if (NODES[id].size > len)
    {
        SAVE_STEP = "shorten the old file";
        if (vfs_truncate(id, len) < 0)
            return 0;
    }
    SAVE_STEP = "write over the old file";
    return write_all(id, buf, len) == 0;
}

const char *vfs_save_step(void) { return SAVE_STEP; }

int vfs_save(int parent, const char *name, const void *buf, unsigned len)
{
    SAVE_STEP = "";
    const VfsNode *p = vfs_node(parent);
    if (!p || !p->is_dir)
        return fail(VFS_ERR_ARG);
    if (name_len(name) < 0)
        return fail(VFS_ERR_BADNAME);
    if (!ops_of(parent))
        return fail(VFS_ERR_UNSUPPORTED);
    ensure_loaded(parent);
    int old = find_child(parent, name);
    if (old >= 0 && NODES[old].is_dir)
        return fail(VFS_ERR_EXISTS);

    /* A new file is simply created and filled. An existing one cannot be shortened (the drives have no
     * truncate), so the new content goes to a temporary file first; the old file is deleted and the
     * temporary one renamed only after all of it was written - a full or failing drive never costs the old text. */
    char tmp[VFS_NAME_MAX];
    const char *target = name;
    if (old >= 0)
    {
        int n = 0;
        for (const char *s = name; *s && n < VFS_NAME_MAX - 6; s++)
            tmp[n++] = *s;
        for (const char *s = ".tmp"; *s; s++)
            tmp[n++] = *s;
        tmp[n] = 0;
        int stale = find_child(parent, tmp);
        SAVE_STEP = "remove a leftover .tmp file";
        if (stale >= 0 && vfs_remove(stale) < 0) /* left over from a save that was cut short */
            return -1;
        target = tmp;
    }
    batch(parent, 1);
    SAVE_STEP = "create the file";
    int id = vfs_add(parent, target, 0, 0);
    int err = 0;
    if (id >= 0)
        SAVE_STEP = "write the text";
    if (id < 0 || write_all(id, buf, len) < 0)
    {
        err = vfs_last_error();
        if (id >= 0)
            vfs_remove(id); /* do not leave a half-written file behind */
        id = -1;
    }
    else if (old >= 0)
    {
        SAVE_STEP = "delete the old file";
        int bad = vfs_remove(old) < 0;
        if (bad && vfs_last_error() == VFS_ERR_UNSUPPORTED && overwrite_in_place(old, buf, len))
        {
            vfs_remove(id); /* the text is in the old file now: the temporary copy is not needed */
            id = old;
            bad = 0;
        }
        else if (!bad)
        {
            SAVE_STEP = "rename the new file";
            bad = vfs_rename(id, name) < 0;
        }
        if (bad)
        {
            err = vfs_last_error();
            id = -1; /* the new text is safe in the .tmp file */
        }
    }
    batch(parent, 0);
    if (id < 0)
        return fail(err);
    SAVE_STEP = "";
    return id;
}
