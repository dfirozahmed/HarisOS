/* HarisOS user C library, part 1 (see libc.h). */
#include "libc.h"

/* ---- memory -------------------------------------------------------------------------------------------------------- */
void *memcpy(void *dst, const void *src, usize n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, usize n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s)
        while (n--)
            *d++ = *s++;
    else
        while (n--) /* destination higher up: copy from the end so an overlap is not overwritten */
            d[n] = s[n];
    return dst;
}

void *memset(void *dst, int c, usize n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, usize n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

/* ---- strings ------------------------------------------------------------------------------------------------------- */
usize strlen(const char *s)
{
    usize n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, usize n)
{
    for (; n; n--, a++, b++)
    {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, usize n)
{
    usize i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++)
    {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return 0;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;
    for (;; s++)
    {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle)
{
    usize n = strlen(needle);
    if (n == 0)
        return (char *)hay;
    for (; *hay; hay++)
        if (*hay == *needle && strncmp(hay, needle, n) == 0)
            return (char *)hay;
    return 0;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, usize n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src)
        *d++ = *src++;
    *d = 0;
    return dst;
}

void *memchr(const void *p, int c, usize n)
{
    const unsigned char *s = p;
    for (; n; n--, s++)
        if (*s == (unsigned char)c)
            return (void *)s;
    return 0;
}

/* ---- characters and numbers ---------------------------------------------------------------------------------------- */
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isprint(int c) { return c >= 32 && c < 127; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int digit_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 99;
}

long long strtoll(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace(*p))
        p++;
    int neg = 0;
    if (*p == '-' || *p == '+')
        neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_value(p[2]) < 16)
    {
        p += 2;
        base = 16;
    }
    else if (base == 0)
        base = p[0] == '0' ? 8 : 10;
    unsigned long long v = 0;
    const char *digits = p;
    while (digit_value(*p) < base)
        v = v * (unsigned)base + (unsigned)digit_value(*p++); /* wraps on overflow: no saturation, kept simple */
    if (end)
        *end = (char *)(p == digits ? s : p);
    return neg ? (long long)(0ULL - v) : (long long)v;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace(*p))
        p++;
    if (*p == '-') /* no negative unsigned numbers: nothing is parsed */
    {
        if (end)
            *end = (char *)s;
        return 0;
    }
    return (unsigned long long)strtoll(s, end, base); /* the same wrapping arithmetic, so the full 64-bit range works */
}

int abs(int x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

static unsigned rand_state = 1;
void srand(unsigned seed) { rand_state = seed; }
int rand(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 1) & 0x7FFFFFFF); /* the low bit of an LCG is weak: drop it */
}

