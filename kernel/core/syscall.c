/* HarisOS system calls: see include/syscall.h. */
#include "syscall.h"
#include "aspace.h"
#include "console.h"
#include "exec.h"
#include "gdt.h"
#include "gui.h"
#include "heap.h"
#include "net.h"
#include "mq.h"
#include "pipe.h"
#include "sem.h"
#include "shm.h"
#include "process.h"
#include "rtc.h"
#include "serial.h"
#include "task.h"
#include "timer.h"
#include "uaccess.h"
#include "usersig.h"
#include "vfs.h"

typedef unsigned long long ull;

static ull COUNT;

ull syscall_count(void) { return COUNT; }

/* A signal with a handler arrived while this process slept in a system call: the call gives up (-1) so that the handler can run at its
 * return. */
static int sig_interrupted(const Process *p) { return p && (p->sig_pending & ~p->sig_blocked & SIG_HANDLER_MASK) != 0; }

/* After registering with mq_wait / pipe_wait / sem_wait_for / mutex_wait_for (their result in `registered`): sleep. If the thread could not be remembered
 * (no memory for the set it would be woken from) nothing would ever wake it, so it only yields and the caller's loop looks again. */
static void sleep_registered(int registered)
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
#define CHUNK 512

static void copy_from_user(char *dst, ull src, unsigned n)
{
    const volatile char *p = (const volatile char *)src; /* volatile: no memcpy in this build, and it must really read */
    user_access_begin();
    for (unsigned i = 0; i < n; i++)
        dst[i] = p[i];
    user_access_end();
}

static void copy_to_user(ull dst, const char *src, unsigned n)
{
    volatile char *p = (volatile char *)dst;
    user_access_begin();
    for (unsigned i = 0; i < n; i++)
        p[i] = src[i];
    user_access_end();
}

/* A NUL-terminated string from ring 3 into dst[cap]: its length, or -1 if it is not readable user memory or does not fit.
 * The pages are checked one at a time as the string runs into them (its length is not known in advance). */
static int copy_string_from_user(char *dst, int cap, ull src, const AddressSpace *as)
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
static int resolve_path(const Process *p, const char *in, int bare_ok)
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

/* ---- open files ----------------------------------------------------------------------------------------------------- */
/* A message queue descriptor: one write = one message (1 .. MQ_MSG_MAX bytes), one read = one message (the buffer must hold it, else
 * -1 and the message stays). They wait like pipes do. */
static long long mq_read_user(Process *p, ull idx, ull ptr, ull len)
{
    int q = pfile_mq(&p->file[idx]);
    unsigned char buf[MQ_MSG_MAX];
    unsigned cap = len < MQ_MSG_MAX ? (unsigned)len : MQ_MSG_MAX;
    int got;
    while ((got = mq_recv(q, buf, cap)) == MQ_WOULDBLOCK)
    {
        sleep_registered(mq_wait(q, 0, task_current()));
        if (sig_interrupted(p))
            return -1;
    }
    if (got < 0)
        return -1;
    copy_to_user(ptr, (const char *)buf, (unsigned)got);
    return got;
}

static long long mq_write_user(Process *p, ull idx, ull ptr, ull len)
{
    int q = pfile_mq(&p->file[idx]);
    if (len == 0 || len > MQ_MSG_MAX)
        return -1;
    unsigned char buf[MQ_MSG_MAX];
    copy_from_user((char *)buf, ptr, (unsigned)len);
    int r;
    while ((r = mq_send(q, buf, (unsigned)len)) == MQ_WOULDBLOCK)
    {
        sleep_registered(mq_wait(q, 1, task_current()));
        if (sig_interrupted(p))
            return -1;
    }
    return r < 0 ? -1 : (long long)len;
}

/* Read from / write to open file slot `idx` of process p (descriptor PROCESS_FD_FIRST + idx, or a redirected fd 0 / 1 / 2).
 * The user buffer was checked by the caller. Positions are 32 bit (a FAT file cannot be bigger). */
/* Reading and writing a pipe end. They wait (the thread is BLOCKED, not spinning) until the pipe has data / room or the other end
 * is gone. Interrupts are off, so nothing can slip in between the check and task_block (see task.h). */
static long long pipe_read_user(Process *p, ull idx, ull ptr, ull len)
{
    int pipe = pfile_pipe(&p->file[idx]);
    if (p->file[idx].flags & PFILE_WRITE)
        return -1; /* the write end */
    char buf[CHUNK];
    unsigned want = len < CHUNK ? (unsigned)len : CHUNK;
    int got;
    while ((got = pipe_read(pipe, buf, want)) == PIPE_WOULDBLOCK)
    {
        sleep_registered(pipe_wait(pipe, 0, task_current()));
        if (sig_interrupted(p))
            return -1;
    }
    if (got > 0)
        copy_to_user(ptr, buf, (unsigned)got);
    return got < 0 ? -1 : got; /* 0 = every writer is gone: end of input */
}

static long long pipe_write_user(Process *p, ull idx, ull ptr, ull len)
{
    int pipe = pfile_pipe(&p->file[idx]);
    if (!(p->file[idx].flags & PFILE_WRITE))
        return -1; /* the read end */
    char buf[CHUNK];
    ull total = 0;
    while (total < len)
    {
        unsigned n = (len - total) < CHUNK ? (unsigned)(len - total) : CHUNK;
        copy_from_user(buf, ptr + total, n);
        unsigned sent = 0;
        while (sent < n)
        {
            int r = pipe_write(pipe, buf + sent, n - sent);
            if (r == PIPE_WOULDBLOCK)
            {
                sleep_registered(pipe_wait(pipe, 1, task_current()));
                if (sig_interrupted(p))
                    return total + sent ? (long long)(total + sent) : -1;
                continue;
            }
            if (r < 0) /* nobody will ever read it: what went in stays, the caller is told */
                return total + sent ? (long long)(total + sent) : -1;
            sent += (unsigned)r;
        }
        total += n;
    }
    return (long long)total;
}

/* ---- sockets ---------------------------------------------------------------------------------------------------------------- */
/* A socket descriptor (SYS_SOCKET): SYS_READ receives, SYS_WRITE sends (sock.h). Receiving sleeps in the same way as a pipe read: ask,
 * and when nothing has arrived give the CPU to the other threads and ask again, so that a signal can end the wait. The kernel stack is
 * small (32 KiB), so data moves in pieces of SOCK_CHUNK bytes. */
#define SOCK_CHUNK 1024

static long long sock_read_user(Process *p, ull idx, ull ptr, ull len)
{
    int s = pfile_sock(&p->file[idx]);
    unsigned char buf[SOCK_CHUNK];
    unsigned cap = len < SOCK_CHUNK ? (unsigned)len : SOCK_CHUNK;
    int got;
    while ((got = sock_recv(s, buf, cap)) == SOCK_WOULDBLOCK)
    {
        task_yield();
        if (sig_interrupted(p))
            return -1;
    }
    if (got > 0)
        copy_to_user(ptr, (const char *)buf, (unsigned)got);
    return got < 0 ? -1 : got; /* 0 = the server closed its side: end of input */
}

static long long sock_write_user(Process *p, ull idx, ull ptr, ull len)
{
    int s = pfile_sock(&p->file[idx]);
    unsigned char buf[SOCK_CHUNK];
    ull total = 0;
    while (total < len)
    {
        unsigned n = (len - total) < SOCK_CHUNK ? (unsigned)(len - total) : SOCK_CHUNK;
        copy_from_user((char *)buf, ptr + total, n);
        if (sock_send(s, buf, n) < 0) /* what went before stays sent: report it */
            return total ? (long long)total : -1;
        total += n;
    }
    return (long long)total;
}

static long long file_read(Process *p, ull idx, ull ptr, ull len)
{
    if (pfile_is_sem(&p->file[idx]) || pfile_is_usync(&p->file[idx]))
        return -1; /* a semaphore, mutex or condition variable holds no bytes */
    if (pfile_is_sock(&p->file[idx]))
        return sock_read_user(p, idx, ptr, len);
    if (pfile_is_mq(&p->file[idx]))
        return mq_read_user(p, idx, ptr, len);
    if (pfile_is_pipe(&p->file[idx]))
        return pipe_read_user(p, idx, ptr, len);
    if (p->file[idx].flags & PFILE_WRITE)
        return -1; /* opened for writing only */
    const VfsNode *vn = vfs_node(p->file[idx].node);
    if (!vn)
        return -1; /* the file was deleted while open */
    char buf[CHUNK];
    ull total = 0;
    while (total < len)
    {
        if (p->file[idx].pos >= vn->size)
            break; /* at or past the end: 0 bytes, whatever a drive's read would say about such an offset */
        ull left = vn->size - p->file[idx].pos;
        unsigned n = (len - total) < CHUNK ? (unsigned)(len - total) : CHUNK;
        if (n > left)
            n = (unsigned)left;
        int got = vfs_read(p->file[idx].node, p->file[idx].pos, buf, n);
        if (got < 0)
            return total ? (long long)total : -1;
        if (got == 0)
            break; /* end of the file */
        copy_to_user(ptr + total, buf, (unsigned)got);
        p->file[idx].pos += (unsigned)got;
        total += (ull)got;
        if ((unsigned)got < n)
            break;
    }
    return (long long)total;
}

