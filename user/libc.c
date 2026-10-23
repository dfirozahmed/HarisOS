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

/* ---- characters and numbers ---------------------------------------------------------------------------------------- */
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
