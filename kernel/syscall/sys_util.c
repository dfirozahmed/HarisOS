/* System calls: moving bytes between ring 3 and the kernel, sleeping, paths. */
#include "syscall_internal.h"


/* After registering with mq_wait / pipe_wait / sem_wait_for / mutex_wait_for (their result in `registered`): sleep. If the thread could not be remembered
 * (no memory for the set it would be woken from) nothing would ever wake it, so it only yields and the caller's loop looks again. */
void sleep_registered(int registered)
{
    if (registered == 0)
        task_block();
    else
        task_yield();
}

/* ---- moving bytes between ring 3 and the kernel ------------------------------------------------------------------
 * Every pointer from ring 3 is checked with aspace_user_ok() before it is used (mapped, user accessible, writable if it
 * is to be written) and only then touched, inside the SMAP window (uaccess.h) and in small pieces through a buffer on the
 * kernel stack: code that talks to the console, the serial port or a drive never runs with the window open. Interrupts
 * are off for the whole call, so the pages cannot be unmapped between the check and the copy, and the process's own
 * address space is the one loaded, so its pointers can be used as they are. */

void copy_from_user(char *dst, ull src, unsigned n)
{
    const volatile char *p = (const volatile char *)src; /* volatile: no memcpy in this build, and it must really read */
    user_access_begin();
    for (unsigned i = 0; i < n; i++)
        dst[i] = p[i];
    user_access_end();
}

void copy_to_user(ull dst, const char *src, unsigned n)
{
    volatile char *p = (volatile char *)dst;
    user_access_begin();
    for (unsigned i = 0; i < n; i++)
        p[i] = src[i];
    user_access_end();
}

/* A NUL-terminated string from ring 3 into dst[cap]: its length, or -1 if it is not readable user memory or does not fit.
 * The pages are checked one at a time as the string runs into them (its length is not known in advance). */
int copy_string_from_user(char *dst, int cap, ull src, const AddressSpace *as)
{
    for (int i = 0; i < cap; i++)
    {
        ull a = src + (ull)i;
        if ((i == 0 || (a & 4095) == 0) && !aspace_user_ok(as, a, 1, 0))
            return -1;
        char c;
        copy_from_user(&c, a, 1);
        dst[i] = c;
        if (c == 0)
            return i;
    }
    return -1;
}

/* ---- paths ---------------------------------------------------------------------------------------------------------- */
#define PATH_MAX 320

/* The VFS node a path from ring 3 names, or -1. An absolute path is looked up as it is. A relative one is joined to the
 * working folder and cleaned ("." dropped, ".." removes the component before it, never above the root) before the lookup.
 * `bare_ok`: a plain file name that was not found that way is looked up in the top folder of every drive (the programs
 * live there), which is how "HELLO.ELF" works from any folder. */
int resolve_path(const Process *p, const char *in, int bare_ok)
{
    if (in[0] == '/')
        return vfs_lookup(in);
    char joined[PATH_MAX], norm[PATH_MAX];
    int n = 0;
    if (p->cwd != VFS_ROOT && vfs_node(p->cwd))
        n = vfs_path(p->cwd, joined, 256);
    joined[n++] = '/';
    int tail = 0;
    while (in[tail] && n + 1 < PATH_MAX)
        joined[n++] = in[tail++];
    joined[n] = 0;
    int node = -1;
    if (!in[tail]) /* (a path too long to join is not found) */
    {
        int len = 0;
        for (const char *s = joined; *s;)
        {
            while (*s == '/')
                s++;
            const char *start = s;
            while (*s && *s != '/')
                s++;
            int clen = (int)(s - start);
            if (clen == 0 || (clen == 1 && start[0] == '.'))
                continue;
            if (clen == 2 && start[0] == '.' && start[1] == '.')
            {
                while (len > 0 && norm[len - 1] != '/')
                    len--;
                if (len > 0)
                    len--; /* the '/' in front of the removed component */
                continue;
            }
            if (len + 1 + clen >= PATH_MAX)
                return -1;
            norm[len++] = '/';
            for (int i = 0; i < clen; i++)
                norm[len++] = start[i];
        }
        if (len == 0)
            norm[len++] = '/';
        norm[len] = 0;
        node = vfs_lookup(norm);
    }
    if (node < 0 && bare_ok)
    {
        int slash = 0;
        for (const char *s = in; *s; s++)
            slash |= *s == '/';
        if (!slash)
            node = exec_find(in);
    }
    return node;
}

