/* System call wrappers for HarisOS user programs (see include/syscall.h for what each call does). No C library: a program
 * is one C file that includes this header and defines _start. Built for the same 64-bit Windows-style target as the kernel,
 * where "long" is 32 bits, so every value that can be a pointer or a position is "long long" here.
 *
 * A program may start as   void _start(long long argc, char **argv)   (the kernel passes them in rcx / rdx and rdi / rsi; argv[argc] is
 * NULL). A program started by the File Explorer gets argc = 0. A program that declares _start(void) just ignores them. */
#ifndef HARIS_USER_SYS_H
#define HARIS_USER_SYS_H

#define SYS_WRITE 1
#define SYS_EXIT 2
#define SYS_YIELD 3
#define SYS_READ 4
#define SYS_OPEN 5
#define SYS_CLOSE 6
#define SYS_SEEK 7
#define SYS_BRK 8
#define SYS_SPAWN 9
#define SYS_WAIT 10
#define SYS_READDIR 11
#define SYS_STAT 12
#define SYS_CHDIR 13
#define SYS_GETCWD 14
#define SYS_MKDIR 15
#define SYS_UNLINK 16
#define SYS_RMDIR 17
#define SYS_RENAME 18
#define SYS_LOCK 19
#define SYS_PIPE 20
#define SYS_KILL 21
#define SYS_SIGIGNORE 22
#define SYS_PSINFO 23
#define SYS_FORK 24
#define SYS_EXEC 25
#define SYS_GETPID 26
#define SYS_DUP2 27
#define SYS_SHM_MAP 28
#define SYS_SHM_UNMAP 29
#define SYS_SHM_UNLINK 30
#define SYS_SHM_SIZE 31
#define SYS_MQ_OPEN 32
#define SYS_MQ_UNLINK 33
#define SYS_SEM_OPEN 34
#define SYS_SEM_WAIT 35
#define SYS_SEM_POST 36
#define SYS_SEM_VALUE 37
#define SYS_SEM_UNLINK 38
#define SYS_SIGACTION 39
#define SYS_SIGRETURN 40
#define SYS_SIGMASK 41
#define SYS_MUTEX_OPEN 42
#define SYS_MUTEX_LOCK 43
#define SYS_MUTEX_UNLOCK 44
#define SYS_COND_OPEN 45
#define SYS_COND_WAIT 46
#define SYS_COND_SIGNAL 47
#define SYS_SYNC_UNLINK 48
#define SYS_SETPRIO 49
#define SYS_GUI_CREATE 50
#define SYS_GUI_PRESENT 51
#define SYS_GUI_EVENT 52
#define SYS_GUI_CTL 53
#define SYS_SOCKET 54
#define SYS_CONNECT 55
#define SYS_RESOLVE 56
#define SYS_BIND 57
#define SYS_LISTEN 58
#define SYS_ACCEPT 59
#define SYS_WIFISCAN 60
#define SYS_SOCK_STREAM 1
#define SYS_GUI_DESTROY 0
#define SYS_GUI_SET_TITLE 1

#define SYS_CREATE 1
#define SYS_EXCL 2

#define SIGINT 2
#define SIGKILL 9
#define SIGTERM 15

#define SYS_O_WRITE 1
#define SYS_O_CREATE 2
#define SYS_O_TRUNC 4
#define SYS_O_APPEND 8

typedef unsigned long long usize;

static inline long long syscall3(long long number, long long a, long long b, long long c)
{
    long long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}