/* Returns the bytes written (a short count if the drive refused part way), or -1 if none could be (not opened for writing,
 * the file is gone, the drive is read only / locked / full, the RAM Disk has no content, or the file would pass 4 GiB). */
static long long file_write(Process *p, ull idx, ull ptr, ull len)
{
    if (pfile_is_sem(&p->file[idx]) || pfile_is_usync(&p->file[idx]))
        return -1;
    if (pfile_is_sock(&p->file[idx]))
        return sock_write_user(p, idx, ptr, len);
    if (pfile_is_mq(&p->file[idx]))
        return mq_write_user(p, idx, ptr, len);
    if (pfile_is_pipe(&p->file[idx]))
        return pipe_write_user(p, idx, ptr, len);
    if (!(p->file[idx].flags & PFILE_WRITE))
        return -1; /* opened for reading */
    const VfsNode *vn = vfs_node(p->file[idx].node);
    if (!vn || vn->is_dir)
        return -1;
    char buf[CHUNK];
    ull total = 0;
    while (total < len)
    {
        if (p->file[idx].flags & PFILE_APPEND)
            p->file[idx].pos = vn->size;
        unsigned n = (len - total) < CHUNK ? (unsigned)(len - total) : CHUNK;
        if ((ull)p->file[idx].pos + n > 0xFFFFFFFFULL)
            break;
        copy_from_user(buf, ptr + total, n);
        int put = vfs_write(p->file[idx].node, p->file[idx].pos, buf, n);
        if (put <= 0)
            break;
        p->file[idx].pos += (unsigned)put;
        total += (ull)put;
        if ((unsigned)put < n)
            break;
        vn = vfs_node(p->file[idx].node); /* (a drive may have changed the node) */
        if (!vn)
            break;
    }
    return total || len == 0 ? (long long)total : -1;
}

/* ---- the calls ----------------------------------------------------------------------------------------------------- */
/* fd 1 and 2: the console, or a file if the shell redirected them. fd 3 and up: an open file. */
static long long sys_write(const SyscallFrame *f)
{
    ull fd = f->rdi, ptr = f->rsi, len = f->rdx;
    Process *p = process_current();
    if (fd == 0 || (fd >= PROCESS_FD_FIRST && (!p || fd >= PROCESS_FD_FIRST + (ull)p->file_cap)))
        return -1;
    if (len > SYS_WRITE_MAX)
        return -1;
    if (fd >= PROCESS_FD_FIRST && (!p || p->file[fd - PROCESS_FD_FIRST].node < 0))
        return -1; /* not an open file */
    if (len == 0)
        return 0;
    const AddressSpace *as = task_address_space();
    if (!as || !aspace_user_ok(as, ptr, len, 0))
        return -1;
    if (fd >= PROCESS_FD_FIRST || (p && p->stdio[fd] >= 0))
    {
        if (!p)
            return -1;
        ull idx = fd >= PROCESS_FD_FIRST ? fd - PROCESS_FD_FIRST : (ull)p->stdio[fd];
        if (p->file[idx].node < 0)
            return -1;
        return file_write(p, idx, ptr, len);
    }
    char buf[CHUNK];
    for (ull done = 0; done < len;)
    {
        unsigned n = (len - done) < CHUNK ? (unsigned)(len - done) : CHUNK;
        copy_from_user(buf, ptr + done, n);
        if (p && p->console >= 0)
            console_write(p->console, buf, n); /* (a window that was closed takes nothing: the text is dropped) */
        else
            for (unsigned i = 0; i < n; i++) /* no window: the serial port, where a program started at boot talks */
            {
                if (buf[i] == '\n')
                    serial_putc('\r');
                serial_putc(buf[i]);
            }
        done += n;
    }
    return (long long)len;
}

static long long sys_read(const SyscallFrame *f)
{
    ull fd = f->rdi, ptr = f->rsi, len = f->rdx;
    if (len == 0)
        return 0;
    if (len > SYS_READ_MAX)
        len = SYS_READ_MAX; /* a read may return less than asked */
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || !aspace_user_ok(as, ptr, len, 1))
        return -1;

    if (fd == 0 && p->stdio[0] >= 0) /* input redirected from a file ("cmd < file") */
        return file_read(p, (ull)p->stdio[0], ptr, len);
    if (fd == 0) /* standard input: a line typed in the console window */
    {
        char buf[CHUNK];
        if (p->console < 0)
            return 0; /* no window: no input */
        int want = len < CHUNK ? (int)len : CHUNK;
        int got;
        for (;;)
        {
            got = console_read(p->console, buf, want);
            if (got >= 0)
                break;
            /* Nothing typed yet. The thread waits without being scheduled; Enter in the window wakes it. Interrupts
             * are off, so the key cannot arrive between console_read and task_block and be missed. */
            console_wait(p->console, task_current());
            task_block();
            if (sig_interrupted(p))
                return -1;
        }
        if (got > 0)
            copy_to_user(ptr, buf, (unsigned)got);
        return got; /* 0 = the window is gone or the program is finished: end of input */
    }

    ull idx = fd - PROCESS_FD_FIRST;
    if (fd < PROCESS_FD_FIRST || idx >= (ull)p->file_cap || p->file[idx].node < 0)
        return -1;
    return file_read(p, idx, ptr, len);
}

/* The folder and the name that a path which may not exist yet means: the folder part is resolved like any path, the name is
 * the last component. Returns the folder's node, or -1 if the path has no usable last component (empty, a dot, two dots, a
 * trailing slash), the folder part is not a folder, or it is "This PC" (only drives live there). */
static int split_parent(const Process *p, const char *path, char *name_out, int name_cap)
{
    int len = 0;
    while (path[len])
        len++;
    if (len == 0 || path[len - 1] == '/')
        return -1;
    int cut = len;
    while (cut > 0 && path[cut - 1] != '/')
        cut--;
    const char *name = path + cut;
    int nlen = len - cut;
    if (nlen >= name_cap || (nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.'))
        return -1;
    char dir[256];
    if (cut == 0) /* no folder part: the working folder */
    {
        dir[0] = '.';
        dir[1] = 0;
    }
    else
    {
        if (cut >= (int)sizeof(dir))
            return -1;
        for (int i = 0; i < cut; i++)
            dir[i] = path[i];
        dir[cut] = 0; /* ends with the slash that separated it ("/" alone stays "/") */
    }
    int parent = resolve_path(p, dir, 0);
    const VfsNode *d = parent >= 0 ? vfs_node(parent) : 0;
    if (!d || !d->is_dir || parent == VFS_ROOT)
        return -1;
    for (int i = 0; i <= nlen; i++)
        name_out[i] = name[i];
    return parent;
}

/* Paths: see syscall.h. flags: 0 = read only; SYS_O_WRITE = write, SYS_O_CREATE = make the file if it is not there,
 * SYS_O_TRUNC = cut it to nothing, SYS_O_APPEND = writes go to the end. CREATE, TRUNC and APPEND need WRITE. */
static long long sys_open(const SyscallFrame *f)
{
    ull ptr = f->rdi, flags = f->rsi;
    if (flags & ~(ull)(SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC | SYS_O_APPEND))
        return -1;
    if ((flags & (SYS_O_CREATE | SYS_O_TRUNC | SYS_O_APPEND)) && !(flags & SYS_O_WRITE))
        return -1;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), ptr, as) < 0)
        return -1;
    int slot = process_file_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1; /* too many open files (or no memory for the list) */
    /* a plain name is searched among the programs only for reading: a file to change belongs to the working folder */
    int node = resolve_path(p, path, !(flags & SYS_O_WRITE));
    const VfsNode *n = node >= 0 ? vfs_node(node) : 0;
    if (n && n->is_dir)
        return -1;
    if (!n && (flags & SYS_O_CREATE))
    {
        char name[VFS_NAME_MAX];
        int parent = split_parent(p, path, name, (int)sizeof(name));
        if (parent < 0)
            return -1;
        node = vfs_add(parent, name, 0, 0);
        n = node >= 0 ? vfs_node(node) : 0;
    }
    if (!n)
        return -1;
    if ((flags & SYS_O_TRUNC) && n->size > 0 && vfs_truncate(node, 0) != 0)
        return -1;
    p->file[slot].node = node;
    p->file[slot].pos = 0;
    p->file[slot].flags = ((flags & SYS_O_WRITE) ? PFILE_WRITE : 0) | ((flags & SYS_O_APPEND) ? PFILE_APPEND : 0);
    return PROCESS_FD_FIRST + slot;
}

static long long sys_close(const SyscallFrame *f)
{
    ull fd = f->rdi;
    Process *p = process_current();
    if (!p)
        return -1;
    if (fd < PROCESS_FD_FIRST) /* closing the console is allowed and does nothing */
        return 0;
    ull idx = fd - PROCESS_FD_FIRST;
    if (idx >= (ull)p->file_cap || p->file[idx].node < 0)
        return -1;
    pfile_close(&p->file[idx]);
    for (int k = 0; k < 3; k++)
        if (p->stdio[k] == (int)idx)
            p->stdio[k] = -1; /* closing the file that stood in for fd k puts the console back */
    return 0;
}

