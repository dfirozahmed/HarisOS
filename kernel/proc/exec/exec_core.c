/* Running a program that is a file on a drive: find, read, load, start. */
#include "exec_internal.h"

/* HarisOS exec: see include/exec.h. */

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower(*a) != lower(*b))
            return 0;
    return *a == *b;
}

int exec_find(const char *name)
{
    if (!name || !*name)
        return -1;
    for (int v = 0; v < vfs_volume_count(); v++)
    {
        int dir = vfs_volume_node(v);
        if (dir < 0)
            continue;
        int ids[256];
        int n = vfs_list(dir, ids, 256); /* reads the folder from the drive if that has not happened yet */
        if (n > 256)
            n = 256;
        for (int i = 0; i < n; i++)
        {
            const VfsNode *node = vfs_node(ids[i]);
            if (node && !node->is_dir && same_name(node->name, name))
                return ids[i];
        }
    }
    return -1;
}

/* All of file `node` into a new kernel buffer, or NULL. */
unsigned char *read_whole(int node, unsigned *size_out, int *err)
{
    const VfsNode *n = vfs_node(node);
    if (!n || n->size == 0 || n->size > EXEC_MAX_FILE)
    {
        *err = EXEC_ERR_SIZE;
        return 0;
    }
    unsigned size = n->size;
    unsigned char *buf = kmalloc(size);
    if (!buf)
    {
        *err = EXEC_ERR_MEMORY;
        return 0;
    }
    unsigned done = 0;
    while (done < size)
    {
        unsigned want = size - done > 65536 ? 65536 : size - done;
        int got = vfs_read(node, done, buf + done, want);
        if (got <= 0)
        {
            kfree(buf);
            *err = EXEC_ERR_READ;
            return 0;
        }
        done += (unsigned)got;
    }
    *size_out = size;
    return buf;
}

int exec_spawn_node(int node, const char *name, ProcessSpawn *sp, int *err)
{
    int e = 0;
    if (err)
        *err = 0;
    unsigned size = 0;
    unsigned char *buf = read_whole(node, &size, &e);
    if (!buf)
        return e;
    int elf_err = 0;
    int id = process_spawn_elf(name, buf, size, sp, &elf_err);
    kfree(buf); /* the process has its own copy of every segment */
    if (id < 0)
    {
        if (err)
            *err = elf_err;
        return EXEC_ERR_LOAD;
    }
    return id;
}

/* exec: the same file reading, then the running process becomes the program. Returns only on failure (same codes as exec_spawn_node). */
int exec_replace_node(int node, const char *name, ProcessSpawn *args, int *err)
{
    int e = 0;
    if (err)
        *err = 0;
    unsigned size = 0;
    unsigned char *buf = read_whole(node, &size, &e);
    if (!buf)
        return e;
    int elf_err = 0;
    process_exec_elf(name, buf, size, args, &elf_err, 1); /* does not return when it works (and frees buf itself then) */
    kfree(buf);
    if (err)
        *err = elf_err;
    return EXEC_ERR_LOAD;
}

int exec_run_node(int node, const char *name, int console, int *err)
{
    ProcessSpawn sp = {.console = console, .parent = -1};
    return exec_spawn_node(node, name, &sp, err);
}

int exec_run(const char *name, int *err)
{
    if (err)
        *err = 0;
    int node = exec_find(name);
    if (node < 0)
        return EXEC_ERR_NOTFOUND;
    return exec_run_node(node, name, -1, err);
}

const char *exec_error_text(int code, int err)
{
    switch (code)
    {
    case EXEC_ERR_NOTFOUND:
        return "file not found";
    case EXEC_ERR_SIZE:
        return "the file is empty or too big";
    case EXEC_ERR_MEMORY:
        return "not enough memory to read the file";
    case EXEC_ERR_READ:
        return "the drive could not read the file";
    case EXEC_ERR_LOAD:
        return err ? elf_error_text(err) : "not enough memory to start the program";
    default:
        return "unknown error";
    }
}
