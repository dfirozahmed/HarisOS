/* System calls: sockets. */
#include "syscall_internal.h"

/* ---- sockets: the calls ------------------------------------------------------------------------------------------------------ */
/* SYS_SOCKET: rdi = type (SYS_SOCK_STREAM). A descriptor for a new, unconnected TCP socket, or -1 (other type, no network adapter, no free
 * socket or descriptor). SYS_READ / SYS_WRITE / SYS_CLOSE work on it; fork copies the descriptor (both processes then share the connection). */
long long sys_socket(const SyscallFrame *f)
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
long long sys_connect(const SyscallFrame *f)
{
    int s = sock_of(process_current(), f->rdi);
    if (s < 0 || f->rsi > 0xFFFFFFFFULL || f->rdx == 0 || f->rdx > 65535)
        return -1;
    unsigned char ip[4] = {(unsigned char)(f->rsi >> 24), (unsigned char)(f->rsi >> 16), (unsigned char)(f->rsi >> 8), (unsigned char)f->rsi};
    return sock_connect(s, ip, (unsigned)f->rdx);
}

/* SYS_RESOLVE: rdi = a host name ("example.com") or a dotted number. The address in the form SYS_CONNECT takes, or -1 (no such name, no DNS
 * server, no network). Waits up to a few seconds. */
long long sys_resolve(const SyscallFrame *f)
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
long long sys_wifiscan(const SyscallFrame *f)
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


/* SYS_BIND: rdi = socket descriptor
, rsi = local address (0 = any; nothing else is accepted: there is one address), rdx = port 1 .. 65535.
 * Claims the port for this socket. 0, or -1 (not a new socket, port in use, bad port). */
long long sys_bind(const SyscallFrame *f)
{
    int s = sock_of(process_current(), f->rdi);
    if (s < 0 || f->rsi != 0 || f->rdx == 0 || f->rdx > 65535)
        return -1;
    return sock_bind(s, (unsigned)f->rdx);
}

/* SYS_LISTEN: rdi = a bound socket descriptor, rsi = backlog (accepted and ignored: the kernel keeps TCP_BACKLOG connections waiting).
 * From now on the port answers connection requests. 0, or -1 (not bound, no free listener). */
long long sys_listen(const SyscallFrame *f)
{
    int s = sock_of(process_current(), f->rdi);
    if (s < 0 || f->rsi > 0xFFFFFFFFULL)
        return -1;
    return sock_listen(s);
}

/* SYS_ACCEPT: rdi = a listening socket descriptor, rsi = 0 or a pointer to a SysSockAddr that gets the peer's address and port. Sleeps (the other
 * threads run) until a client has completed the handshake, then returns a NEW descriptor for that connection. -1: not a listening socket, bad
 * pointer, no free socket or descriptor, or a signal handler ran during the wait. */
long long sys_accept(const SyscallFrame *f)
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