/* whence: 0 = from the start, 1 = from the current position, 2 = from the end. Returns the new position. A position past
 * the end is allowed (a read there returns 0); a negative one is not. */
static long long sys_seek(const SyscallFrame *f)
{
    ull fd = f->rdi;
    long long off = (long long)f->rsi;
    ull whence = f->rdx;
    Process *p = process_current();
    ull idx = fd - PROCESS_FD_FIRST;
    if (!p || fd < PROCESS_FD_FIRST || idx >= (ull)p->file_cap || p->file[idx].node < 0 || p->file[idx].node >= PROCESS_PIPE_NODE)
        return -1; /* (a pipe or a message queue has no position) */
    const VfsNode *n = vfs_node(p->file[idx].node);
    if (!n)
        return -1;
    long long base;
    if (whence == 0)
        base = 0;
    else if (whence == 1)
        base = (long long)p->file[idx].pos;
    else if (whence == 2)
        base = (long long)n->size;
    else
        return -1;
    /* off is any 64-bit number from ring 3: base is between 0 and 2^32, so only a huge positive offset can overflow */
    if (off > 0 && off > 0x7FFFFFFFFFFFFFFFLL - base)
        return -1;
    long long pos = base + off;
    if (pos < 0 || pos > 0xFFFFFFFFLL)
        return -1;
    p->file[idx].pos = (unsigned)pos;
    return pos;
}

/* The arguments of SYS_SPAWN / SYS_EXEC, copied into one blob "arg0\0arg1\0..." in a kernel heap block that grows as the strings come
 * (the kernel stack is too small for PROCESS_ARGS_BYTES). argv_ptr is a NULL-terminated array of string pointers, or 0 = just the path
 * as argv[0]. 0 and *blob_out / *len_out / *argc_out, or -1 (bad pointers, too many or too long, no memory). *blob_out is set even when
 * it fails (NULL or a block): the caller always kfrees it. */
typedef struct
{
    char *p;
    unsigned len, cap;
} ArgBlob;

/* Append the NUL-terminated string at user address `src` (the pages are checked one at a time as the string runs into them). 0, or -1. */
static int blob_append_string(ArgBlob *b, const AddressSpace *as, ull src)
{
    for (ull i = 0;; i++)
    {
        ull a = src + i;
        if ((i == 0 || (a & 4095) == 0) && !aspace_user_ok(as, a, 1, 0))
            return -1;
        if (b->len + 1 > PROCESS_ARGS_BYTES)
            return -1; /* the arguments together do not fit */
        if (b->len + 1 > b->cap)
        {
            unsigned cap = b->cap ? b->cap * 2 : 256;
            char *n = krealloc(b->p, cap);
            if (!n)
                return -1;
            b->p = n;
            b->cap = cap;
        }
        char c;
        copy_from_user(&c, a, 1);
        b->p[b->len++] = c;
        if (c == 0)
            return 0;
    }
}

static int collect_args(const AddressSpace *as, ull path_ptr, ull argv_ptr, char **blob_out, unsigned *len_out, int *argc_out)
{
    ArgBlob b = {0, 0, 0};
    int argc = 0, ok = 1;
    if (argv_ptr == 0)
    {
        ok = blob_append_string(&b, as, path_ptr) == 0;
        argc = 1;
    }
    else
    {
        for (;; argc++)
        {
            if (argc == PROCESS_ARGS_MAX)
            {
                ok = 0; /* no terminating NULL within the limit */
                break;
            }
            ull slot_va = argv_ptr + (ull)argc * 8;
            ull str;
            if (slot_va < argv_ptr || !aspace_user_ok(as, slot_va, 8, 0))
            {
                ok = 0;
                break;
            }
            copy_from_user((char *)&str, slot_va, 8);
            if (str == 0)
                break;
            if (blob_append_string(&b, as, str) != 0)
            {
                ok = 0; /* unreadable, or the arguments together do not fit */
                break;
            }
        }
    }
    *blob_out = b.p;
    *len_out = b.len;
    *argc_out = argc;
    return ok ? 0 : -1;
}

/* SYS_SPAWN: path in rdi (as for SYS_OPEN), argv in rsi (a NULL-terminated array of string pointers, or 0 = just the path
 * as argv[0]). Starts the program as a child that shares this process's console. Returns the child's pid, or -1: no such
 * program, not a runnable file, bad pointers, too many or too long arguments, too many children not waited for yet, or no
 * memory / thread slot. Reading the program file happens inside this call. */
static long long spawn_with_blob(const SyscallFrame *f, char **blob_out)
{
    ull path_ptr = f->rdi, argv_ptr = f->rsi, io_ptr = f->rdx;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), path_ptr, as) < 0)
        return -1;
    int slot = process_child_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1; /* wait for some of them first */
    int node = resolve_path(p, path, 1);
    const VfsNode *n = node >= 0 ? vfs_node(node) : 0;
    if (!n || n->is_dir)
        return -1;

    char *blob;
    unsigned len = 0;
    int argc = 0;
    int collected = collect_args(as, path_ptr, argv_ptr, &blob, &len, &argc);
    *blob_out = blob; /* (the caller frees it, whatever happens below) */
    if (collected != 0)
        return -1;

    /* fd 0, 1, 2 of the child: the files the caller named (its own open descriptors, copied), else what the caller itself has
     * (a redirected shell passes it on), else the console */
    int io[3] = {-1, -1, -1};
    if (io_ptr)
    {
        if (!aspace_user_ok(as, io_ptr, sizeof(io), 0))
            return -1;
        copy_from_user((char *)io, io_ptr, sizeof(io));
    }
    ProcessFile redir[3];
    int any_redirect = 0;
    for (int k = 0; k < 3; k++)
    {
        redir[k].node = -1;
        redir[k].pos = redir[k].flags = 0;
        int idx = p->stdio[k];
        if (io[k] != -1)
        {
            if (io[k] < PROCESS_FD_FIRST || io[k] >= PROCESS_FD_FIRST + p->file_cap)
                return -1;
            idx = io[k] - PROCESS_FD_FIRST;
            if (p->file[idx].node < 0 || pfile_is_object(&p->file[idx]) || ((p->file[idx].flags & PFILE_WRITE) != 0) != (k != 0))
                return -1; /* not open, or opened the wrong way round (input must be readable, output writable) */
        }
        if (idx >= 0)
        {
            redir[k] = p->file[idx];
            any_redirect = 1;
        }
    }

    ProcessSpawn sp = {.console = p->console, .argc = argc, .args = blob, .args_len = len, .parent = task_current(),
                       .cwd = p->cwd, .redirect = any_redirect ? redir : 0};
    int err = 0;
    int id = exec_spawn_node(node, n->name, &sp, &err);
    if (id < 0)
        return -1;
    p->child[slot] = sp.pid;
    return sp.pid;
}

/* The child's stack has its own copy of the arguments by the time the call returns: the blob is not needed any more. */
static long long sys_spawn(const SyscallFrame *f)
{
    char *blob = 0;
    long long r = spawn_with_blob(f, &blob);
    kfree(blob);
    return r;
}

/* SYS_WAIT: pid in rdi (from SYS_SPAWN), a pointer for the exit code in rsi (or 0). Waits, without using the CPU, until
 * that child has ended; then returns the pid and stores the exit code (-1 if a CPU exception ended it). Each child can be
 * waited for once. -1 if pid is not a child of this process that was not waited for, or the pointer is bad (the child is
 * then still there to wait for). */
static long long sys_wait(const SyscallFrame *f)
{
    ull want = f->rdi, status_ptr = f->rsi;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || want == 0 || want > 0x7FFFFFFF)
        return -1;
    int slot = -1;
    for (int i = 0; i < p->child_cap; i++)
        if (p->child[i] == (int)want)
            slot = i;
    if (slot < 0)
        return -1;
    if (status_ptr && !aspace_user_ok(as, status_ptr, 4, 1))
        return -1;
    int code;
    while (!process_reap((int)want, &code))
    {
        task_block(); /* the child's exit wakes this thread; interrupts are off, so that cannot slip in between */
        if (sig_interrupted(p))
            return -1; /* the child stays waitable */
    }
    p->child[slot] = 0;
    if (status_ptr)
        copy_to_user(status_ptr, (const char *)&code, 4);
    return (long long)want;
}

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
static long long sys_readdir(const SyscallFrame *f)
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

static long long sys_stat(const SyscallFrame *f)
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

static long long sys_chdir(const SyscallFrame *f)
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

static long long sys_getcwd(const SyscallFrame *f)
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
static long long sys_pipe(const SyscallFrame *f)
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
static long long sys_mkdir(const SyscallFrame *f)
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

static long long sys_unlink(const SyscallFrame *f) { return remove_node(f, 0); }
static long long sys_rmdir(const SyscallFrame *f) { return remove_node(f, 1); }

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 32) != (*b | 32))
            return 0;
    return *a == *b;
}

/* SYS_RENAME: rdi = old path, rsi = new path. Either a new name in the same folder, or the same name in another folder of
 * the same drive (a move); not both at once, and the new path must not exist. 0 or -1. */