static inline long long sys_write(int fd, const void *buf, usize len) { return syscall3(SYS_WRITE, fd, (long long)(usize)buf, (long long)len); }
static inline long long sys_read(int fd, void *buf, usize len) { return syscall3(SYS_READ, fd, (long long)(usize)buf, (long long)len); }
static inline long long sys_open(const char *path) { return syscall3(SYS_OPEN, (long long)(usize)path, 0, 0); }
static inline long long sys_close(int fd) { return syscall3(SYS_CLOSE, fd, 0, 0); }
static inline long long sys_seek(int fd, long long offset, int whence) { return syscall3(SYS_SEEK, fd, offset, whence); }
static inline long long sys_brk(long long addr) { return syscall3(SYS_BRK, addr, 0, 0); }
/* argv: a NULL-terminated array of strings, or 0 for just the path. Returns the child's pid or -1. */
static inline long long sys_spawn(const char *path, const char *const *argv) { return syscall3(SYS_SPAWN, (long long)(usize)path, (long long)(usize)argv, 0); }
/* Waits for child `pid`; its exit code goes to *status (if not 0). Returns pid or -1. */
static inline long long sys_wait(long long pid, int *status) { return syscall3(SYS_WAIT, pid, (long long)(usize)status, 0); }
/* what sys_readdir and sys_stat fill in (SysDirEntry in include/syscall.h) */
typedef struct
{
    unsigned size;
    unsigned is_dir;
    char name[96];
} DirEntry;

/* 1 = entry `index` of folder `path` is in *e, 0 = no more entries, -1 = error */
static inline long long sys_readdir(const char *path, unsigned index, DirEntry *e) { return syscall3(SYS_READDIR, (long long)(usize)path, index, (long long)(usize)e); }
static inline long long sys_stat(const char *path, DirEntry *e) { return syscall3(SYS_STAT, (long long)(usize)path, (long long)(usize)e, 0); }
static inline long long sys_chdir(const char *path) { return syscall3(SYS_CHDIR, (long long)(usize)path, 0, 0); }
/* the working folder into buf (NUL-terminated); its length, or -1 */
static inline long long sys_getcwd(char *buf, usize size) { return syscall3(SYS_GETCWD, (long long)(usize)buf, (long long)size, 0); }
/* open with flags (SYS_O_*): sys_open(path) is the read-only form */
static inline long long sys_open_flags(const char *path, unsigned flags) { return syscall3(SYS_OPEN, (long long)(usize)path, flags, 0); }
static inline long long sys_mkdir(const char *path) { return syscall3(SYS_MKDIR, (long long)(usize)path, 0, 0); }
static inline long long sys_unlink(const char *path) { return syscall3(SYS_UNLINK, (long long)(usize)path, 0, 0); }
static inline long long sys_rmdir(const char *path) { return syscall3(SYS_RMDIR, (long long)(usize)path, 0, 0); }
static inline long long sys_rename(const char *from, const char *to) { return syscall3(SYS_RENAME, (long long)(usize)from, (long long)(usize)to, 0); }
/* drive lock: 1 = lock, 0 = unlock, 2 = ask (returns 1 if locked); see SYS_LOCK in include/syscall.h */
static inline long long sys_lock(const char *path, int mode) { return syscall3(SYS_LOCK, (long long)(usize)path, mode, 0); }
typedef struct
{
    int pid, ppid, running, console;
    char name[16];
} PsInfo;

