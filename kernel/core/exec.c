/* HarisOS exec: see include/exec.h. */
#include "exec.h"
#include "elf.h"
#include "heap.h"
#include "pmm.h"
#include "process.h"
#include "task.h"
#include "vfs.h"

typedef unsigned long long u64;

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
static unsigned char *read_whole(int node, unsigned *size_out, int *err)
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

int exec_run(const char *name, int *err)
{
    int e = 0;
    if (err)
        *err = 0;
    int node = exec_find(name);
    if (node < 0)
        return EXEC_ERR_NOTFOUND;
    unsigned size = 0;
    unsigned char *buf = read_whole(node, &size, &e);
    if (!buf)
        return e;
    int elf_err = 0;
    int id = process_start_elf(name, buf, size, &elf_err);
    kfree(buf); /* the process has its own copy of every segment */
    if (id < 0)
    {
        if (err)
            *err = elf_err;
        return EXEC_ERR_LOAD;
    }
    return id;
}

/* ---- boot self test ----------------------------------------------------------------------------------------- */

static u64 get64(const unsigned char *p)
{
    u64 v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void put64(unsigned char *p, u64 v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

int exec_selftest(void)
{
    int node = exec_find("HELLO.ELF");
    if (node < 0)
        return 1;

    /* "free memory" = free pages + pages the heap holds as arenas (the heap may keep one it grew for the file buffer) */
    HeapStats h0;
    heap_stats(&h0);
    u64 free0 = pmm_free_pages() + h0.arena_bytes / 4096;
    ProcessStats s0;
    process_stats(&s0);

    /* the real program: its output goes to the serial port, and it exits with 42 only if .data, .bss, .rodata and its
     * stack all had the right contents */
    int err = 0;
    int id = exec_run("hello.elf", &err); /* (case does not matter) */
    if (id < 0)
        return -1;
    if (process_wait(id) != 0)
        return -2;
    ProcessStats s1;
    process_stats(&s1);
    if (s1.exited != s0.exited + 1 || s1.last_exit != 42)
        return -3;

    /* files the loader must refuse, made by damaging a copy of the real one */
    unsigned size = 0;
    int rerr = 0;
    unsigned char *img = read_whole(node, &size, &rerr);
    if (!img)
        return -4;
    int result = 0;
    u64 phoff = size >= 64 ? get64(img + 32) : 0;
    if (size < 64 + 56 || phoff > size - 56)
        result = -5; /* not what a linker makes */
    else
    {
        unsigned char saved[8];
        int e1 = 0, e2 = 0, e3 = 0;

        img[0] = 'X'; /* no longer an ELF file */
        int a = process_start_elf("BAD1", img, size, &e1);
        img[0] = 0x7F;

        for (int i = 0; i < 8; i++)
            saved[i] = img[phoff + 16 + i];
        put64(img + phoff + 16, 0x1000); /* the first segment would land in kernel memory */
        int b = process_start_elf("BAD2", img, size, &e2);
        for (int i = 0; i < 8; i++)
            img[phoff + 16 + i] = saved[i];

        int c = process_start_elf("BAD3", img, size / 2, &e3); /* cut in half */

        if (a != -1 || e1 != ELF_ERR_FORMAT)
            result = -6;
        else if (b != -1 || e2 != ELF_ERR_SEGMENT)
            result = -7;
        else if (c != -1 || e3 == ELF_OK)
            result = -8;
    }
    kfree(img);
    if (result)
        return result;

    HeapStats h1;
    heap_stats(&h1);
    if (pmm_free_pages() + h1.arena_bytes / 4096 != free0)
        return -9; /* a page, a stack or a table was not given back */
    return 0;
}