static long long sys_rename(const SyscallFrame *f)
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
static long long sys_lock(const SyscallFrame *f)
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

/* ---- fork, exec, dup2, getpid ----------------------------------------------------------------------------------------------- */
/* SYS_FORK: a copy of this process (see process_fork). The parent gets the child's pid, the child starts after the SYSCALL
 * instruction with rax = 0 and the registers of the parent NOT preserved (the fork() of the user library saves them on the stack
 * first). -1: too many children, no memory. */
static long long sys_fork(const SyscallFrame *f)
{
    if (!process_current())
        return -1;
    return process_fork(f->rcx, f->user_rsp);
}

/* SYS_EXEC: rdi = path (as for SYS_SPAWN), rsi = argv. This process becomes that program: same pid, console, folder, open files
 * and redirections. Does not return when it works. -1 (and nothing changed): no such program, not an ELF file, bad pointers, no
 * memory. */
static long long exec_with_blob(const SyscallFrame *f, char **blob_out)
{
    ull path_ptr = f->rdi, argv_ptr = f->rsi;
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    char path[256];
    if (copy_string_from_user(path, (int)sizeof(path), path_ptr, as) < 0)
        return -1;
    int node = resolve_path(p, path, 1);
    const VfsNode *n = node >= 0 ? vfs_node(node) : 0;
    if (!n || n->is_dir)
        return -1;
    char *blob;
    unsigned len = 0;
    int argc = 0;
    int collected = collect_args(as, path_ptr, argv_ptr, &blob, &len, &argc);
    *blob_out = blob; /* (freed by sys_exec if we come back) */
    if (collected != 0)
        return -1;
    ProcessSpawn sp = {.argc = argc, .args = blob, .args_len = len, .free_args = 1}; /* a program that starts frees it itself, we never return then */
    int err = 0;
    exec_replace_node(node, n->name, &sp, &err); /* returns only if it failed */
    return -1;
}

static long long sys_exec(const SyscallFrame *f)
{
    char *blob = 0;
    long long r = exec_with_blob(f, &blob); /* (it does not come back when the new program starts: the blob was freed on the way) */
    kfree(blob);
    return r;
}

/* SYS_GETPID: rdi = 0 for this process's pid, 1 for the parent's (0 if it has none or is gone). */
static long long sys_getpid(const SyscallFrame *f)
{
    Process *p = process_current();
    if (!p || f->rdi > 1)
        return -1;
    return f->rdi == 0 ? p->pid : process_ppid();
}

/* Let go of the descriptor that stands in for fd k (0..2): its file is closed unless fd 1 and 2 still share it. */
static void release_stdio(Process *p, int k)
{
    int slot = p->stdio[k];
    p->stdio[k] = -1;
    if (slot < 0)
        return;
    for (int j = 0; j < 3; j++)
        if (p->stdio[j] == slot)
            return;
    pfile_close(&p->file[slot]);
}

/* SYS_DUP2: rdi = old descriptor, rsi = new = 0, 1 or 2. Afterwards fd `new` is the same open file as `old` (a copy in a slot of its
 * own: closing `old` later does not close `new`, and a pipe end is counted for both). old = a file descriptor from SYS_OPEN or
 * SYS_PIPE, or 0..2 (copy what that one is now: "2>&1"), or -1 = the console. Returns `new`, or -1 (no free slot, bad fd, the
 * wrong direction: fd 0 must be readable, fd 1 and 2 writable). A child keeps these across SYS_EXEC: this is how a fork-and-exec
 * shell makes redirections. */
static long long sys_dup2(const SyscallFrame *f)
{
    Process *p = process_current();
    long long old = (long long)f->rdi;
    ull nw = f->rsi;
    if (!p || nw > 2)
        return -1;
    int src = -1; /* the file slot to copy, -1 = the console */
    if (old == -1)
        src = -1;
    else if (old >= 0 && old <= 2)
        src = p->stdio[old];
    else if (old >= PROCESS_FD_FIRST && old < PROCESS_FD_FIRST + p->file_cap && p->file[old - PROCESS_FD_FIRST].node >= 0)
        src = (int)old - PROCESS_FD_FIRST;
    else
        return -1;
    if (src >= 0 && (pfile_is_object(&p->file[src]) || ((p->file[src].flags & PFILE_WRITE) != 0) != (nw != 0)))
        return -1; /* input must be readable, output writable; a message queue is neither */
    if (src >= 0 && p->stdio[nw] == src)
        return (long long)nw; /* already so */
    int slot = -1;
    if (src >= 0)
    {
        slot = process_file_slot(p); /* (may move the list: src is an index) */
        if (slot < 0)
            return -1;
    }
    release_stdio(p, (int)nw);
    if (src >= 0)
    {
        p->file[slot] = p->file[src];
        pfile_ref(&p->file[slot]);
        p->stdio[nw] = slot;
    }
    return (long long)nw;
}

/* ---- shared memory and message queues ---------------------------------------------------------------------------------------- */
static ProcessShm *shm_slot_of(Process *p, ull addr)
{
    for (int i = 0; i < p->shm_cap; i++)
        if (p->shm[i].seg >= 0 && p->shm[i].addr == addr)
            return &p->shm[i];
    return 0;
}

/* Map all of segment s, readable and writable, into p's space at the next free address and record it in `slot` (a free entry of
 * p->shm). Returns the address, or 0 with nothing mapped. (SYS_SHM_MAP, and SYS_GUI_CREATE for the pixels of a window.) */
static ull shm_attach(Process *p, AddressSpace *as, ProcessShm *slot, int s)
{
    unsigned pages = shm_pages(s);
    ull addr = p->shm_next ? p->shm_next : PROCESS_SHM_BASE;
    int ok = addr + (ull)pages * 4096 <= PROCESS_SHM_END;
    unsigned done = 0;
    for (; ok && done < pages; done++)
        if (aspace_map_shared(as, addr + (ull)done * 4096, shm_page(s, done), ASPACE_WRITE) != 0)
            ok = 0;
    if (!ok)
    {
        for (unsigned i = 0; i < done; i++)
            aspace_unmap_shared(as, addr + (ull)i * 4096);
        return 0;
    }
    shm_ref(s);
    slot->seg = s;
    slot->addr = addr;
    slot->pages = pages;
    p->shm_next = addr + (ull)(pages + 1) * 4096; /* one page of nothing after it, so a run past the end faults */
    return addr;
}

/* SYS_SHM_MAP: rdi = name, rsi = size in bytes (for a segment that is made: rounded up to pages, at most SHM_MAX_PAGES), rdx = flags
 * (SHM_CREATE, SHM_EXCL). Finds or makes the segment and maps all of it, readable and writable, into this process at an address the
 * kernel picks. Returns the address, or -1. A segment made here starts as zeros. */
static long long sys_shm_map(const SyscallFrame *f)
{
    ull name_ptr = f->rdi, size = f->rsi, flags = f->rdx;
    AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || (flags & ~(ull)(SHM_CREATE | SHM_EXCL)))
        return -1;
    char name[SHM_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), name_ptr, as) < 0 || !shm_valid_name(name))
        return -1;
    ProcessShm *slot = process_shm_slot(p); /* a free entry; the list grows when all are in use */
    if (!slot)
        return -1;
    int existed = shm_open(name, 0, 0) >= 0;
    int s = shm_open(name, size, (unsigned)flags);
    if (s < 0)
        return -1;
    ull addr = shm_attach(p, as, slot, s);
    if (!addr)
    {
        if (!existed)
            shm_unlink(name); /* made just now and not usable: do not leave it behind */
        return -1;
    }
    return (long long)addr;
}

/* SYS_SHM_UNMAP: rdi = an address SYS_SHM_MAP returned. The pages leave this process (the segment lives on for others and by name). */
static long long sys_shm_unmap(const SyscallFrame *f)
{
    AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    ProcessShm *slot = shm_slot_of(p, f->rdi);
    if (!slot)
        return -1;
    for (unsigned i = 0; i < slot->pages; i++)
        aspace_unmap_shared(as, slot->addr + (ull)i * 4096);
    shm_unref(slot->seg);
    slot->seg = -1;
    int any = 0;
    for (int i = 0; i < p->shm_cap; i++)
        any |= p->shm[i].seg >= 0;
    if (!any)
        p->shm_next = 0;
    return 0;
}

/* SYS_SHM_SIZE: rdi = an address SYS_SHM_MAP returned: the size of the mapping in bytes (a whole number of pages), or -1. */
static long long sys_shm_size(const SyscallFrame *f)
{
    Process *p = process_current();
    ProcessShm *slot = p ? shm_slot_of(p, f->rdi) : 0;
    return slot ? (long long)slot->pages * 4096 : -1;
}

/* SYS_SHM_UNLINK: rdi = name. The name goes away; the pages stay until nobody maps them. 0 or -1. */
static long long sys_shm_unlink(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    if (!as || !process_current())
        return -1;
    char name[SHM_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !shm_valid_name(name))
        return -1;
    return shm_unlink(name);
}

/* SYS_MQ_OPEN: rdi = name, rsi = flags (MQ_CREATE, MQ_EXCL). A descriptor for the queue: SYS_WRITE sends one message, SYS_READ
 * receives one (see mq.h); SYS_CLOSE lets go. -1: no such queue, bad name, no free descriptor or queue. */