/* end process pid with signal sig (SIGINT, SIGTERM, SIGKILL; 0 = only check that it exists): 0 or -1 */
static inline long long sys_kill(long long pid, int sig) { return syscall3(SYS_KILL, pid, sig, 0); }
/* ignore the signals whose bits (1 << signal) are set in mask: this process only */
static inline void sys_sigignore(unsigned mask) { syscall3(SYS_SIGIGNORE, mask, 0, 0); }
/* the index-th process: 1 = filled in, 0 = no more */
static inline long long sys_psinfo(unsigned index, PsInfo *p) { return syscall3(SYS_PSINFO, index, (long long)(usize)p, 0); }
/* replace this process by another program (same pid, files, redirections); returns only on failure, -1 */
static inline long long sys_exec(const char *path, const char *const *argv) { return syscall3(SYS_EXEC, (long long)(usize)path, (long long)(usize)argv, 0); }
/* the pid of this process (which = 0) or of its parent (1) */
static inline long long sys_getpid(int which) { return syscall3(SYS_GETPID, which, 0, 0); }
/* fd new (0, 1, 2) becomes the same open file as fd old (-1 = the console): returns new or -1 */
static inline long long sys_dup2(int old, int newfd) { return syscall3(SYS_DUP2, old, newfd, 0); }
/* shared memory: the address of the mapped segment, or (void *)-1; flags SYS_CREATE / SYS_EXCL */
static inline void *sys_shm_map(const char *name, usize size, unsigned flags) { return (void *)syscall3(SYS_SHM_MAP, (long long)(usize)name, (long long)size, flags); }
static inline long long sys_shm_unmap(void *addr) { return syscall3(SYS_SHM_UNMAP, (long long)(usize)addr, 0, 0); }
static inline long long sys_shm_unlink(const char *name) { return syscall3(SYS_SHM_UNLINK, (long long)(usize)name, 0, 0); }
static inline long long sys_shm_size(void *addr) { return syscall3(SYS_SHM_SIZE, (long long)(usize)addr, 0, 0); }
/* a message queue: a descriptor (sys_write sends one message, sys_read receives one), or -1 */
static inline long long sys_mq_open(const char *name, unsigned flags) { return syscall3(SYS_MQ_OPEN, (long long)(usize)name, flags, 0); }
static inline long long sys_mq_unlink(const char *name) { return syscall3(SYS_MQ_UNLINK, (long long)(usize)name, 0, 0); }
/* a semaphore: a descriptor (sys_close lets go), or -1. initial = the count of a new one. wait: takes one off (0), sleeping while the
 * count is 0; try_wait returns 1 instead of sleeping. post: adds one. */
static inline long long sys_sem_open(const char *name, unsigned flags, unsigned initial) { return syscall3(SYS_SEM_OPEN, (long long)(usize)name, flags, initial); }
static inline long long sys_sem_wait(int fd) { return syscall3(SYS_SEM_WAIT, fd, 0, 0); }
static inline long long sys_sem_try_wait(int fd) { return syscall3(SYS_SEM_WAIT, fd, 1, 0); }
static inline long long sys_sem_post(int fd) { return syscall3(SYS_SEM_POST, fd, 0, 0); }
static inline long long sys_sem_value(int fd) { return syscall3(SYS_SEM_VALUE, fd, 0, 0); }
static inline long long sys_sem_unlink(const char *name) { return syscall3(SYS_SEM_UNLINK, (long long)(usize)name, 0, 0); }
/* a named mutex and condition variable: descriptors (sys_close lets go), or -1. lock sleeps while another process holds the mutex; try_lock
 * returns 1 instead. cond_wait(cv, mutex) releases the held mutex, sleeps until a signal, takes the mutex again: call it in a loop that
 * checks the condition. cond_signal wakes one sleeper, cond_broadcast all. unlink: the name goes away (mutex = 0, condition variable = 1). */
static inline long long sys_mutex_open(const char *name, unsigned flags) { return syscall3(SYS_MUTEX_OPEN, (long long)(usize)name, flags, 0); }
static inline long long sys_mutex_lock(int fd) { return syscall3(SYS_MUTEX_LOCK, fd, 0, 0); }
static inline long long sys_mutex_try_lock(int fd) { return syscall3(SYS_MUTEX_LOCK, fd, 1, 0); }
static inline long long sys_mutex_unlock(int fd) { return syscall3(SYS_MUTEX_UNLOCK, fd, 0, 0); }
static inline long long sys_cond_open(const char *name, unsigned flags) { return syscall3(SYS_COND_OPEN, (long long)(usize)name, flags, 0); }
static inline long long sys_cond_wait(int cond_fd, int mutex_fd) { return syscall3(SYS_COND_WAIT, cond_fd, mutex_fd, 0); }
static inline long long sys_cond_signal(int fd) { return syscall3(SYS_COND_SIGNAL, fd, 0, 0); }
static inline long long sys_cond_broadcast(int fd) { return syscall3(SYS_COND_SIGNAL, fd, 1, 0); }
static inline long long sys_mutex_unlink(const char *name) { return syscall3(SYS_SYNC_UNLINK, (long long)(usize)name, 0, 0); }
static inline long long sys_cond_unlink(const char *name) { return syscall3(SYS_SYNC_UNLINK, (long long)(usize)name, 1, 0); }
/* scheduling priority 0 .. 4 (higher runs first, default 2) of a process (pid 0 = this one; this one and its children only): sets it and
 * returns the old one, or -1. sys_getprio only reads it. */
