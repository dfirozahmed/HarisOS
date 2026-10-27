/* System calls: the file system tree (folders, stat, mkdir, unlink, rename, lock). */
#include "syscall_internal.h"

/* ---- folders -------------------------------------------------------------------------------------------------------- */
#define READDIR_MAX 512

static void fill_entry(SysDirEntry *e, const VfsNode *n)
{
    e->size = n->is_dir ? 0 : n->size;
    e->is_dir = n->is_dir ? 1 : 0;
    unsigned i = 0;
    for (; n->name[i] && i < sizeof(e->name) - 1; i++)
        e->name[i] = n->name[i];
    for (; i < sizeof(e->name); i++)
        e->name[i] = 0;
}

/* SYS_READDIR: the folder is read (listed, sorted) on every call and the index-th entry is handed out: no state is kept in
 * the process, so an entry added or removed between two calls only shifts the numbering. */
long long sys_readdir(const SyscallFrame *f)
{
    ull path_ptr = f->rdi, index = f->rsi, out_ptr = f->rdx;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || !aspace_user_ok(as, out_ptr, sizeof(SysDirEntry), 1))
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), path_ptr, as) < 0)
        return -1;
    int dir = resolve_path(p, path, 0);
    const VfsNode *d = dir >= 0 ? vfs_node(dir) : 0;
    if (!d || !d->is_dir || index >= READDIR_MAX)
        return -1;
    int ids[READDIR_MAX];
    int total = vfs_list(dir, ids, READDIR_MAX);
    if (index >= (ull)total)
        return 0;
    const VfsNode *n = vfs_node(ids[index]);
    if (!n)
        return -1;
    SysDirEntry e;
    fill_entry(&e, n);
    copy_to_user(out_ptr, (const char *)&e, sizeof(e));
    return 1;
}

long long sys_stat(const SyscallFrame *f)
{
    ull path_ptr = f->rdi, out_ptr = f->rsi;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || !aspace_user_ok(as, out_ptr, sizeof(SysDirEntry), 1))
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), path_ptr, as) < 0)
        return -1;
    int node = resolve_path(p, path, 1);
    const VfsNode *n = node >= 0 ? vfs_node(node) : 0;
    if (!n)
        return -1;
    SysDirEntry e;
    fill_entry(&e, n);
    copy_to_user(out_ptr, (const char *)&e, sizeof(e));
    return 0;
}

long long sys_chdir(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), f->rdi, as) < 0)
        return -1;
    int node = resolve_path(p, path, 0);
    const VfsNode *n = node >= 0 ? vfs_node(node) : 0;
    if (!n || !n->is_dir)
        return -1;
    p->cwd = node;
    return 0;
}

long long sys_getcwd(const SyscallFrame *f)
{
    ull ptr = f->rdi, cap = f->rsi;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || cap == 0 || cap > SYS_READ_MAX || !aspace_user_ok(as, ptr, cap, 1))
        return -1;
    if (!vfs_node(p->cwd))
        p->cwd = VFS_ROOT; /* the drive was unplugged: back to the top */
    char tmp[256];
    int n = vfs_path(p->cwd, tmp, (int)sizeof(tmp));
    if ((ull)n + 1 > cap)
        return -1;
    copy_to_user(ptr, tmp, (unsigned)n + 1);
    return n;
}

/* SYS_PIPE: rdi = pointer to two ints; they get the descriptors of the read end and the write end of a new pipe. 0, or -1 (fewer
 * than two free descriptors, no free pipe, bad pointer). */
long long sys_pipe(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || !aspace_user_ok(as, f->rdi, 8, 1))
        return -1;
    int slot[2];
    slot[0] = process_file_slot(p);
    if (slot[0] < 0)
        return -1;
    p->file[slot[0]].node = PROCESS_PIPE_NODE; /* held for a moment, so that the second search finds another entry */
    slot[1] = process_file_slot(p); /* (may move the list: entries are named by index) */
    if (slot[1] < 0)
    {
        pfile_clear(&p->file[slot[0]]);
        return -1;
    }
    int pipe = pipe_new();
    if (pipe < 0)
    {
        pfile_clear(&p->file[slot[0]]); /* (the entry held above) */
        return -1;
    }
    for (int k = 0; k < 2; k++)
    {
        p->file[slot[k]].node = PROCESS_PIPE_NODE + pipe;
        p->file[slot[k]].pos = 0;
        p->file[slot[k]].flags = k == 1 ? PFILE_WRITE : 0;
        pipe_ref(pipe, k == 1);
    }
    int fds[2] = {PROCESS_FD_FIRST + slot[0], PROCESS_FD_FIRST + slot[1]};
    copy_to_user(f->rdi, (const char *)fds, sizeof(fds));
    return 0;
}