static long long sys_mq_open(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || (f->rsi & ~(ull)(MQ_CREATE | MQ_EXCL)))
        return -1;
    char name[MQ_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !mq_valid_name(name))
        return -1;
    int slot = process_file_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1;
    int q = mq_open(name, (unsigned)f->rsi);
    if (q < 0)
        return -1;
    p->file[slot].node = PROCESS_MQ_NODE + q;
    p->file[slot].pos = 0;
    p->file[slot].flags = 0;
    mq_ref(q);
    return PROCESS_FD_FIRST + slot;
}

static long long sys_mq_unlink(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    if (!as || !process_current())
        return -1;
    char name[MQ_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !mq_valid_name(name))
        return -1;
    return mq_unlink(name);
}

/* ---- sockets: the calls ------------------------------------------------------------------------------------------------------ */
/* SYS_SOCKET: rdi = type (SYS_SOCK_STREAM). A descriptor for a new, unconnected TCP socket, or -1 (other type, no network adapter, no free
 * socket or descriptor). SYS_READ / SYS_WRITE / SYS_CLOSE work on it; fork copies the descriptor (both processes then share the connection). */
static long long sys_socket(const SyscallFrame *f)
{
    Process *p = process_current();
    if (!p || f->rdi != SYS_SOCK_STREAM)
        return -1;
    int slot = process_file_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1;
    int s = sock_create();
    if (s < 0)
        return -1;
    p->file[slot].node = PROCESS_SOCK_NODE + s;
    p->file[slot].pos = 0;
    p->file[slot].flags = 0;
    sock_ref(s);
    return PROCESS_FD_FIRST + slot;
}

/* The socket behind descriptor fd, or -1 if it is not one. */
static int sock_of(Process *p, ull fd)
{
    ull idx = fd - PROCESS_FD_FIRST;
    if (!p || fd < PROCESS_FD_FIRST || idx >= (ull)p->file_cap || p->file[idx].node < 0 || !pfile_is_sock(&p->file[idx]))
        return -1;
    return pfile_sock(&p->file[idx]);
}

/* SYS_CONNECT: rdi = socket descriptor, rsi = the server's address as one number (a << 24 | b << 16 | c << 8 | d for a.b.c.d), rdx = port
 * 1 .. 65535. Waits for the TCP handshake (about 5 seconds at most). 0, or -1 (not a socket, already connected, nobody answers, refused). */
static long long sys_connect(const SyscallFrame *f)
{
    int s = sock_of(process_current(), f->rdi);
    if (s < 0 || f->rsi > 0xFFFFFFFFULL || f->rdx == 0 || f->rdx > 65535)
        return -1;
    unsigned char ip[4] = {(unsigned char)(f->rsi >> 24), (unsigned char)(f->rsi >> 16), (unsigned char)(f->rsi >> 8), (unsigned char)f->rsi};
    return sock_connect(s, ip, (unsigned)f->rdx);
}

/* SYS_RESOLVE: rdi = a host name ("example.com") or a dotted number. The address in the form SYS_CONNECT takes, or -1 (no such name, no DNS
 * server, no network). Waits up to a few seconds. */
static long long sys_resolve(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    if (!as || !process_current())
        return -1;
    char name[256];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) <= 0)
        return -1;
    unsigned char ip[4];
    if (!sock_resolve(name, ip))
        return -1;
    return (long long)((ull)ip[0] << 24 | (ull)ip[1] << 16 | (ull)ip[2] << 8 | ip[3]);
}

/* SYS_WIFISCAN: rdi = line number, rsi = buffer, rdx = its size (2 .. 256). One line of the last Wi-Fi scan; its length or -1. */
static long long sys_wifiscan(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    if (!as || !process_current() || f->rdi > 0xFFFF || f->rdx < 2 || f->rdx > 256 || !aspace_user_ok(as, f->rsi, f->rdx, 1))
        return -1;
    char line[256];
    int n = net_wifi_scan_line((unsigned)f->rdi, line, (unsigned)f->rdx);
    if (n < 0 || (unsigned)n >= (unsigned)f->rdx)
        return -1;
    copy_to_user(f->rsi, line, (unsigned)n + 1); /* with the terminating NUL */
    return n;
}

/* SYS_CLOCK: no arguments. The milliseconds since the kernel started (timer_ms), 0 if the timer is not running. */
static long long sys_clock(const SyscallFrame *f)
{
    (void)f;
    return (long long)timer_ms();
}

/* SYS_TIME: no arguments. The wall clock as Unix time (seconds since 1970-01-01), taken from the real-time clock, which is read as UTC.
 * -1 if there is no clock. The calendar date becomes a day count with the usual "days from civil" arithmetic (years from March, so the
 * leap day is the last day of a year). */
static long long sys_time(const SyscallFrame *f)
{
    (void)f;
    RtcTime t;
    if (rtc_read(&t) != 0 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.year < 1970)
        return -1;
    long long y = t.year - (t.month <= 2);
    long long era = y / 400;
    long long yoe = y - era * 400;
    long long mp = (t.month + 9) % 12; /* March = 0 */
    long long doy = (153 * mp + 2) / 5 + t.day - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return days * 86400 + t.hour * 3600LL + t.minute * 60LL + t.second;
}

/* SYS_BIND: rdi = socket descriptor
, rsi = local address (0 = any; nothing else is accepted: there is one address), rdx = port 1 .. 65535.
 * Claims the port for this socket. 0, or -1 (not a new socket, port in use, bad port). */
static long long sys_bind(const SyscallFrame *f)
{
    int s = sock_of(process_current(), f->rdi);
    if (s < 0 || f->rsi != 0 || f->rdx == 0 || f->rdx > 65535)
        return -1;
    return sock_bind(s, (unsigned)f->rdx);
}

/* SYS_LISTEN: rdi = a bound socket descriptor, rsi = backlog (accepted and ignored: the kernel keeps TCP_BACKLOG connections waiting).
 * From now on the port answers connection requests. 0, or -1 (not bound, no free listener). */
static long long sys_listen(const SyscallFrame *f)
{
    int s = sock_of(process_current(), f->rdi);
    if (s < 0 || f->rsi > 0xFFFFFFFFULL)
        return -1;
    return sock_listen(s);
}

/* SYS_ACCEPT: rdi = a listening socket descriptor, rsi = 0 or a pointer to a SysSockAddr that gets the peer's address and port. Sleeps (the other
 * threads run) until a client has completed the handshake, then returns a NEW descriptor for that connection. -1: not a listening socket, bad
 * pointer, no free socket or descriptor, or a signal handler ran during the wait. */
static long long sys_accept(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    int s = sock_of(p, f->rdi);
    if (s < 0 || !as || (f->rsi && !aspace_user_ok(as, f->rsi, sizeof(SysSockAddr), 1)))
        return -1;
    int slot = process_file_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1;
    unsigned char ip[4];
    unsigned port = 0;
    int n;
    while ((n = sock_accept(s, ip, &port)) == SOCK_WOULDBLOCK)
    {
        task_yield();
        if (sig_interrupted(p))
            return -1;
    }
    if (n < 0)
        return -1;
    p->file[slot].node = PROCESS_SOCK_NODE + n;
    p->file[slot].pos = 0;
    p->file[slot].flags = 0;
    sock_ref(n);
    if (f->rsi)
    {
        SysSockAddr a;
        a.ip = (unsigned)ip[0] << 24 | (unsigned)ip[1] << 16 | (unsigned)ip[2] << 8 | ip[3];
        a.port = port;
        copy_to_user(f->rsi, (const char *)&a, sizeof a);
    }
    return PROCESS_FD_FIRST + slot;
}

/* ---- semaphores -------------------------------------------------------------------------------------------------------------- */
/* SYS_SEM_OPEN: rdi = name, rsi = flags (SYS_CREATE, SYS_EXCL), rdx = the starting count for one that is made (0 .. 2^31-1).
 * A descriptor for the semaphore, or -1. SYS_CLOSE lets go; fork copies the descriptor. */
static long long sys_sem_open(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || (f->rsi & ~(ull)(SEM_CREATE | SEM_EXCL)) || f->rdx > SEM_VALUE_MAX)
        return -1;
    char name[SEM_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !sem_valid_name(name))
        return -1;
    int slot = process_file_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1;
    int s = sem_open(name, (unsigned)f->rsi, (unsigned)f->rdx);
    if (s < 0)
        return -1;
    p->file[slot].node = PROCESS_SEM_NODE + s;
    p->file[slot].pos = 0;
    p->file[slot].flags = 0;
    sem_ref(s);
    return PROCESS_FD_FIRST + slot;
}

static int sem_of(Process *p, ull fd)
{
    ull idx = fd - PROCESS_FD_FIRST;
    if (!p || fd < PROCESS_FD_FIRST || idx >= (ull)p->file_cap || p->file[idx].node < 0 || !pfile_is_sem(&p->file[idx]))
        return -1;
    return pfile_sem(&p->file[idx]);
}

