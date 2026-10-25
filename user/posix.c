/* HarisOS user C library, part 3: what POSIX / ISO C programs expect around the basics (see libc.h): errno and strerror, exit and abort,
 * setjmp / longjmp and the smaller string functions (numconv.c has strtod and the digits behind printf's %e %f %g). */
#include "libc.h"
#include <math.h>
#include <setjmp.h>

int errno;

/* the compiler for this target wants this symbol in any program that touches floating point (weak: a program may define its own) */
__attribute__((weak)) int _fltused = 1;

/* ---- errno and strerror -------------------------------------------------------------------------------------------------------- */
const char *strerror(int e)
{
    switch (e)
    {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EIO: return "Input/output error";
    case EBADF: return "Bad file descriptor";
    case ECHILD: return "No child processes";
    case EAGAIN: return "Resource temporarily unavailable";
    case ENOMEM: return "Cannot allocate memory";
    case EACCES: return "Permission denied";
    case EFAULT: return "Bad address";
    case EBUSY: return "Device or resource busy";
    case EEXIST: return "File exists";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case EMFILE: return "Too many open files";
    case ENOSPC: return "No space left on device";
    case ESPIPE: return "Illegal seek";
    case EDOM: return "Numerical argument out of domain";
    case ERANGE: return "Numerical result out of range";
    case ENOSYS: return "Function not implemented";
    default: return "Unknown error";
    }
}

/* ---- exit, abort, atexit ------------------------------------------------------------------------------------------------------- */
#define ATEXIT_MAX 32
static void (*atexit_fns[ATEXIT_MAX])(void);
static int atexit_count;

int atexit(void (*fn)(void))
{
    if (atexit_count == ATEXIT_MAX)
        return -1;
    atexit_fns[atexit_count++] = fn;
    return 0;
}

void exit(int code)
{
    while (atexit_count > 0)
        atexit_fns[--atexit_count]();
    fflush(0); /* every open stream */
    sys_exit(code);
    for (;;)
        ;
}

void _exit(int code)
{
    sys_exit(code);
    for (;;)
        ;
}

void abort(void)
{
    fflush(0);
    sys_exit(134); /* 128 + SIGABRT, what a shell shows for an aborted program */
    for (;;)
        ;
}

void __assert_fail(const char *expr, const char *file, int line)
{
    fflush(0);
    fdprintf(2, "assertion failed: %s (%s:%d)\n", expr, file, line);
    abort();
}

char *getenv(const char *name)
{
    (void)name; /* HarisOS processes have no environment yet */
    return 0;
}

/* ---- setjmp / longjmp ---------------------------------------------------------------------------------------------------------- */
/* Layout of the buffer (include/setjmp.h): bytes 0 .. 79 rbx rbp rdi rsi rsp r12 r13 r14 r15 rip, bytes 80 .. 239 xmm6 .. xmm15.
 * The first argument arrives in rcx (Microsoft x64). rsp is stored as it will be after the return, rip is the return address. */
__asm__(".text\n"
        ".p2align 4\n"
        ".globl setjmp\n"
        ".globl _setjmp\n"
        "setjmp:\n"
        "_setjmp:\n"
        "    movq %rbx, 0(%rcx)\n"
        "    movq %rbp, 8(%rcx)\n"
        "    movq %rdi, 16(%rcx)\n"
        "    movq %rsi, 24(%rcx)\n"
        "    leaq 8(%rsp), %rax\n"
        "    movq %rax, 32(%rcx)\n"
        "    movq %r12, 40(%rcx)\n"
        "    movq %r13, 48(%rcx)\n"
        "    movq %r14, 56(%rcx)\n"
        "    movq %r15, 64(%rcx)\n"
        "    movq (%rsp), %rax\n"
        "    movq %rax, 72(%rcx)\n"
        "    movups %xmm6, 80(%rcx)\n"
        "    movups %xmm7, 96(%rcx)\n"
        "    movups %xmm8, 112(%rcx)\n"
        "    movups %xmm9, 128(%rcx)\n"
        "    movups %xmm10, 144(%rcx)\n"
        "    movups %xmm11, 160(%rcx)\n"
        "    movups %xmm12, 176(%rcx)\n"
        "    movups %xmm13, 192(%rcx)\n"
        "    movups %xmm14, 208(%rcx)\n"
        "    movups %xmm15, 224(%rcx)\n"
        "    xorl %eax, %eax\n"
        "    ret\n"
        ".p2align 4\n"
        ".globl longjmp\n"
        ".globl _longjmp\n"
        "longjmp:\n"
        "_longjmp:\n"
        "    movl %edx, %eax\n"
        "    testl %eax, %eax\n"
        "    jnz 1f\n"
        "    movl $1, %eax\n" /* longjmp(env, 0) makes setjmp return 1 */
        "1:\n"
        "    movq 0(%rcx), %rbx\n"
        "    movq 8(%rcx), %rbp\n"
        "    movq 16(%rcx), %rdi\n"
        "    movq 24(%rcx), %rsi\n"
        "    movq 40(%rcx), %r12\n"
        "    movq 48(%rcx), %r13\n"
        "    movq 56(%rcx), %r14\n"
        "    movq 64(%rcx), %r15\n"
        "    movups 80(%rcx), %xmm6\n"
        "    movups 96(%rcx), %xmm7\n"
        "    movups 112(%rcx), %xmm8\n"
        "    movups 128(%rcx), %xmm9\n"
        "    movups 144(%rcx), %xmm10\n"
        "    movups 160(%rcx), %xmm11\n"
        "    movups 176(%rcx), %xmm12\n"
        "    movups 192(%rcx), %xmm13\n"
        "    movups 208(%rcx), %xmm14\n"
        "    movups 224(%rcx), %xmm15\n"
        "    movq 32(%rcx), %rsp\n"
        "    jmpq *72(%rcx)\n");