/* ---- changing the tree ---------------------------------------------------------------------------------------------- */
/* Are any of the open files of this process (or the working folder) this node? Deleting or renaming it would leave a stale
 * descriptor, so it is refused. (Other processes' files are not checked: a read of a deleted file fails cleanly instead,
 * see file_read.) */
static int in_use_here(const Process *p, int node)
{
    if (p->cwd == node)
        return 1;
    for (int i = 0; i < p->file_cap; i++)
        if (p->file[i].node == node)
            return 1;
    return 0;
}

/* SYS_MKDIR: rdi = path of a new folder inside an existing folder of a drive. 0, or -1 (exists, no such folder, "This PC",
 * a read-only / locked / full drive, a bad name). */
long long sys_mkdir(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    char path[256], name[VFS_NAME_MAX];
    if (copy_string_from_user(path, (int)sizeof(path), f->rdi, as) < 0)
        return -1;
    if (resolve_path(p, path, 0) >= 0)
        return -1; /* it exists (a file or a folder) */
    int parent = split_parent(p, path, name, (int)sizeof(name));
    if (parent < 0)
        return -1;
    return vfs_add(parent, name, 1, 0) >= 0 ? 0 : -1;
}

/* SYS_UNLINK deletes a file, SYS_RMDIR an empty folder. Neither touches a drive's root, and neither follows a path through
 * a name that does not exist. A file or folder that this process still has open (or is in) is refused. */
static long long remove_node(const SyscallFrame *f, int want_dir)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), f->rdi, as) < 0)
        return -1;
    int node = resolve_path(p, path, 0);
    const VfsNode *n = node >= 0 ? vfs_node(node) : 0;
    if (!n || node == VFS_ROOT || vfs_is_volume_root(node) || (n->is_dir != 0) != want_dir || in_use_here(p, node))
        return -1;
    if (want_dir && vfs_subtree_count(node) != 0)
        return -1; /* not empty */
    return vfs_remove(node) >= 0 ? 0 : -1;
}

long long sys_unlink(const SyscallFrame *f) { return remove_node(f, 0); }
long long sys_rmdir(const SyscallFrame *f) { return remove_node(f, 1); }

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 32) != (*b | 32))
            return 0;
    return *a == *b;
}

/* SYS_RENAME: rdi = old path, rsi = new path. Either a new name in the same folder, or the same name in another folder of
 * the same drive (a move); not both at once, and the new path must not exist. 0 or -1. */
long long sys_rename(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    char from[256], to[256], name[VFS_NAME_MAX];
    if (copy_string_from_user(from, (int)sizeof(from), f->rdi, as) < 0 || copy_string_from_user(to, (int)sizeof(to), f->rsi, as) < 0)
        return -1;
    int node = resolve_path(p, from, 0);
    const VfsNode *n = node >= 0 ? vfs_node(node) : 0;
    if (!n || node == VFS_ROOT || vfs_is_volume_root(node) || in_use_here(p, node))
        return -1;
    int parent = split_parent(p, to, name, (int)sizeof(name));
    if (parent < 0)
        return -1;
    int existing = resolve_path(p, to, 0);
    if (existing >= 0 && existing != node)
        return -1; /* the name is taken */
    int old_parent = n->parent;
    if (parent == old_parent)
        return vfs_rename(node, name) >= 0 ? 0 : -1; /* (the same name with other capitals is allowed) */
    if (!same_name(n->name, name) || n->name[0] == 0)
        return -1; /* a move that also renames: do it in two steps */
    if (vfs_volume_of(parent) != vfs_volume_of(node))
        return -1; /* between drives: copy and delete is the program's job */
    int moved = vfs_move(node, parent);
    return moved >= 0 ? 0 : -1;
}

/* SYS_LOCK: rdi = a path on a drive, rsi = 1 lock writing, 0 unlock it, 2 ask. Drives start LOCKED (blk.h): a mistake in a file
 * system driver cannot damage a disk nobody has unlocked. Returns the lock state (1 = locked) for 2, 0 for a change that was
 * made, or -1 (not on a drive, the drive cannot be written at all, cannot be locked / unlocked). */
long long sys_lock(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || f->rsi > 2)
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), f->rdi, as) < 0)
        return -1;
    int node = resolve_path(p, path, 0);
    int vol = node >= 0 ? vfs_volume_of(node) : -1;
    VfsVolInfo info;
    if (vol < 0 || vfs_volume_info(vol, &info) != 0)
        return -1;
    if (f->rsi == 2)
        return info.locked ? 1 : 0;
    if (!info.writable || !info.lockable)
        return -1;
    return vfs_set_locked(vol, (int)f->rsi) == 0 ? 0 : -1;
}