/* SYS_SEM_WAIT: rdi = descriptor, rsi = 0 to wait (sleeps while the count is 0), 1 to only try. Takes one off the count: 0.
 * With rsi = 1 and a count of 0: 1 (it would have to wait). -1: not a semaphore descriptor. */
static long long sys_sem_wait(const SyscallFrame *f)
{
    int s = sem_of(process_current(), f->rdi);
    if (s < 0 || f->rsi > 1)
        return -1;
    int r;
    while ((r = sem_trywait(s)) == SEM_WOULDBLOCK)
    {
        if (f->rsi == 1)
            return 1;
        sleep_registered(sem_wait_for(s, task_current())); /* a post wakes it; interrupts are off, so the post cannot slip in between the check and here */
        if (sig_interrupted(process_current()))
            return -1;
    }
    return r == 0 ? 0 : -1;
}

/* SYS_SEM_POST: rdi = descriptor: the count goes up by one and a sleeper is woken. 0, or -1 (not a semaphore, or the count is at its
 * maximum). */
static long long sys_sem_post(const SyscallFrame *f)
{
    int s = sem_of(process_current(), f->rdi);
    return s >= 0 && sem_post(s) == 0 ? 0 : -1;
}

/* SYS_SEM_VALUE: rdi = descriptor: the count now, or -1. */
static long long sys_sem_value(const SyscallFrame *f)
{
    int s = sem_of(process_current(), f->rdi);
    return s >= 0 ? sem_value(s) : -1;
}

static long long sys_sem_unlink(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    if (!as || !process_current())
        return -1;
    char name[SEM_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !sem_valid_name(name))
        return -1;
    return sem_unlink(name);
}

/* ---- mutexes and condition variables ------------------------------------------------------------------------------------------ */
/* SYS_MUTEX_OPEN / SYS_COND_OPEN: rdi = name, rsi = flags (SYS_CREATE, SYS_EXCL). A descriptor for the named object, or -1. SYS_CLOSE lets go;
 * fork copies the descriptor. Names are per kind: a mutex and a condition variable may have the same name. */
static long long usync_open_fd(const SyscallFrame *f, int kind)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || (f->rsi & ~(ull)(USYNC_CREATE | USYNC_EXCL)))
        return -1;
    char name[USYNC_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !usync_valid_name(name))
        return -1;
    int slot = process_file_slot(p); /* a free entry; the list grows when all are in use */
    if (slot < 0)
        return -1;
    int s = usync_open(name, kind, (unsigned)f->rsi);
    if (s < 0)
        return -1;
    p->file[slot].node = (kind == USYNC_MUTEX ? PROCESS_MUTEX_NODE : PROCESS_COND_NODE) + s;
    p->file[slot].pos = 0;
    p->file[slot].flags = 0;
    usync_ref(s);
    return PROCESS_FD_FIRST + slot;
}

static long long sys_mutex_open(const SyscallFrame *f) { return usync_open_fd(f, USYNC_MUTEX); }
static long long sys_cond_open(const SyscallFrame *f) { return usync_open_fd(f, USYNC_COND); }

/* The object number behind descriptor fd if it is a mutex (kind USYNC_MUTEX) or condition variable (USYNC_COND), else -1. */
static int usync_of(Process *p, ull fd, int kind)
{
    ull idx = fd - PROCESS_FD_FIRST;
    if (!p || fd < PROCESS_FD_FIRST || idx >= (ull)p->file_cap || p->file[idx].node < 0)
        return -1;
    if (kind == USYNC_MUTEX ? !pfile_is_mutex(&p->file[idx]) : !pfile_is_cond(&p->file[idx]))
        return -1;
    return pfile_usync(&p->file[idx]);
}

/* SYS_MUTEX_LOCK: rdi = descriptor, rsi = 0 to wait (sleeps while another process holds it), 1 to only try. 0: it is yours now. With rsi = 1
 * and the mutex held: 1. -1: not a mutex, you hold it already, or a signal handler interrupted the wait. */
static long long sys_mutex_lock(const SyscallFrame *f)
{
    Process *p = process_current();
    int s = usync_of(p, f->rdi, USYNC_MUTEX);
    if (s < 0 || f->rsi > 1)
        return -1;
    Mutex *m = usync_mutex(s);
    int me = task_current();
    if (m->owner == me)
        return -1; /* it would wait for itself for ever */
    while (mutex_trylock(m) != 0)
    {
        if (f->rsi == 1)
            return 1;
        if (mutex_would_deadlock(m, me))
            return -1; /* the holder (directly or through other mutexes) waits for something this process holds */
        sleep_registered(mutex_wait_for(m, me)); /* an unlock wakes it; interrupts are off, so the unlock cannot slip in between the check and here */
        if (sig_interrupted(p))
        {
            mutex_cancel_wait(m, me);
            return -1;
        }
    }
    mutex_cancel_wait(m, me);
    return 0;
}

/* SYS_MUTEX_UNLOCK: rdi = descriptor. 0, or -1 (not a mutex, or this process does not hold it). */
static long long sys_mutex_unlock(const SyscallFrame *f)
{
    int s = usync_of(process_current(), f->rdi, USYNC_MUTEX);
    return s >= 0 && mutex_unlock(usync_mutex(s)) == 0 ? 0 : -1;
}

/* SYS_COND_WAIT: rdi = condition variable descriptor, rsi = mutex descriptor (held by the caller): lets the mutex go and sleeps until a
 * SYS_COND_SIGNAL, then takes the mutex again. 0. -1 if the descriptors are wrong or the mutex is not held by the caller. A signal handler
 * that interrupts the sleep gives -1 as well, but the mutex is held again then, as after 0: check the condition in a loop either way. */
static long long sys_cond_wait(const SyscallFrame *f)
{
    Process *p = process_current();
    int sc = usync_of(p, f->rdi, USYNC_COND), sm = usync_of(p, f->rsi, USYNC_MUTEX);
    if (sc < 0 || sm < 0)
        return -1;
    CondVar *c = usync_cond(sc);
    Mutex *m = usync_mutex(sm);
    int me = task_current();
    if (m->owner != me)
        return -1;
    cond_wait_for(c, me); /* registered before the mutex is let go: a signal sent after the unlock cannot be missed */
    mutex_unlock(m);
    int interrupted = 0;
    while (cond_waiting(c, me))
    {
        task_block();
        if (sig_interrupted(p))
        {
            cond_cancel_wait(c, me);
            interrupted = 1;
        }
    }
    mutex_relock(m); /* the mutex comes back whatever woke us (it may sleep until its holder lets go) */
    return interrupted ? -1 : 0;
}

/* SYS_COND_SIGNAL: rdi = descriptor, rsi = 0 wakes one sleeper (the one with the highest priority), 1 wakes all. 0, or -1. A signal with
 * nobody waiting is lost. */
static long long sys_cond_signal(const SyscallFrame *f)
{
    int s = usync_of(process_current(), f->rdi, USYNC_COND);
    if (s < 0 || f->rsi > 1)
        return -1;
    if (f->rsi == 0)
        cond_signal(usync_cond(s));
    else
        cond_broadcast(usync_cond(s));
    return 0;
}

/* SYS_SYNC_UNLINK: rdi = name, rsi = 0 for a mutex, 1 for a condition variable: the name goes away; the object lives while a descriptor is
 * open. 0, or -1. */
static long long sys_sync_unlink(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    if (!as || !process_current() || f->rsi > 1)
        return -1;
    char name[USYNC_NAME_MAX + 1];
    if (copy_string_from_user(name, (int)sizeof(name), f->rdi, as) < 0 || !usync_valid_name(name))
        return -1;
    return usync_unlink(name, (int)f->rsi);
}

/* ---- signal handlers --------------------------------------------------------------------------------------------------------- */
static long long sys_sigaction(const SyscallFrame *f)
{
    Process *p = process_current();
    ull sig = f->rdi, handler = f->rsi, entry = f->rdx;
    if (!p || (sig != SIGINT && sig != SIGTERM))
        return -1;
    if (handler == 0)
    {
        p->sig_handler[sig] = 0;
        p->sig_pending &= ~(1u << sig);
        return 0;
    }
    if (handler < PAGING_USER_BASE || handler >= PAGING_USER_END || entry < PAGING_USER_BASE || entry >= PAGING_USER_END)
        return -1;
    p->sig_handler[sig] = handler;
    p->sig_entry = entry;
    p->sig_ignore &= ~(1u << sig); /* installing a handler ends "ignore" */
    return 0;
}

static long long sys_sigmask(const SyscallFrame *f)
{
    Process *p = process_current();
    if (!p)
        return -1;
    unsigned old = p->sig_blocked;
    p->sig_blocked = (unsigned)f->rdi & SIG_HANDLER_MASK; /* a signal that was waiting runs as this call returns */
    return old;
}

/* ---- signals and process list ---------------------------------------------------------------------------------------- */
static long long sys_kill(const SyscallFrame *f)
{
    if (!process_current())
        return -1;
    ull pid = f->rdi, sig = f->rsi;
    if (pid == 0 || pid > 0x7FFFFFFF || (sig != 0 && sig != SIGINT && sig != SIGKILL && sig != SIGTERM))
        return -1;
    return process_signal((int)pid, (int)sig) == 0 ? 0 : -1; /* (a process that signals itself does not come back) */
}

