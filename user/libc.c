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