/* heapsort: in place, O(n log n) worst case, no recursion and no allocation (a program may have no heap yet) */
static void swap_bytes(char *a, char *b, usize n)
{
    while (n--)
    {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(char *base, usize size, usize root, usize n, int (*cmp)(const void *, const void *))
{
    for (;;)
    {
        usize child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (cmp(base + root * size, base + child * size) >= 0)
            return;
        swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

void qsort(void *base, usize count, usize size, int (*cmp)(const void *, const void *))
{
    if (count < 2 || size == 0)
        return;
    char *b = base;
    for (usize i = count / 2; i-- > 0;)
        sift_down(b, size, i, count, cmp);
    for (usize end = count - 1; end > 0; end--)
    {
        swap_bytes(b, b + end * size, size);
        sift_down(b, size, 0, end, cmp);
    }
}

int atoi(const char *s) { return (int)strtoll(s, 0, 10); }

/* ---- printf -------------------------------------------------------------------------------------------------------- */
/* Output goes to a sink: either the caller's buffer (snprintf: counts everything, stores what fits) or a file descriptor
 * (printf: collected in tmp and written when it is full, so a long text needs no big buffer). */
typedef struct
{
    char *buf;
    usize cap, total; /* buffer mode: room, and characters produced so far (also past cap) */
    int fd;           /* >= 0: descriptor mode */
    char tmp[128];
    usize used;
} Sink;

static void sink_flush(Sink *s)
{
    if (s->fd >= 0 && s->used)
    {
        sys_write(s->fd, s->tmp, s->used);
        s->used = 0;
    }
}

static void sink_put(Sink *s, char c)
{
    if (s->fd >= 0)
    {
        s->tmp[s->used++] = c;
        if (s->used == sizeof(s->tmp))
            sink_flush(s);
    }
    else if (s->total + 1 < s->cap) /* keep one byte for the NUL */
        s->buf[s->total] = c;
    s->total++;
}

/* `body` of `len` characters with `prefix` ("-", "0x" or "") in front, padded to `width` columns */
static void put_padded(Sink *s, const char *body, int len, const char *prefix, int width, int left, int zero)
{
    int plen = (int)strlen(prefix);
    int pad = width - len - plen;
    if (!left && !zero)
        for (; pad > 0; pad--)
            sink_put(s, ' ');
    for (int i = 0; i < plen; i++)
        sink_put(s, prefix[i]);
    if (!left && zero)
        for (; pad > 0; pad--)
            sink_put(s, '0');
    for (int i = 0; i < len; i++)
        sink_put(s, body[i]);
    if (left)
        for (; pad > 0; pad--)
            sink_put(s, ' ');
}

static void format(Sink *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++)
    {
        if (*fmt != '%')
        {
            sink_put(s, *fmt);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, width = 0;
        int size = 0; /* -2 hh, -1 h, 0 int, 1 long (32 bit here), 2 long long, 3 size_t */
        for (;; fmt++)
        {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                zero = 1;
            else
                break;
        }
        while (isdigit(*fmt))
            width = width * 10 + (*fmt++ - '0');
        for (;; fmt++)
        {
            if (*fmt == 'l')
                size = size == 1 ? 2 : 1;
            else if (*fmt == 'h')
                size = size == -1 ? -2 : -1;
            else if (*fmt == 'z')
                size = 3;
            else
                break;
        }
        char c = *fmt;
        if (c == 0)
            break; /* a lone % at the end */
        char tmp[24];
        int n = 0;
        const char *prefix = "";
        if (c == 'd' || c == 'i')
        {
            long long v = size >= 2 ? va_arg(ap, long long) : size == 1 ? va_arg(ap, long) : va_arg(ap, int);
            if (size == -1)
                v = (short)v;
            else if (size == -2)
                v = (signed char)v;
            unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
            if (v < 0)
                prefix = "-";
            char rev[24];
            int r = 0;
            do
                rev[r++] = (char)('0' + u % 10);
            while ((u /= 10) != 0);
            while (r)
                tmp[n++] = rev[--r];
            put_padded(s, tmp, n, prefix, width, left, zero);
        }
        else if (c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p')
        {
            unsigned long long u;
            if (c == 'p')
            {
                u = (unsigned long long)(usize)va_arg(ap, void *);
                prefix = "0x";
            }
            else if (size >= 2)
                u = va_arg(ap, unsigned long long);
            else if (size == 1)
                u = va_arg(ap, unsigned long);
            else
            {
                u = va_arg(ap, unsigned);
                if (size == -1)
                    u &= 0xFFFF;
                else if (size == -2)
                    u &= 0xFF;
            }
            unsigned base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
            const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char rev[24];
            int r = 0;
            do
                rev[r++] = dig[u % base];
            while ((u /= base) != 0);
            while (r)
                tmp[n++] = rev[--r];
            put_padded(s, tmp, n, prefix, width, left, zero);
        }
        else if (c == 'c')
        {
            tmp[0] = (char)va_arg(ap, int);
            put_padded(s, tmp, 1, "", width, left, 0);
        }
        else if (c == 's')
        {
            const char *str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            put_padded(s, str, (int)strlen(str), "", width, left, 0);
        }
        else if (c == '%')
            sink_put(s, '%');
        else
        {
            sink_put(s, '%'); /* unknown conversion: shown as written */
            sink_put(s, c);
        }
    }
}

int vsnprintf(char *buf, usize size, const char *fmt, va_list ap)
{
    Sink s = {buf, size, 0, -1, {0}, 0};
    format(&s, fmt, ap);
    if (size)
        buf[s.total < size ? s.total : size - 1] = 0;
    return (int)s.total;
}

int snprintf(char *buf, usize size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static int vfdprintf(int fd, const char *fmt, va_list ap)
{
    Sink s = {0, 0, 0, fd, {0}, 0};
    format(&s, fmt, ap);
    sink_flush(&s);
    return (int)s.total;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfdprintf(1, fmt, ap);
    va_end(ap);
    return n;
}

int fdprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    sys_write(1, s, strlen(s));
    sys_write(1, "\n", 1);
    return 0;
}

int putchar(int c)
{
    char ch = (char)c;
    sys_write(1, &ch, 1);
    return c;
}

/* ---- heap ---------------------------------------------------------------------------------------------------------- */
/* The heap is one run of blocks from heap_lo to heap_hi, each with a 16-byte header in front of its payload; the next
 * block starts right after the payload, so no list pointers are needed. malloc takes the first free block that is big
 * enough (splitting off the rest), free marks a block free and merges neighbours. Linear walks: simple, not fast. */
#define HDR 16
#define MIN_PAYLOAD 16
#define MAGIC_USED 0xA110CA7EA110CA7EULL
#define MAGIC_FREE 0xF4EEF4EEF4EEF4EEULL
#define GROW_MIN (64 * 1024)

typedef struct
{
    usize size;  /* payload bytes, a multiple of 16 */
    usize magic; /* MAGIC_USED / MAGIC_FREE */
} Block;

static char *heap_lo, *heap_hi;

void *sbrk(long long increment)
{
    long long cur = sys_brk(0);
    if (cur < 0)
        return (void *)-1;
    if (increment == 0)
        return (void *)cur;
    long long r = sys_brk(cur + increment);
    return r < 0 ? (void *)-1 : (void *)cur;
}

static Block *next_block(Block *b) { return (Block *)((char *)b + HDR + b->size); }

/* merge every run of free blocks into one */
static void coalesce(void)
{
    for (Block *b = (Block *)heap_lo; (char *)b < heap_hi;)
    {
        Block *n = next_block(b);
        if (b->magic == MAGIC_FREE && (char *)n < heap_hi && n->magic == MAGIC_FREE)
            b->size += HDR + n->size; /* look at b again: the next one may be free too */
        else
            b = n;
    }
}

/* ask the kernel for at least `need` more payload bytes; they become one free block at the end of the heap */
static int grow(usize need)
{
    usize bytes = (need + HDR + 4095) & ~(usize)4095;
    if (bytes < GROW_MIN)
        bytes = GROW_MIN;
    char *old = sbrk((long long)bytes);
    if (old == (char *)-1)
        return -1;
    if (!heap_lo)
    {
        heap_lo = (char *)(((usize)old + 15) & ~(usize)15);
        heap_hi = heap_lo;
        bytes -= (usize)(heap_lo - old);
    }
    Block *b = (Block *)heap_hi;
    b->size = bytes - HDR;
    b->magic = MAGIC_FREE;
    heap_hi += bytes;
    coalesce();
    return 0;
}

void *malloc(usize size)
{
    if (size > ((usize)1 << 40))
        return 0; /* no overflow in the rounding below, and nothing that big fits anyway */
    size = (size + 15) & ~(usize)15;
    if (size < MIN_PAYLOAD)
        size = MIN_PAYLOAD;
    for (int attempt = 0; attempt < 2; attempt++)
    {
        for (Block *b = (Block *)heap_lo; (char *)b < heap_hi; b = next_block(b))
        {
            if (b->magic != MAGIC_FREE || b->size < size)
                continue;
            if (b->size >= size + HDR + MIN_PAYLOAD) /* split: the rest stays free */
            {
                Block *rest = (Block *)((char *)b + HDR + size);
                rest->size = b->size - size - HDR;
                rest->magic = MAGIC_FREE;
                b->size = size;
            }
            b->magic = MAGIC_USED;
            return (char *)b + HDR;
        }
        if (attempt == 0 && grow(size) != 0)
            return 0;
    }
    return 0;
}

static Block *block_of(void *ptr)
{
    Block *b = (Block *)((char *)ptr - HDR);
    if ((char *)ptr < heap_lo + HDR || (char *)ptr >= heap_hi || ((usize)ptr & 15) || b->magic != MAGIC_USED)
    {
        fdprintf(2, "free/realloc: bad pointer %p\n", ptr);
        sys_exit(127);
    }
    return b;
}

void free(void *ptr)
{
    if (!ptr)
        return;
    block_of(ptr)->magic = MAGIC_FREE;
    coalesce();
}

void *calloc(usize count, usize size)
{
    if (size && count > ((usize)1 << 40) / size)
        return 0;
    void *p = malloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void *realloc(void *ptr, usize size)
{
    if (!ptr)
        return malloc(size);
    Block *b = block_of(ptr);
    if (size <= b->size)
        return ptr; /* never shrinks in place: the tail stays with the block */
    void *n = malloc(size);
    if (!n)
        return 0; /* the old block is untouched, as the standard says */
    memcpy(n, ptr, b->size);
    free(ptr);
    return n;
}

/* ---- processes ----------------------------------------------------------------------------------------------------------- */
/* fork: see libc.h. The kernel starts the child after the SYSCALL instruction with only rax (= 0), rsp and rip set, so everything
 * the C caller keeps in callee-saved registers is saved on the stack before the call (the child's stack is a copy) and put back
 * after it, in both processes. The Microsoft x64 convention: rbx, rbp, rdi, rsi, r12-r15 and xmm6-xmm15 are callee-saved.
 * Alignment: on entry rsp is 8 mod 16; eight pushes keep it so; 168 more make it 16-aligned for the movaps. */
#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)
__asm__(".text\n"
        ".p2align 4\n"
        ".globl fork\n"
        "fork:\n"
        "    pushq %rbx\n"
        "    pushq %rbp\n"
        "    pushq %rdi\n"
        "    pushq %rsi\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $168, %rsp\n"
        "    movaps %xmm6, 0(%rsp)\n"
        "    movaps %xmm7, 16(%rsp)\n"
        "    movaps %xmm8, 32(%rsp)\n"
        "    movaps %xmm9, 48(%rsp)\n"
        "    movaps %xmm10, 64(%rsp)\n"
        "    movaps %xmm11, 80(%rsp)\n"
        "    movaps %xmm12, 96(%rsp)\n"
        "    movaps %xmm13, 112(%rsp)\n"
        "    movaps %xmm14, 128(%rsp)\n"
        "    movaps %xmm15, 144(%rsp)\n"
        "    movl $" STRINGIFY(SYS_FORK) ", %eax\n"
        "    syscall\n"
        "    movaps 0(%rsp), %xmm6\n"
        "    movaps 16(%rsp), %xmm7\n"
        "    movaps 32(%rsp), %xmm8\n"
        "    movaps 48(%rsp), %xmm9\n"
        "    movaps 64(%rsp), %xmm10\n"
        "    movaps 80(%rsp), %xmm11\n"
        "    movaps 96(%rsp), %xmm12\n"
        "    movaps 112(%rsp), %xmm13\n"
        "    movaps 128(%rsp), %xmm14\n"
        "    movaps 144(%rsp), %xmm15\n"
        "    addq $168, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rbp\n"
        "    popq %rbx\n"
        "    ret\n");

int execv(const char *path, const char *const *argv) { return (int)sys_exec(path, argv); }
int dup2(int oldfd, int newfd) { return (int)sys_dup2(oldfd, newfd); }
int getpid(void) { return (int)sys_getpid(0); }
int getppid(void) { return (int)sys_getpid(1); }

/* ---- signals ------------------------------------------------------------------------------------------------------------- */
/* The stub the kernel starts a handler through (usersig.h): it arrives with rdx = the handler, r8 = the signal and rsp = 8 mod 16 as at
 * the entry of a function. It calls handler(sig) (Microsoft x64: argument in rcx, 32 bytes of shadow space) and then asks the kernel to
 * go back to where the program was. */
__asm__(".text\n"
        ".p2align 4\n"
        ".globl sigentry\n"
        "sigentry:\n"
        "    movl %r8d, %ecx\n"
        "    movq %rdx, %rax\n"
        "    subq $40, %rsp\n"
        "    call *%rax\n"
        "    addq $40, %rsp\n"
        "    movl $" STRINGIFY(SYS_SIGRETURN) ", %eax\n"
        "    syscall\n"
        "    ud2\n");

extern char sigentry[];

int signal(int sig, sighandler_t handler) { return (int)sys_sigaction(sig, (void *)handler, handler ? (void *)sigentry : 0); }
unsigned sigblock(unsigned mask) { return sys_sigmask(mask); }

/* ---- big stack frames ------------------------------------------------------------------------------------------------------------ */
/* The compiler calls __chkstk (rax = the size of the frame it is about to make) before a function whose frame is bigger than a page, so that
 * the pages are touched one after the other, from the top. The kernel grows the stack by itself on a page fault, but only for an access not
 * far below the stack pointer (process_stack_fault), so this walks the stack pointer down a page at a time, reads a byte at each step, and puts
 * it back. A frame can be as big as the stack may grow (ELF_STACK_RESERVE, 8 MiB); beyond that the read faults and the program ends. Every
 * register but rax, r10 and r11 is kept (r10 and r11 are saved here too, a Microsoft x64 caller does not expect them kept, but it is harmless). */
__asm__(".text\n"
        ".p2align 4\n"
        ".globl __chkstk\n"
        "__chkstk:\n"
        "    pushq %r10\n"
        "    pushq %r11\n"
        "    movq %rax, %r10\n"
        "    movq %rsp, %r11\n"
        "1:\n"
        "    cmpq $4096, %r10\n"
        "    jbe 2f\n"
        "    subq $4096, %rsp\n"
        "    testb %al, (%rsp)\n"
        "    subq $4096, %r10\n"
        "    jmp 1b\n"
        "2:\n"
        "    movq %r11, %rsp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    ret\n");

char *strdup(const char *s)
{
    usize n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* ---- stdio ----------------------------------------------------------------------------------------------------------------- */
/* A FILE is either a read stream (buf[rpos .. rlen) holds bytes read ahead) or a write stream (buf[0 .. wlen) holds bytes not yet
 * written). The kernel has no read+write open, so the two never mix and a seek just drops or flushes the buffer. */
#define F_READ 1
#define F_WRITE 2
#define F_EOF 4
#define F_ERR 8
#define F_UNBUF 16
#define F_HEAP 32 /* made by fopen: fclose frees it */
#define OPEN_STREAMS_MAX 32
#define SYS_WRITE_MAX 4096 /* one SYS_WRITE call sends at most this much (include/syscall.h) */

static FILE stdin_file = {0, F_READ, 0, 0, 0, {0}};
static FILE stdout_file = {1, F_WRITE | F_UNBUF, 0, 0, 0, {0}};
static FILE stderr_file = {2, F_WRITE | F_UNBUF, 0, 0, 0, {0}};
FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

static FILE *open_streams[OPEN_STREAMS_MAX]; /* for fflush(NULL) */

/* write all n bytes (a call takes at most SYS_WRITE_MAX and a drive may take fewer): 0, or -1 */
static int write_all(int fd, const char *p, usize n)
{
    while (n)
    {
        long long r = sys_write(fd, p, n < SYS_WRITE_MAX ? n : SYS_WRITE_MAX);
        if (r <= 0)
            return -1;
        p += r;
        n -= (usize)r;
    }
    return 0;
}

int fflush(FILE *f)
{
    if (!f)
    {
        int rc = 0;
        for (int i = 0; i < OPEN_STREAMS_MAX; i++)
            if (open_streams[i] && fflush(open_streams[i]) != 0)
                rc = EOF;
        return rc;
    }
    if ((f->flags & F_WRITE) && f->wlen)
    {
        int bad = write_all(f->fd, f->buf, (usize)f->wlen);
        f->wlen = 0;
        if (bad)
        {
            f->flags |= F_ERR;
            return EOF;
        }
    }
    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    unsigned oflags;
    int sflags;
    if (mode[0] == 'r')
    {
        oflags = 0;
        sflags = F_READ;
    }
    else if (mode[0] == 'w')
    {
        oflags = SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC;
        sflags = F_WRITE;
    }
    else if (mode[0] == 'a')
    {
        oflags = SYS_O_WRITE | SYS_O_CREATE | SYS_O_APPEND;
        sflags = F_WRITE;
    }
    else
        return 0;
    for (int i = 1; mode[i]; i++)
        if (mode[i] == '+')
            return 0; /* update modes need a read+write open the kernel does not have */
    int slot = -1;
    for (int i = 0; i < OPEN_STREAMS_MAX; i++)
        if (!open_streams[i])
        {
            slot = i;
            break;
        }
    if (slot < 0)
        return 0;
    FILE *f = malloc(sizeof(FILE));
    if (!f)
        return 0;
    long long fd = sys_open_flags(path, oflags);
    if (fd < 0)
    {
        free(f);
        return 0;
    }
    f->fd = (int)fd;
    f->flags = sflags | F_HEAP;
    f->rpos = f->rlen = f->wlen = 0;
    open_streams[slot] = f;
    return f;
}

int fclose(FILE *f)
{
    if (!f)
        return EOF;
    int rc = fflush(f);
    if (f->fd > 2 && sys_close(f->fd) < 0)
        rc = EOF;
    for (int i = 0; i < OPEN_STREAMS_MAX; i++)
        if (open_streams[i] == f)
            open_streams[i] = 0;
    if (f->flags & F_HEAP)
        free(f);
    return rc;
}

/* refill the read buffer: 1 = got bytes, 0 = end or error (flag set) */
static int fill(FILE *f)
{
    long long r = sys_read(f->fd, f->buf, BUFSIZ);
    f->rpos = f->rlen = 0;
    if (r > 0)
    {
        f->rlen = (int)r;
        return 1;
    }
    f->flags |= r == 0 ? F_EOF : F_ERR;
    return 0;
}

int fgetc(FILE *f)
{
    if (!(f->flags & F_READ))
    {
        f->flags |= F_ERR;
        return EOF;
    }
    if (f->rpos >= f->rlen && !fill(f))
        return EOF;
    return (unsigned char)f->buf[f->rpos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (c == EOF || !(f->flags & F_READ))
        return EOF;
    if (f->rpos == 0) /* nothing consumed from this buffer yet: make room in front */
    {
        if (f->rlen >= BUFSIZ)
            return EOF;
        memmove(f->buf + 1, f->buf, (usize)f->rlen);
        f->rlen++;
        f->rpos = 1;
    }
    f->buf[--f->rpos] = (char)c;
    f->flags &= ~F_EOF;
    return (unsigned char)c;
}

usize fread(void *ptr, usize size, usize count, FILE *f)
{
    if (!size || !count)
        return 0;
    if (!(f->flags & F_READ) || count > ((usize)1 << 40) / size)
    {
        f->flags |= F_ERR;
        return 0;
    }
    char *d = ptr;
    usize want = size * count, got = 0;
    while (got < want)
    {
        if (f->rpos < f->rlen)
        {
            usize n = (usize)(f->rlen - f->rpos);
            if (n > want - got)
                n = want - got;
            memcpy(d + got, f->buf + f->rpos, n);
            f->rpos += (int)n;
            got += n;
        }
        else if (want - got >= BUFSIZ) /* big request: straight into the caller's memory */
        {
            long long r = sys_read(f->fd, d + got, want - got);
            if (r <= 0)
            {
                f->flags |= r == 0 ? F_EOF : F_ERR;
                break;
            }
            got += (usize)r;
        }
        else if (!fill(f))
            break;
    }
    return got / size; /* whole items only, as the standard says */
}

usize fwrite(const void *ptr, usize size, usize count, FILE *f)
{
    if (!size || !count)
        return 0;
    if (!(f->flags & F_WRITE) || count > ((usize)1 << 40) / size)
    {
        f->flags |= F_ERR;
        return 0;
    }
    const char *p = ptr;
    usize n = size * count;
    if (f->flags & F_UNBUF)
    {
        if (write_all(f->fd, p, n))
        {
            f->flags |= F_ERR;
            return 0;
        }
        return count;
    }
    if (n >= BUFSIZ) /* too big to be worth copying: flush what is queued, then write it in one go */
    {
        if (fflush(f) || write_all(f->fd, p, n))
        {
            f->flags |= F_ERR;
            return 0;
        }
        return count;
    }
    if (f->wlen + n > BUFSIZ && fflush(f))
        return 0;
    memcpy(f->buf + f->wlen, p, n);
    f->wlen += (int)n;
    return count;
}

int fputc(int c, FILE *f)
{
    char ch = (char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? (unsigned char)ch : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }

int fputs(const char *s, FILE *f)
{
    usize n = strlen(s);
    return n == 0 || fwrite(s, 1, n, f) == n ? 0 : EOF;
}

char *fgets(char *s, int size, FILE *f)
{
    if (size <= 0)
        return 0;
    int i = 0;
    while (i < size - 1)
    {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return 0; /* end of file (or an error) with nothing read */
    s[i] = 0;
    return s;
}

int fseek(FILE *f, long long offset, int whence)
{
    if (fflush(f))
        return -1;
    if (whence == SEEK_CUR)
        offset -= f->rlen - f->rpos; /* the kernel's position is after the read-ahead */
    f->rpos = f->rlen = 0;
    f->flags &= ~F_EOF;
    return sys_seek(f->fd, offset, whence) < 0 ? -1 : 0;
}

long long ftell(FILE *f)
{
    long long pos = sys_seek(f->fd, 0, SEEK_CUR);
    if (pos < 0)
        return -1;
    return pos - (f->rlen - f->rpos) + f->wlen;
}

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    f->flags &= ~F_ERR;
}

int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE *f) { return f->fd; }

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char small[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    int rc = n;
    if (n >= (int)sizeof(small)) /* did not fit: format again into a block of the exact size */
    {
        char *big = malloc((usize)n + 1);
        if (!big)
            rc = -1;
        else
        {
            vsnprintf(big, (usize)n + 1, fmt, ap2);
            if (fwrite(big, 1, (usize)n, f) != (usize)n)
                rc = -1;
            free(big);
        }
    }
    else if (n && fwrite(small, 1, (usize)n, f) != (usize)n)
        rc = -1;
    va_end(ap2);
    return rc;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int remove(const char *path) { return sys_unlink(path) == 0 || sys_rmdir(path) == 0 ? 0 : -1; }
int rename(const char *from, const char *to) { return (int)sys_rename(from, to); }

/* ---- pthread mutexes and condition variables ----------------------------------------------------------------------------------- */
/* The kernel objects are named: a name made from the pid and a counter creates one, and is unlinked at once (the object stays while the
 * descriptor is open, see SYS_SYNC_UNLINK). */
static unsigned sync_counter;

static void sync_name(char *buf, char kind) { snprintf(buf, 32, "pt%c%d_%u", kind, getpid(), sync_counter++); }

static int mutex_fd(pthread_mutex_t *m)
{
    if (m->fd >= 0)
        return m->fd;
    char name[32];
    sync_name(name, 'm');
    long long fd = sys_mutex_open(name, SYS_CREATE | SYS_EXCL);
    if (fd < 0)
        return -1;
    sys_mutex_unlink(name);
    m->fd = (int)fd;
    return m->fd;
}

static int cond_fd(pthread_cond_t *c)
{
    if (c->fd >= 0)
        return c->fd;
    char name[32];
    sync_name(name, 'c');
    long long fd = sys_cond_open(name, SYS_CREATE | SYS_EXCL);
    if (fd < 0)
        return -1;
    sys_cond_unlink(name);
    c->fd = (int)fd;
    return c->fd;
}

int pthread_mutex_init(pthread_mutex_t *m, const void *attr_unused)
{
    (void)attr_unused;
    m->fd = -1;
    return mutex_fd(m) < 0 ? EAGAIN : 0;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    int fd = mutex_fd(m);
    if (fd < 0)
        return EAGAIN;
    return sys_mutex_lock(fd) == 0 ? 0 : EINVAL; /* -1 also covers "already held by the caller": waiting would deadlock */
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    int fd = mutex_fd(m);
    if (fd < 0)
        return EAGAIN;
    long long r = sys_mutex_try_lock(fd);
    return r == 0 ? 0 : r == 1 ? EBUSY : EINVAL;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->fd < 0)
        return EPERM;
    return sys_mutex_unlock(m->fd) == 0 ? 0 : EPERM;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    if (m->fd >= 0)
        sys_close(m->fd);
    m->fd = -1;
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const void *attr_unused)
{
    (void)attr_unused;
    c->fd = -1;
    return cond_fd(c) < 0 ? EAGAIN : 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    int cf = cond_fd(c);
    if (cf < 0 || m->fd < 0)
        return EINVAL;
    return sys_cond_wait(cf, m->fd) == 0 ? 0 : EINVAL;
}

int pthread_cond_signal(pthread_cond_t *c)
{
    int cf = cond_fd(c);
    return cf >= 0 && sys_cond_signal(cf) == 0 ? 0 : EINVAL;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    int cf = cond_fd(c);
    return cf >= 0 && sys_cond_broadcast(cf) == 0 ? 0 : EINVAL;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
    if (c->fd >= 0)
        sys_close(c->fd);
    c->fd = -1;
    return 0;
}