/* SYS_SETPRIO: rdi = pid (0 = the caller), rsi = new priority (0 .. 4, higher runs first) or -1 to only read it. Only the caller itself and
 * its children (not yet waited for) can be changed. Returns the priority the process had, or -1. */
static long long sys_setprio(const SyscallFrame *f)
{
    Process *p = process_current();
    if (!p || f->rdi > 0x7FFFFFFF || (f->rsi != ~0ULL && f->rsi > (ull)TASK_PRIO_MAX))
        return -1;
    int pid = f->rdi == 0 ? p->pid : (int)f->rdi;
    int allowed = pid == p->pid;
    for (int i = 0; i < p->child_cap && !allowed; i++)
        allowed = p->child[i] == pid;
    if (!allowed)
        return -1;
    return process_priority(pid, f->rsi == ~0ULL ? -1 : (int)f->rsi);
}

static long long sys_sigignore(const SyscallFrame *f)
{
    Process *p = process_current();
    if (!p)
        return -1;
    p->sig_ignore = (unsigned)f->rdi & ((1u << SIGINT) | (1u << SIGTERM)); /* SIGKILL cannot be ignored; no other exists */
    return 0;
}

static long long sys_psinfo(const SyscallFrame *f)
{
    ull index = f->rdi, out_ptr = f->rsi;
    const AddressSpace *as = task_address_space();
    if (!as || !process_current() || !aspace_user_ok(as, out_ptr, sizeof(SysPsInfo), 1))
        return -1;
    if (index >= (ull)task_slots())
        return 0;
    ProcessInfo pi;
    if (!process_info((int)index, &pi))
        return 0;
    SysPsInfo e;
    e.pid = pi.pid;
    e.ppid = pi.ppid;
    e.running = pi.running;
    e.console = pi.console;
    for (int i = 0; i < (int)sizeof(e.name); i++)
        e.name[i] = i < (int)sizeof(pi.name) ? pi.name[i] : 0;
    copy_to_user(out_ptr, (const char *)&e, sizeof(e));
    return 1;
}

/* ---- GUI windows (gui.h) ------------------------------------------------------------------------------------------------------ */
/* The surface a handle from ring 3 names, if it is this process's own; its handle, or -1. (A handle is a positive int.) */
static int gui_mine(const Process *p, ull h)
{
    if (!p || h == 0 || h > 0x7FFFFFFFull)
        return -1;
    return gui_owner((int)h) == p->pid ? (int)h : -1;
}

/* SYS_GUI_CREATE: rdi = pointer to a SysGuiInfo (title, width, height in; handle, stride, pixels out). The surface's pixels are a shared
 * memory segment, mapped here like SYS_SHM_MAP does it; the window itself appears on the desktop later, from the main loop. 0, or -1. */
static long long sys_gui_create(const SyscallFrame *f)
{
    AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p || !aspace_user_ok(as, f->rdi, sizeof(SysGuiInfo), 1))
        return -1;
    SysGuiInfo info;
    copy_from_user((char *)&info, f->rdi, sizeof(info));
    if (info.title[sizeof(info.title) - 1] != 0) /* not NUL-terminated within its 32 bytes */
        return -1;
    ProcessShm *slot = process_shm_slot(p); /* a free entry; the list grows when all are in use */
    if (!slot)
        return -1;
    int seg;
    int h = gui_create(info.title, info.width, info.height, p->pid, &seg);
    if (h < 0)
        return -1;
    ull addr = shm_attach(p, as, slot, seg);
    if (!addr)
    {
        gui_destroy(h); /* the only holder of the pixels: they are freed */
        return -1;
    }
    info.handle = h;
    info.stride = info.width;
    info.pixels = addr;
    copy_to_user(f->rdi, (const char *)&info, sizeof(info));
    return 0;
}

/* SYS_GUI_PRESENT: rdi = handle, rsi = x | y << 16, rdx = w | h << 16. */
static long long sys_gui_present(const SyscallFrame *f)
{
    int h = gui_mine(process_current(), f->rdi);
    if (h < 0)
        return -1;
    return gui_present(h, (int)(f->rsi & 0xFFFF), (int)((f->rsi >> 16) & 0xFFFF), (int)(f->rdx & 0xFFFF), (int)((f->rdx >> 16) & 0xFFFF));
}

/* SYS_GUI_EVENT: rdi = handle, rsi = pointer to a GuiEvent, rdx = SYS_GUI_NOWAIT or 0. Sleeps like a pipe read until an event is there. */
static long long sys_gui_event(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    Process *p = process_current();
    int h = gui_mine(p, f->rdi);
    if (h < 0 || !as || !aspace_user_ok(as, f->rsi, sizeof(GuiEvent), 1))
        return -1;
    GuiEvent ev;
    int r;
    while ((r = gui_next_event(h, &ev)) == 0)
    {
        if (f->rdx & SYS_GUI_NOWAIT)
            return 0;
        gui_wait(h, task_current());
        task_block();
        if (sig_interrupted(p))
            return -1;
    }
    if (r < 0)
        return -1; /* the window was destroyed while we slept */
    copy_to_user(f->rsi, (const char *)&ev, sizeof(ev));
    return 1;
}

/* SYS_GUI_CTL: rdi = handle, rsi = SYS_GUI_DESTROY, or SYS_GUI_SET_TITLE with rdx = the new title. */
static long long sys_gui_ctl(const SyscallFrame *f)
{
    const AddressSpace *as = task_address_space();
    int h = gui_mine(process_current(), f->rdi);
    if (h < 0 || !as)
        return -1;
    if (f->rsi == SYS_GUI_DESTROY)
    {
        gui_destroy(h);
        return 0;
    }
    if (f->rsi == SYS_GUI_SET_TITLE)
    {
        char title[GUI_TITLE_MAX + 1];
        if (copy_string_from_user(title, (int)sizeof(title), f->rdx, as) < 0)
            return -1;
        return gui_set_title(h, title);
    }
    return -1;
}

/* The heap. Pages are mapped one by one up to the new break; if memory runs out half way, the pages already mapped stay
 * (heap_mapped moves with them, so a retry does not map them twice) but the break does not move. */
static long long sys_brk(const SyscallFrame *f)
{
    ull want = f->rdi;
    AddressSpace *as = task_address_space();
    Process *p = process_current();
    if (!as || !p)
        return -1;
    if (want == 0)
        return (long long)p->heap_brk;
    if (want < p->heap_base || want > p->heap_limit)
        return -1;
    ull need = (want + 4095) & ~4095ULL; /* want <= heap_limit < 2^47: no overflow */
    while (p->heap_mapped < need)
    {
        if (aspace_map_new(as, p->heap_mapped, ASPACE_WRITE) != 0)
            return -1;
        p->heap_mapped += 4096;
    }
    p->heap_brk = want;
    return (long long)want;
}

static long long dispatch_call(SyscallFrame *f)
{
    COUNT++;
    switch (f->rax)
    {
    case SYS_WRITE:
        return sys_write(f);
    case SYS_EXIT:
        process_exit((int)f->rdi); /* does not return */
    case SYS_YIELD:
        task_yield();
        return 0;
    case SYS_READ:
        return sys_read(f);
    case SYS_OPEN:
        return sys_open(f);
    case SYS_CLOSE:
        return sys_close(f);
    case SYS_SEEK:
        return sys_seek(f);
    case SYS_BRK:
        return sys_brk(f);
    case SYS_SPAWN:
        return sys_spawn(f);
    case SYS_WAIT:
        return sys_wait(f);
    case SYS_READDIR:
        return sys_readdir(f);
    case SYS_STAT:
        return sys_stat(f);
    case SYS_CHDIR:
        return sys_chdir(f);
    case SYS_GETCWD:
        return sys_getcwd(f);
    case SYS_MKDIR:
        return sys_mkdir(f);
    case SYS_UNLINK:
        return sys_unlink(f);
    case SYS_RMDIR:
        return sys_rmdir(f);
    case SYS_RENAME:
        return sys_rename(f);
    case SYS_LOCK:
        return sys_lock(f);
    case SYS_PIPE:
        return sys_pipe(f);
    case SYS_KILL:
        return sys_kill(f);
    case SYS_SIGIGNORE:
        return sys_sigignore(f);
    case SYS_PSINFO:
        return sys_psinfo(f);
    case SYS_FORK:
        return sys_fork(f);
    case SYS_EXEC:
        return sys_exec(f);
    case SYS_GETPID:
        return sys_getpid(f);
    case SYS_DUP2:
        return sys_dup2(f);
    case SYS_SHM_MAP:
        return sys_shm_map(f);
    case SYS_SHM_UNMAP:
        return sys_shm_unmap(f);
    case SYS_SHM_UNLINK:
        return sys_shm_unlink(f);
    case SYS_SHM_SIZE:
        return sys_shm_size(f);
    case SYS_MQ_OPEN:
        return sys_mq_open(f);
    case SYS_MQ_UNLINK:
        return sys_mq_unlink(f);
    case SYS_SEM_OPEN:
        return sys_sem_open(f);
    case SYS_SEM_WAIT:
        return sys_sem_wait(f);
    case SYS_SEM_POST:
        return sys_sem_post(f);
    case SYS_SEM_VALUE:
        return sys_sem_value(f);
    case SYS_SEM_UNLINK:
        return sys_sem_unlink(f);
    case SYS_MUTEX_OPEN:
        return sys_mutex_open(f);
    case SYS_MUTEX_LOCK:
        return sys_mutex_lock(f);
    case SYS_MUTEX_UNLOCK:
        return sys_mutex_unlock(f);
    case SYS_COND_OPEN:
        return sys_cond_open(f);
    case SYS_COND_WAIT:
        return sys_cond_wait(f);
    case SYS_COND_SIGNAL:
        return sys_cond_signal(f);
    case SYS_SYNC_UNLINK:
        return sys_sync_unlink(f);
    case SYS_SETPRIO:
        return sys_setprio(f);
    case SYS_GUI_CREATE:
        return sys_gui_create(f);
    case SYS_GUI_PRESENT:
        return sys_gui_present(f);
    case SYS_GUI_EVENT:
        return sys_gui_event(f);
    case SYS_GUI_CTL:
        return sys_gui_ctl(f);
    case SYS_SOCKET:
        return sys_socket(f);
    case SYS_CONNECT:
        return sys_connect(f);
    case SYS_RESOLVE:
        return sys_resolve(f);
    case SYS_BIND:
        return sys_bind(f);
    case SYS_LISTEN:
        return sys_listen(f);
    case SYS_ACCEPT:
        return sys_accept(f);
    case SYS_WIFISCAN:
        return sys_wifiscan(f);
    case SYS_CLOCK:
        return sys_clock(f);
    case SYS_TIME:
        return sys_time(f);
    case SYS_SIGACTION:
        return sys_sigaction(f);
    case SYS_SIGRETURN:
        return signal_sigreturn(f);
    case SYS_SIGMASK:
        return sys_sigmask(f);
    default:
        return -1;
    }
}

