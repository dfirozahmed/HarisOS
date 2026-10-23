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