static inline long long sys_setprio(int pid, int prio) { return syscall3(SYS_SETPRIO, pid, prio, 0); }
static inline long long sys_getprio(int pid) { return syscall3(SYS_SETPRIO, pid, -1, 0); }
/* ---- sockets (TCP client; SYS_SOCKET ... in include/syscall.h) ---- */
/* an IPv4 address as the one number sys_connect takes: SOCK_IP(10, 0, 2, 2) */
#define SOCK_IP(a, b, c, d) ((unsigned)(a) << 24 | (unsigned)(b) << 16 | (unsigned)(c) << 8 | (unsigned)(d))
/* a new TCP socket: a descriptor, or -1. Then sys_connect, sys_send / sys_recv (or sys_write / sys_read), sys_close. */
static inline long long sys_socket(void) { return syscall3(SYS_SOCKET, SYS_SOCK_STREAM, 0, 0); }
/* connect to ip (see SOCK_IP) and port: 0, or -1 */
static inline long long sys_connect(int fd, unsigned ip, unsigned port) { return syscall3(SYS_CONNECT, fd, (long long)ip, port); }
/* host name or dotted number -> the address in the form sys_connect takes (0 .. 2^32-1), or -1 */
static inline long long sys_resolve(const char *name) { return syscall3(SYS_RESOLVE, (long long)(usize)name, 0, 0); }
/* send all len bytes: len, or -1 if the connection broke (then some of it may have gone). One system call takes at most 4096 bytes. */
static inline long long sys_send(int fd, const void *buf, usize len)
{
    const char *p = (const char *)buf;
    usize done = 0;
    while (done < len)
    {
        usize n = len - done < 4096 ? len - done : 4096;
        long long r = sys_write(fd, p + done, n);
        if (r <= 0)
            return -1;
        done += (usize)r;
    }
    return (long long)len;
}
/* receive up to len bytes: how many (>0), 0 = the server closed and everything was read, -1 = error. Waits for the first byte. */
static inline long long sys_recv(int fd, void *buf, usize len) { return sys_read(fd, buf, len); }
/* ---- sockets, server side ---- */
/* the peer's address and port, as sys_accept fills them in */
typedef struct
{
    unsigned ip; /* a << 24 | b << 16 | c << 8 | d, like SOCK_IP */
    unsigned port;
} SockAddr;
/* claim a local port (1 .. 65535) for a new socket: 0, or -1 (port in use) */
static inline long long sys_bind(int fd, unsigned port) { return syscall3(SYS_BIND, fd, 0, port); }
/* start answering connection requests on the bound port: 0, or -1. backlog is ignored (the kernel keeps 4 waiting) */
static inline long long sys_listen(int fd, int backlog) { return syscall3(SYS_LISTEN, fd, backlog, 0); }
/* wait for a client: a NEW descriptor for the connection (read / write / close it), or -1. addr may be NULL. */
static inline long long sys_accept(int fd, SockAddr *addr) { return syscall3(SYS_ACCEPT, fd, (long long)(usize)addr, 0); }
/* line `index` of the last Wi-Fi scan into buf[cap] (cap 2 .. 256): its length, or -1 (no scan, no such line) */
static inline long long sys_wifiscan(unsigned index, char *buf, usize cap) { return syscall3(SYS_WIFISCAN, index, (long long)(usize)buf, (long long)cap); }
/* ---- GUI windows (gui.h, SYS_GUI_* in include/syscall.h) ---- */
#define GUI_EV_KEY 1        /* key, ch, pressed, mods (KEY_* values of keyboard.h: KEY_ESC is 256) */
#define GUI_EV_MOUSE_MOVE 2 /* x, y, buttons (bit 0 = left, bit 1 = right) */
#define GUI_EV_MOUSE_DOWN 3 /* x, y, buttons = the button that went down */
#define GUI_EV_MOUSE_UP 4   /* x, y; the left button went up */
#define GUI_EV_WHEEL 5      /* x = steps, + = down */
#define GUI_EV_CLOSE 6      /* the user asked to close the window: clean up and sys_gui_destroy */