/* Every system call goes through here: after the work, a signal handler that is due runs before the process sees the result (usersig.h).
 * SYS_SIGRETURN is the one that comes back from a handler: it is not interrupted by the next one at once (the timer tick will do it). */
long long syscall_dispatch(SyscallFrame *f)
{
    /* The stack pointer may be below the mapped part of the stack (a big frame was made with one subtraction and nothing touched yet): map
     * it down to there now, so that the pointers the call checks (a buffer in that frame) are valid. */
    process_stack_grow(f->user_rsp);
    long long r = dispatch_call(f);
    if (f->rax != SYS_SIGRETURN && signal_deliverable(process_current()))
        r = signal_syscall_return(f, r);
    return r;
}


#if defined(__x86_64__) && !defined(HARIS_HOST)

typedef unsigned long long u64;

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE 1ULL

/* RFLAGS bits cleared on entry: IF (no interrupts until we are back), TF, DF (the C code expects it clear), NT, AC. */
#define SYSCALL_FLAG_MASK 0x44700ULL

/* One CPU, interrupts off from SYSCALL to SYSRET: plain variables do instead of per-CPU data. */
u64 syscall_user_rsp;
u64 syscall_kstack;

/* A return from a signal handler that came in at a timer tick must restore rcx and r11 too, which SYSRET cannot: the exit then goes
 * through IRETQ (signal.c asks with syscall_request_iret; the exit code below looks at the flag). */
u64 syscall_use_iret;
u64 syscall_iret_rip, syscall_iret_rflags;

void syscall_request_iret(u64 rip, u64 rflags)
{
    syscall_iret_rip = rip;
    syscall_iret_rflags = rflags;
    syscall_use_iret = 1;
}

/* The code the CPU jumps to on SYSCALL. At this point RSP is still the user's. Frame layout = SyscallFrame.
 *
 * Alignment: the stack top is 16-aligned and the frame is 10 qwords (80 bytes), so RSP is 16-aligned after the pushes;
 * 32 more bytes are the shadow space of the Microsoft x64 call convention (arguments in rcx).
 *
 * Before SYSRET the user RIP in rcx must be a canonical address: a non-canonical one makes SYSRET fault in ring 0
 * while RSP is already the user's. The user RIP is the address after the SYSCALL instruction, which is canonical unless
 * the instruction sits at the very end of the lower half; such a process is ended instead. */
__asm__(".text\n"
        ".p2align 4\n"
        ".globl syscall_entry\n"
        "syscall_entry:\n"
        "    movq %rsp, syscall_user_rsp(%rip)\n"
        "    movq syscall_kstack(%rip), %rsp\n"
        "    pushq syscall_user_rsp(%rip)\n"
        "    pushq %r11\n"
        "    pushq %rcx\n"
        "    pushq %rax\n"
        "    pushq %rdi\n"
        "    pushq %rsi\n"
        "    pushq %rdx\n"
        "    pushq %r10\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    movq %rsp, %rcx\n"
        "    subq $128, %rsp\n" /* 32 shadow space + 96 for xmm0-5: the C code may change them, the program expects them back */
        "    movups %xmm0, 32(%rsp)\n"
        "    movups %xmm1, 48(%rsp)\n"
        "    movups %xmm2, 64(%rsp)\n"
        "    movups %xmm3, 80(%rsp)\n"
        "    movups %xmm4, 96(%rsp)\n"
        "    movups %xmm5, 112(%rsp)\n"
        "    cld\n"
        "    call syscall_dispatch\n"
        "    movups 32(%rsp), %xmm0\n"
        "    movups 48(%rsp), %xmm1\n"
        "    movups 64(%rsp), %xmm2\n"
        "    movups 80(%rsp), %xmm3\n"
        "    movups 96(%rsp), %xmm4\n"
        "    movups 112(%rsp), %xmm5\n"
        "    addq $128, %rsp\n"
        "    movq %rax, 48(%rsp)\n" /* the result goes into the saved rax */
        "    cmpq $0, syscall_use_iret(%rip)\n"
        "    jne 4f\n"
        "    movq 56(%rsp), %rcx\n"
        "    movq %rcx, %rdx\n"
        "    sarq $47, %rdx\n"
        "    jz 1f\n"
        "    cmpq $-1, %rdx\n"
        "    jne 2f\n"
        "1:  popq %r9\n"
        "    popq %r8\n"
        "    popq %r10\n"
        "    popq %rdx\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rax\n"
        "    popq %rcx\n"
        "    popq %r11\n"
        "    popq %rsp\n" /* the user's stack again */
        "    sysretq\n"
        "2:  movq $-1, %rcx\n"
        "    subq $32, %rsp\n"
        "    call process_exit\n"
        "3:  hlt\n"
        "    jmp 3b\n"
        /* the return from a signal handler that interrupted the program at a timer tick: every register from the frame (rcx and r11 as the
         * program had them), then IRETQ with the address and flags signal.c chose (it checked that rip is in the user half) */
        "4:  movq $0, syscall_use_iret(%rip)\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %r10\n"
        "    popq %rdx\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rax\n"
        "    popq %rcx\n"
        "    popq %r11\n"
        "    pushq $0x1B\n" /* SS: the user's stack segment ... */
        "    pushq 8(%rsp)\n" /* ... RSP: the saved user stack pointer is where the pops stopped */
        "    pushq syscall_iret_rflags(%rip)\n"
        "    pushq $0x23\n" /* CS */
        "    pushq syscall_iret_rip(%rip)\n"
        "    iretq\n");

void syscall_entry(void);

static u64 rdmsr(unsigned msr)
{
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void wrmsr(unsigned msr, u64 v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((unsigned)v), "d"((unsigned)(v >> 32)));
}

void syscall_init(void)
{
    /* SYSCALL loads CS from STAR[47:32] (SS = +8: our kernel code and data). SYSRET to 64-bit code loads SS from
     * STAR[63:48] + 8 and CS from + 16, and it loads those numbers as they are: the real CPU does NOT turn the RPL
     * bits into 3 by itself (an emulator may, which hid this once). User code would then run with CS = 0x20 / SS = 0x18,
     * RPL 0, and the first interrupt would push those; IRETQ refuses a frame whose SS and CS have different RPL
     * (#GP). So the base carries the RPL: 0x13, giving SS = 0x1B and CS = 0x23 (SEL_SYSRET_BASE in segments.h). */
    u64 star = ((u64)SEL_SYSRET_BASE << 48) | ((u64)SEL_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    wrmsr(MSR_SFMASK, SYSCALL_FLAG_MASK);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void syscall_set_kernel_stack(u64 top)
{
    syscall_kstack = top;
    gdt_set_kernel_stack(top); /* the same stack for interrupts that arrive while ring 3 code runs */
}

#else

/* Host tests: no instruction, no registers; the stack is just remembered. */
static unsigned long long KSTACK;
unsigned long long syscall_iret_rip, syscall_iret_rflags; /* what the last syscall_request_iret asked for (the test looks) */
int syscall_iret_requests;
void syscall_request_iret(unsigned long long rip, unsigned long long rflags)
{
    syscall_iret_rip = rip;
    syscall_iret_rflags = rflags;
    syscall_iret_requests++;
}
void syscall_init(void) {}
void syscall_set_kernel_stack(unsigned long long top)
{
    KSTACK = top;
    gdt_set_kernel_stack(top);
}

#endif
