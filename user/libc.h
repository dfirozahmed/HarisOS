/* HarisOS user C library, part 1: strings, memory, numbers, printf. No heap yet (malloc comes with SYS_BRK), no files
 * beyond the raw calls in sys.h. A program includes this header and is linked with libc.c (CMake does it for every
 * program in USER_PROGRAMS). Built for the same target as sys.h: "long" is 32 bits, so use "long long" for big values. */
#ifndef HARIS_USER_LIBC_H
#define HARIS_USER_LIBC_H

#include <stdarg.h>
#include "sys.h"

void *memcpy(void *dst, const void *src, usize n);
void *memmove(void *dst, const void *src, usize n);
void *memset(void *dst, int c, usize n);
int memcmp(const void *a, const void *b, usize n);

usize strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, usize n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, usize n); /* pads with NULs like the standard one */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, usize n); /* appends at most n characters, then always a NUL */
char *strdup(const char *s);                        /* malloc'ed copy; free() it */
void *memchr(const void *p, int c, usize n);

int isdigit(int c);
int isalpha(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isalnum(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

/* Text to number. Leading spaces and a sign are accepted; parsing stops at the first character that is not a digit.
 * base 0 = look at the prefix (0x = 16, 0 = 8, else 10). *end gets the stop (or s itself if there were no digits). */
long long strtoll(const char *s, char **end, int base);
int atoi(const char *s);
unsigned long long strtoull(const char *s, char **end, int base);
int abs(int x);
long long llabs(long long x);
/* rand() is a fixed-seed linear congruential generator (31 bits); srand() reseeds it. Not random enough for anything but tests. */
int rand(void);
void srand(unsigned seed);
void qsort(void *base, usize count, usize size, int (*cmp)(const void *, const void *)); /* in place, no heap needed */

/* printf family. Supported: %d %i %u %x %X %o %c %s %p %%, flags '-' and '0', width, and the size letters hh h l ll z.
 * snprintf always NUL-terminates (if size > 0) and returns the length the whole text would have had. */
int vsnprintf(char *buf, usize size, const char *fmt, va_list ap);
int snprintf(char *buf, usize size, const char *fmt, ...);
int printf(const char *fmt, ...);           /* to fd 1 */
int fdprintf(int fd, const char *fmt, ...); /* to fd 1 or 2 */
int puts(const char *s);                    /* the string and a newline */
int putchar(int c);

/* The heap, on top of SYS_BRK. Blocks are 16-byte aligned. malloc(0) returns a valid minimal block; a failed request
 * returns NULL. free(NULL) does nothing; freeing a pointer that malloc did not give out (or freeing twice) is caught by
 * a marker in the block header: the program prints a message and exits with 127. */
void *sbrk(long long increment); /* the old break, or (void *)-1 */
void *malloc(usize size);
void *calloc(usize count, usize size); /* zeroed; NULL if count * size overflows */
void *realloc(void *ptr, usize size);
void free(void *ptr);

/* Processes. fork() makes a copy of this process: it returns the child's pid in the parent and 0 in the child (-1 if it failed). Both
 * continue after the call, each with its own copy of all memory (nothing is shared; a child that changes a variable does not change
 * the parent's). The child is waited for with sys_wait(pid, &status) like a spawned one. execv() makes this process another program:
 * it returns only on failure (-1). The open files and redirections stay, so the usual pattern for "cmd > file" is
 *     pid = fork(); if (pid == 0) { dup2(fd, 1); sys_close(fd); execv("CMD.ELF", argv); sys_exit(1); }  // and the parent waits
 * fork() saves the callee-saved registers (rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15) on the stack and restores them in both; the
 * floating point control state of the child is the default one. */
long long fork(void);
int execv(const char *path, const char *const *argv);
int dup2(int oldfd, int newfd);
int getpid(void);
int getppid(void);

/* Signal handlers. signal(SIGINT, f) makes the kernel call f(SIGINT) instead of ending the process when Ctrl+C (or kill) comes: while
 * the program computes (at a timer tick) or at the end of a system call. signal(sig, 0) goes back to the default (the process ends).
 * f is an ordinary function; while it runs the same signal is blocked, and when it returns the program goes on exactly where it was
 * (a sleeping system call, such as a read, returns -1 first). Only SIGINT and SIGTERM can be caught; SIGKILL cannot. Handlers are
 * inherited by fork and dropped by exec. sigblock(mask) blocks the signals whose bits (1 << signal) are set and returns the old mask:
 * a blocked signal waits (use it around code that must not be interrupted). Returns 0, or -1. */
typedef void (*sighandler_t)(int);
int signal(int sig, sighandler_t handler);
unsigned sigblock(unsigned mask);

/* ---- stdio: buffered FILE streams on top of sys_open / sys_read / sys_write / sys_seek ----
 * fopen modes: "r", "w", "a" (and a "b", ignored). The "+" update modes are NOT supported (fopen returns NULL): a stream either
 * reads or writes. A file stream is buffered (512 bytes): data reaches the drive at fflush / fclose / fseek or when the buffer is full,
 * and there is no exit hook, so fclose (or fflush) every file you wrote before sys_exit. stdout and stderr are unbuffered (written at once);
 * stdin reads fd 0. Errors: NULL / EOF / 0 as in the standard; feof and ferror tell the two end states apart. */
#define EOF (-1)
#define BUFSIZ 512
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#ifndef NULL
#define NULL ((void *)0)
#endif

typedef struct FILE
{
    int fd;
    int flags;
    int rpos, rlen; /* unread bytes of buf: rpos .. rlen (read streams) */
    int wlen;       /* bytes waiting in buf (write streams) */
    char buf[BUFSIZ];
} FILE;

extern FILE *stdin, *stdout, *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
int fflush(FILE *f); /* f == NULL: every open stream */
usize fread(void *ptr, usize size, usize count, FILE *f);
usize fwrite(const void *ptr, usize size, usize count, FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f); /* one character of pushback */
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
char *fgets(char *s, int size, FILE *f); /* up to size-1 characters, stops after a newline; NULL at the end with nothing read */
int fputs(const char *s, FILE *f);       /* no newline added */
int fseek(FILE *f, long long offset, int whence);
long long ftell(FILE *f);
void rewind(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int fileno(FILE *f);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int fprintf(FILE *f, const char *fmt, ...);
int remove(const char *path); /* a file, or an empty folder */
int rename(const char *from, const char *to);

/* ---- pthread: only the synchronization part ----
 * There are NO threads: the kernel runs one thread per process (no shared address space), so pthread_create / pthread_join do not exist.
 * What is here wraps the kernel's mutexes and condition variables (SYS_MUTEX_*, SYS_COND_*), which work between processes: after fork() both
 * copies hold the same kernel object. A pthread_mutex_t / pthread_cond_t is used in place (PTHREAD_*_INITIALIZER or _init); the kernel
 * object is made on first use under a private name that is removed at once, so it lives while a descriptor is open (destroy closes it).
 * At most 16 mutexes + condition variables per system. Return values are 0 or an errno number. */
#define EPERM 1
#define EAGAIN 11
#define EBUSY 16
#define EINVAL 22
typedef struct
{
    int fd; /* -1 until first use */
} pthread_mutex_t;
typedef struct
{
    int fd;
} pthread_cond_t;
#define PTHREAD_MUTEX_INITIALIZER {-1}
#define PTHREAD_COND_INITIALIZER {-1}
int pthread_mutex_init(pthread_mutex_t *m, const void *attr_unused);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m); /* EBUSY if held */
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_cond_init(pthread_cond_t *c, const void *attr_unused);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_destroy(pthread_cond_t *c);

#endif
