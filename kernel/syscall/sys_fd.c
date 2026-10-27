/* System calls: file descriptors (files, pipes, message queues, sockets) and read / write / open / close / seek. */
#include "syscall_internal.h"

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

long long file_read(Process *p, ull idx, ull ptr, ull len)
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
long long sys_write(const SyscallFrame *f)
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

long long sys_read(const SyscallFrame *f)
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
int split_parent(const Process *p, const char *path, char *name_out, int name_cap)
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
long long sys_open(const SyscallFrame *f)
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

long long sys_close(const SyscallFrame *f)
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
long long sys_seek(const SyscallFrame *f)
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