/* ---- more string functions ----------------------------------------------------------------------------------------------------- */
int strcoll(const char *a, const char *b) { return strcmp(a, b); } /* the only locale is "C" */

usize strspn(const char *s, const char *accept)
{
    usize n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

usize strcspn(const char *s, const char *reject)
{
    usize n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++)
        if (strchr(accept, *s))
            return (char *)s;
    return 0;
}

usize strnlen(const char *s, usize max)
{
    usize n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d || !*a)
            return d;
    }
}

int strncasecmp(const char *a, const char *b, usize n)
{
    for (; n; n--, a++, b++)
    {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d || !*a)
            return d;
    }
    return 0;
}

int isxdigit(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int ispunct(int c) { return isprint(c) && !isalnum(c) && c != ' '; }
int isgraph(int c) { return c > 32 && c < 127; }
long labs(long x) { return x < 0 ? -x : x; }

void *bsearch(const void *key, const void *base, usize count, usize size, int (*cmp)(const void *, const void *))
{
    usize lo = 0, hi = count;
    while (lo < hi)
    {
        usize mid = lo + (hi - lo) / 2;
        const char *p = (const char *)base + mid * size;
        int c = cmp(key, p);
        if (c == 0)
            return (void *)p;
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return 0;
}

long strtol(const char *s, char **end, int base)
{
    long long v = strtoll(s, end, base);
    if (v > 2147483647LL)
    {
        errno = ERANGE;
        return 2147483647L;
    }
    if (v < -2147483647LL - 1)
    {
        errno = ERANGE;
        return -2147483647L - 1;
    }
    return (long)v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    unsigned long long v = strtoull(s, end, base);
    if (v > 0xFFFFFFFFULL)
    {
        errno = ERANGE;
        return 0xFFFFFFFFUL;
    }
    return (unsigned long)v;
}

/* the "C" locale is the only one */
static char locale_name[] = "C";
char *setlocale(int category, const char *name)
{
    (void)category;
    if (name && *name && strcmp(name, "C") != 0 && strcmp(name, "POSIX") != 0)
        return 0;
    return locale_name;
}

static struct lconv the_lconv = {".", "", "", "", "", "", "", "", "", ""};
struct lconv *localeconv(void) { return &the_lconv; }


/* ---- time ---------------------------------------------------------------------------------------------------------------------- */
time_t time(time_t *out)
{
    time_t t = sys_time();
    if (out)
        *out = t;
    return t;
}

clock_t clock(void) { return sys_clock(); }

double difftime(time_t a, time_t b) { return (double)(a - b); }

/* days since 1970-01-01 -> calendar date, the usual "civil from days" arithmetic (the year starts in March) */
struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    long long secs = *t;
    long long days = secs / 86400, rem = secs % 86400;
    if (rem < 0)
    {
        rem += 86400;
        days--;
    }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    long long wd = (days + 4) % 7; /* 1970-01-01 was a Thursday */
    out->tm_wday = (int)(wd < 0 ? wd + 7 : wd);
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    out->tm_mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    out->tm_mon = m - 1;
    out->tm_year = (int)(y + (m <= 2) - 1900);
    int leap = ((y + (m <= 2)) % 4 == 0 && (y + (m <= 2)) % 100 != 0) || (y + (m <= 2)) % 400 == 0;
    static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    out->tm_yday = before[m - 1] + out->tm_mday - 1 + (leap && m > 2);
    out->tm_isdst = 0;
    return out;
}

static struct tm tm_static;
struct tm *gmtime(const time_t *t) { return gmtime_r(t, &tm_static); }
struct tm *localtime(const time_t *t) { return gmtime_r(t, &tm_static); }

/* tm fields may be out of range (month 14, day 0 ...): they are normalised, and *tm is updated, as the standard says */
time_t timegm(struct tm *tm)
{
    long long y = (long long)tm->tm_year + 1900 + tm->tm_mon / 12;
    int mon = tm->tm_mon % 12;
    if (mon < 0)
    {
        mon += 12;
        y--;
    }
    long long yy = y - (mon < 2);
    long long era = (yy >= 0 ? yy : yy - 399) / 400;
    long long yoe = yy - era * 400;
    long long mp = (mon + 10) % 12; /* March = 0 */
    long long doy = (153 * mp + 2) / 5 + tm->tm_mday - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    time_t t = days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
    gmtime_r(&t, tm);
    return t;
}

time_t mktime(struct tm *tm) { return timegm(tm); }

static int put_num(char *buf, usize size, usize *pos, long long v, int width, char pad)
{
    char tmp[24];
    int n = 0;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do
        tmp[n++] = (char)('0' + u % 10);
    while ((u /= 10) != 0);
    if (v < 0)
        tmp[n++] = '-';
    while (n < width)
        tmp[n++] = pad;
    while (n)
    {
        if (*pos + 1 >= size)
            return -1;
        buf[(*pos)++] = tmp[--n];
    }
    return 0;
}

usize strftime(char *buf, usize size, const char *fmt, const struct tm *tm)
{
    static const char *const day[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const mon[12] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    usize pos = 0;
    if (size == 0)
        return 0;
    for (; *fmt; fmt++)
    {
        if (*fmt != '%')
        {
            if (pos + 1 >= size)
                return 0;
            buf[pos++] = *fmt;
            continue;
        }
        const char *text = 0;
        int textlen = 0, bad = 0;
        char c = *++fmt;
        char sub[32]; /* composite conversions are formatted by a nested call */
        switch (c)
        {
        case 'Y': bad = put_num(buf, size, &pos, tm->tm_year + 1900LL, 1, '0'); break;
        case 'y': bad = put_num(buf, size, &pos, (tm->tm_year + 1900) % 100, 2, '0'); break;
        case 'm': bad = put_num(buf, size, &pos, tm->tm_mon + 1, 2, '0'); break;
        case 'd': bad = put_num(buf, size, &pos, tm->tm_mday, 2, '0'); break;
        case 'e': bad = put_num(buf, size, &pos, tm->tm_mday, 2, ' '); break;
        case 'H': bad = put_num(buf, size, &pos, tm->tm_hour, 2, '0'); break;
        case 'I': bad = put_num(buf, size, &pos, tm->tm_hour % 12 == 0 ? 12 : tm->tm_hour % 12, 2, '0'); break;
        case 'M': bad = put_num(buf, size, &pos, tm->tm_min, 2, '0'); break;
        case 'S': bad = put_num(buf, size, &pos, tm->tm_sec, 2, '0'); break;
        case 'j': bad = put_num(buf, size, &pos, tm->tm_yday + 1, 3, '0'); break;
        case 'w': bad = put_num(buf, size, &pos, tm->tm_wday, 1, '0'); break;
        case 'A': text = day[(unsigned)tm->tm_wday % 7]; break;
        case 'a': text = day[(unsigned)tm->tm_wday % 7]; textlen = 3; break;
        case 'B': text = mon[(unsigned)tm->tm_mon % 12]; break;
        case 'b':
        case 'h': text = mon[(unsigned)tm->tm_mon % 12]; textlen = 3; break;
        case 'p': text = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'Z': text = "UTC"; break;
        case 'z': text = "+0000"; break;
        case 'n': text = "\n"; break;
        case 't': text = "\t"; break;
        case '%': text = "%"; break;
        case 'F': strftime(sub, sizeof(sub), "%Y-%m-%d", tm); text = sub; break;
        case 'T': strftime(sub, sizeof(sub), "%H:%M:%S", tm); text = sub; break;
        case 'R': strftime(sub, sizeof(sub), "%H:%M", tm); text = sub; break;
        case 'D': strftime(sub, sizeof(sub), "%m/%d/%y", tm); text = sub; break;
        case 'c': strftime(sub, sizeof(sub), "%a %b %e %H:%M:%S %Y", tm); text = sub; break;
        case 'x': strftime(sub, sizeof(sub), "%m/%d/%y", tm); text = sub; break;
        case 'X': strftime(sub, sizeof(sub), "%H:%M:%S", tm); text = sub; break;
        default: return 0; /* a conversion this library does not know (and the end of the format after a lone %) */
        }
        if (bad)
            return 0;
        if (text)
        {
            if (!textlen)
                textlen = (int)strlen(text);
            if (pos + (usize)textlen + 1 > size)
                return 0;
            for (int i = 0; i < textlen; i++)
                buf[pos++] = text[i];
        }
    }
    buf[pos] = 0;
    return pos;
}

int raise(int sig) { return (int)sys_kill(sys_getpid(0), sig); }