typedef struct
{
    char title[32];  /* in */
    int width, height; /* in: 64..512 by 48..512 */
    int handle;      /* out */
    int stride;      /* out: pixels per row */
    usize pixels;    /* out: address of width * height 32 bit pixels (0x00RRGGBB) */
} GuiInfo;

typedef struct
{
    int type;
    int x, y;
    int buttons;
    int key, ch, pressed, mods;
} GuiEvent;

/* a window: fills *info; 0 or -1. Draw into (unsigned *)info->pixels, then sys_gui_present. */
static inline long long sys_gui_create(GuiInfo *info) { return syscall3(SYS_GUI_CREATE, (long long)(usize)info, 0, 0); }
/* this part of the picture changed (w or h 0 = all of it) */
static inline long long sys_gui_present(int handle, int x, int y, int w, int h) { return syscall3(SYS_GUI_PRESENT, handle, (x & 0xFFFF) | ((long long)(y & 0xFFFF) << 16), (w & 0xFFFF) | ((long long)(h & 0xFFFF) << 16)); }
/* the next event: 1 = in *ev, 0 = none (only with nowait = 1), -1 = the window is gone */
static inline long long sys_gui_event(int handle, GuiEvent *ev, int nowait) { return syscall3(SYS_GUI_EVENT, handle, (long long)(usize)ev, nowait); }
static inline long long sys_gui_destroy(int handle) { return syscall3(SYS_GUI_CTL, handle, SYS_GUI_DESTROY, 0); }
static inline long long sys_gui_set_title(int handle, const char *title) { return syscall3(SYS_GUI_CTL, handle, SYS_GUI_SET_TITLE, (long long)(usize)title); }

/* signal handlers: use signal() of libc.h. sigmask blocks the signals whose bits are set (returns the old mask) */
static inline long long sys_sigaction(int sig, void *handler, void *entry) { return syscall3(SYS_SIGACTION, sig, (long long)(usize)handler, (long long)(usize)entry); }
static inline unsigned sys_sigmask(unsigned mask) { return (unsigned)syscall3(SYS_SIGMASK, mask, 0, 0); }
/* a pipe: fds[0] reads what is written to fds[1]; 0 or -1 */
static inline long long sys_pipe(int *fds) { return syscall3(SYS_PIPE, (long long)(usize)fds, 0, 0); }
/* sys_spawn with redirection: io[0..2] = open descriptors for the child's fd 0, 1, 2, or -1 to leave one alone */
static inline long long sys_spawn_io(const char *path, const char *const *argv, const int *io) { return syscall3(SYS_SPAWN, (long long)(usize)path, (long long)(usize)argv, (long long)(usize)io); }
static inline void sys_yield(void) { syscall3(SYS_YIELD, 0, 0, 0); }

__attribute__((noreturn)) static inline void sys_exit(int code)
{
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;)
        ;
}

static inline usize str_len(const char *s)
{
    usize n = 0;
    while (s[n])
        n++;
    return n;
}

static inline void print(const char *s) { sys_write(1, s, str_len(s)); }

/* decimal text of v into out (at least 21 bytes); returns its length */
static inline int format_number(char *out, long long v)
{
    char tmp[24];
    int n = 0, len = 0;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do
        tmp[n++] = (char)('0' + u % 10);
    while ((u /= 10) != 0);
    if (v < 0)
        out[len++] = '-';
    while (n)
        out[len++] = tmp[--n];
    out[len] = 0;
    return len;
}

static inline void print_number(long long v)
{
    char s[24];
    format_number(s, v);
    print(s);
}

#endif
