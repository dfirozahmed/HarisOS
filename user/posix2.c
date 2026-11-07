/* HarisOS user C library, part 5: more POSIX for ported programs (the NetSurf browser first): files and folders with POSIX names,
 * stat, getopt, sscanf, a small iconv, inet_pton, float math, and some string and time functions. See libc.h and include/ for the
 * declarations. Everything sits on the system calls of sys.h. */
#include "libc.h"
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <dirent.h>
#include <limits.h>
#include <iconv.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <arpa/inet.h>
#include <sys/select.h>

#ifndef E2BIG
#define E2BIG 7
#endif
#ifndef EILSEQ
#define EILSEQ 84
#endif

/* ---- strings ------------------------------------------------------------------------------------------------------------------- */
char *strndup(const char *s, usize n)
{
    usize len = strnlen(s, n);
    char *p = malloc(len + 1);
    if (p)
    {
        memcpy(p, s, len);
        p[len] = 0;
    }
    return p;
}

char *strtok_r(char *s, const char *delim, char **save)
{
    if (!s)
        s = *save;
    s += strspn(s, delim);
    if (!*s)
    {
        *save = s;
        return NULL;
    }
    char *tok = s;
    s += strcspn(s, delim);
    if (*s)
        *s++ = 0;
    *save = s;
    return tok;
}

char *strtok(char *s, const char *delim)
{
    static char *save;
    return strtok_r(s, delim, &save);
}

char *strcasestr(const char *hay, const char *needle)
{
    usize n = strlen(needle);
    if (n == 0)
        return (char *)hay;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0)
            return (char *)hay;
    return NULL;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }
int isascii(int c) { return (c & ~0x7F) == 0; }

/* ---- float math ---------------------------------------------------------------------------------------------------------------- */
float fabsf(float x) { return (float)fabs(x); }
float sqrtf(float x) { return (float)sqrt(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float roundf(float x) { return (float)round(x); }
float truncf(float x) { return (float)trunc(x); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
float powf(float x, float y) { return (float)pow(x, y); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float expf(float x) { return (float)exp(x); }
float logf(float x) { return (float)log(x); }

/* ---- time text ----------------------------------------------------------------------------------------------------------------- */
char *asctime(const struct tm *tm)
{
    static char buf[32];
    static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const mons[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (tm->tm_wday < 0 || tm->tm_wday > 6 || tm->tm_mon < 0 || tm->tm_mon > 11)
        return NULL;
    snprintf(buf, sizeof buf, "%s %s %2d %02d:%02d:%02d %d\n", days[tm->tm_wday], mons[tm->tm_mon], tm->tm_mday, tm->tm_hour, tm->tm_min,
             tm->tm_sec, tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *t)
{
    struct tm tm;
    gmtime_r(t, &tm);
    return asctime(&tm);
}

/* ---- files with POSIX names ---------------------------------------------------------------------------------------------------- */
int open(const char *path, int flags, ...)
{
    unsigned f = 0;
    if ((flags & 3) != O_RDONLY)
        f |= SYS_O_WRITE;
    if (flags & O_CREAT)
        f |= SYS_O_CREATE | SYS_O_WRITE;
    if (flags & O_TRUNC)
        f |= SYS_O_TRUNC;
    if (flags & O_APPEND)
        f |= SYS_O_APPEND;
    long long fd = sys_open_flags(path, f);
    if (fd < 0)
    {
        errno = ENOENT;
        return -1;
    }
    return (int)fd;
}

int fcntl(int fd, int cmd, ...)
{
    (void)fd;
    (void)cmd;
    return 0;
}

ssize_t read(int fd, void *buf, usize n) { return sys_read(fd, buf, n); }
ssize_t write(int fd, const void *buf, usize n) { return sys_write(fd, buf, n); }
int close(int fd) { return (int)sys_close(fd); }
off_t lseek(int fd, off_t off, int whence) { return sys_seek(fd, off, whence); }
int unlink(const char *path) { return (int)sys_unlink(path); }
int rmdir(const char *path) { return (int)sys_rmdir(path); }
int chdir(const char *path) { return (int)sys_chdir(path); }
int isatty(int fd) { return fd >= 0 && fd <= 2; }

int ftruncate(int fd, off_t length)
{
    (void)fd;
    (void)length;
    errno = ENOSYS;
    return -1;
}

char *getcwd(char *buf, usize size)
{
    if (!buf || size == 0)
    {
        errno = EINVAL;
        return NULL;
    }
    if (sys_getcwd(buf, size) < 0)
    {
        errno = ERANGE;
        return NULL;
    }
    return buf;
}

int stat(const char *path, struct stat *st)
{
    DirEntry e;
    memset(st, 0, sizeof *st);
    if (sys_stat(path, &e) < 0)
    {
        errno = ENOENT;
        return -1;
    }
    st->st_nlink = 1;
    st->st_size = e.size;
    st->st_mode = e.is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    return 0;
}

int lstat(const char *path, struct stat *st) { return stat(path, st); }

int fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_nlink = 1;
    st->st_mode = S_IFREG | 0644;
    long long cur = sys_seek(fd, 0, SEEK_CUR);
    if (cur < 0)
    {
        if (fd <= 2)
            return 0; /* a console or pipe: no size */
        errno = EBADF;
        return -1;
    }
    long long end = sys_seek(fd, 0, SEEK_END);
    sys_seek(fd, cur, SEEK_SET);
    st->st_size = end < 0 ? 0 : end;
    return 0;
}

int mkdir(const char *path, mode_t mode)
{
    (void)mode;
    return (int)sys_mkdir(path);
}

int access(const char *path, int mode)
{
    struct stat st;
    (void)mode;
    return stat(path, &st);
}

char *realpath(const char *path, char *resolved)
{
    char tmp[PATH_MAX];
    if (path[0] == '/')
        snprintf(tmp, sizeof tmp, "%s", path);
    else
    {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd))
            return NULL;
        snprintf(tmp, sizeof tmp, "%s%s%s", cwd, strcmp(cwd, "/") == 0 ? "" : "/", path);
    }
    struct stat st;
    if (stat(tmp, &st) < 0)
        return NULL;
    if (!resolved)
        resolved = malloc(PATH_MAX);
    if (resolved)
        snprintf(resolved, PATH_MAX, "%s", tmp);
    return resolved;
}

int uname(struct utsname *u)
{
    memset(u, 0, sizeof *u);
    strcpy(u->sysname, "HarisOS");
    strcpy(u->nodename, "haris");
    strcpy(u->release, "1");
    strcpy(u->version, "1");
    strcpy(u->machine, "x86_64");
    return 0;
}

/* ---- folders ------------------------------------------------------------------------------------------------------------------- */
struct DIR_
{
    char path[PATH_MAX];
    unsigned index;
    struct dirent ent;
};

DIR *opendir(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
    {
        errno = ENOTDIR;
        return NULL;
    }
    DIR *d = calloc(1, sizeof *d);
    if (!d)
    {
        errno = ENOMEM;
        return NULL;
    }
    snprintf(d->path, sizeof d->path, "%s", path);
    return d;
}

struct dirent *readdir(DIR *d)
{
    DirEntry e;
    if (sys_readdir(d->path, d->index, &e) != 1)
        return NULL;
    d->index++;
    d->ent.d_ino = d->index;
    d->ent.d_type = e.is_dir ? DT_DIR : DT_REG;
    snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", e.name);
    return &d->ent;
}

int closedir(DIR *d)
{
    free(d);
    return 0;
}

/* ---- getopt -------------------------------------------------------------------------------------------------------------------- */
char *optarg;
int optind = 1, opterr = 1, optopt;

int getopt(int argc, char *const argv[], const char *optstring)
{
    static int pos;
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] == 0)
        return -1;
    if (strcmp(argv[optind], "--") == 0)
    {
        optind++;
        return -1;
    }
    if (pos == 0)
        pos = 1;
    int c = (unsigned char)argv[optind][pos];
    const char *spec = strchr(optstring, c);
    if (!spec || c == ':')
    {
        optopt = c;
        if (argv[optind][++pos] == 0)
        {
            optind++;
            pos = 0;
        }
        return '?';
    }
    if (spec[1] == ':')
    {
        if (argv[optind][pos + 1])
            optarg = &argv[optind][pos + 1];
        else if (optind + 1 < argc)
            optarg = argv[++optind];
        else
        {
            optopt = c;
            optind++;
            pos = 0;
            return optstring[0] == ':' ? ':' : '?';
        }
        optind++;
        pos = 0;
        return c;
    }
    if (argv[optind][++pos] == 0)
    {
        optind++;
        pos = 0;
    }
    return c;
}

/* "--name", "--name=value" and "--name value" for the long table; anything else goes to getopt. */
int getopt_long(int argc, char *const argv[], const char *optstring, const struct option *longopts, int *longindex)
{
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] != '-' || argv[optind][2] == 0)
        return getopt(argc, argv, optstring);
    const char *arg = argv[optind] + 2;
    usize nlen = strcspn(arg, "=");
    for (int i = 0; longopts && longopts[i].name; i++)
    {
        if (strlen(longopts[i].name) != nlen || strncmp(longopts[i].name, arg, nlen) != 0)
            continue;
        optind++;
        optarg = NULL;
        if (arg[nlen] == '=')
            optarg = (char *)arg + nlen + 1;
        else if (longopts[i].has_arg == required_argument && optind < argc)
            optarg = argv[optind++];
        if (longopts[i].has_arg == required_argument && !optarg)
            return '?';
        if (longindex)
            *longindex = i;
        if (longopts[i].flag)
        {
            *longopts[i].flag = longopts[i].val;
            return 0;
        }
        return longopts[i].val;
    }
    optind++;
    return '?';
}

/* ---- inet ---------------------------------------------------------------------------------------------------------------------- */
static int parse_v4(const char *s, uint8_t out[4], const char **endp)
{
    for (int i = 0; i < 4; i++)
    {
        if (i && *s++ != '.')
            return 0;
        if (!isdigit((unsigned char)*s))
            return 0;
        unsigned v = 0, digits = 0;
        while (isdigit((unsigned char)*s) && digits < 4)
        {
            v = v * 10 + (unsigned)(*s++ - '0');
            digits++;
        }
        if (v > 255 || digits > 3)
            return 0;
        out[i] = (uint8_t)v;
    }
    *endp = s;
    return 1;
}

int inet_aton(const char *s, struct in_addr *a)
{
    uint8_t b[4];
    const char *end;
    if (!parse_v4(s, b, &end) || (*end && !isspace((unsigned char)*end)))
        return 0;
    memcpy(&a->s_addr, b, 4);
    return 1;
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af == AF_INET)
    {
        const char *end;
        uint8_t b[4];
        if (!parse_v4(src, b, &end) || *end)
            return 0;
        memcpy(dst, b, 4);
        return 1;
    }
    if (af != AF_INET6)
    {
        errno = EINVAL;
        return -1;
    }
    uint16_t w[8];
    int n = 0, gap = -1;
    const char *s = src;
    if (s[0] == ':')
    {
        if (s[1] != ':')
            return 0;
        s++;
    }
    while (*s)
    {
        if (*s == ':')
        {
            if (gap >= 0)
                return 0;
            gap = n;
            s++;
            continue;
        }
        const char *end;
        uint8_t b4[4];
        if (n <= 6 && strchr(s, '.') && parse_v4(s, b4, &end) && !*end) /* embedded IPv4 as the last 32 bits */
        {
            w[n++] = (uint16_t)((b4[0] << 8) | b4[1]);
            w[n++] = (uint16_t)((b4[2] << 8) | b4[3]);
            break;
        }
        unsigned v = 0, digits = 0;
        while (isxdigit((unsigned char)*s) && digits < 5)
        {
            int c = tolower((unsigned char)*s++);
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
            digits++;
        }
        if (digits == 0 || digits > 4 || n >= 8)
            return 0;
        w[n++] = (uint16_t)v;
        if (*s == ':')
        {
            s++;
            if (!*s)
                return 0;
        }
        else if (*s)
            return 0;
    }
    uint16_t full[8] = {0};
    if (gap < 0)
    {
        if (n != 8)
            return 0;
        memcpy(full, w, sizeof full);
    }
    else
    {
        if (n > 7)
            return 0;
        for (int i = 0; i < gap; i++)
            full[i] = w[i];
        for (int i = gap; i < n; i++)
            full[8 - (n - i)] = w[i];
    }
    uint8_t *o = dst;
    for (int i = 0; i < 8; i++)
    {
        o[2 * i] = (uint8_t)(full[i] >> 8);
        o[2 * i + 1] = (uint8_t)full[i];
    }
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    const uint8_t *b = src;
    char tmp[INET6_ADDRSTRLEN];
    if (af == AF_INET)
        snprintf(tmp, sizeof tmp, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
    else if (af == AF_INET6)
    {
        /* the longest run of zero groups becomes "::" */
        int best = -1, bestlen = 0;
        for (int i = 0; i < 8;)
        {
            int j = i;
            while (j < 8 && b[2 * j] == 0 && b[2 * j + 1] == 0)
                j++;
            if (j - i > bestlen && j - i >= 2)
            {
                best = i;
                bestlen = j - i;
            }
            i = j > i ? j : i + 1;
        }
        char *p = tmp;
        for (int i = 0; i < 8; i++)
        {
            if (i == best)
            {
                *p++ = ':';
                i += bestlen - 1;
                if (i == 7)
                    *p++ = ':';
                continue;
            }
            if (i)
                *p++ = ':';
            p += snprintf(p, 5, "%x", (b[2 * i] << 8) | b[2 * i + 1]);
        }
        *p = 0;
    }
    else
    {
        errno = EINVAL;
        return NULL;
    }
    if (strlen(tmp) + 1 > size)
    {
        errno = ERANGE;
        return NULL;
    }
    strcpy(dst, tmp);
    return dst;
}

/* ---- sscanf -------------------------------------------------------------------------------------------------------------------- */
static int digit_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 99;
}

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    const char *start = s;
    int done = 0;
    while (*fmt)
    {
        if (isspace((unsigned char)*fmt))
        {
            while (isspace((unsigned char)*s))
                s++;
            fmt++;
            continue;
        }
        if (*fmt != '%')
        {
            if (*s != *fmt)
                return done == 0 && !*s ? EOF : done;
            s++;
            fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%')
        {
            if (*s != '%')
                return done;
            s++;
            fmt++;
            continue;
        }
        int suppress = 0, width = 0, lng = 0; /* lng: -2 hh, -1 h, 0 int, 1 l, 2 ll / z / j / t */
        if (*fmt == '*')
        {
            suppress = 1;
            fmt++;
        }
        while (isdigit((unsigned char)*fmt))
            width = width * 10 + (*fmt++ - '0');
        for (;; fmt++)
        {
            if (*fmt == 'h')
                lng = lng == -1 ? -2 : -1;
            else if (*fmt == 'l')
                lng = lng == 1 ? 2 : 1;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'L' || *fmt == 'q')
                lng = 2;
            else
                break;
        }
        int conv = *fmt ? *fmt++ : 0;
        if (conv != 'c' && conv != '[' && conv != 'n')
            while (isspace((unsigned char)*s))
                s++;
        if (conv != 'n' && !*s)
            return done == 0 ? EOF : done;
        int maxw = width ? width : 0x7FFFFFFF;

        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p')
        {
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'o' ? 8 : conv == 'i' ? 0 : 16;
            int neg = 0, used = 0;
            const char *p = s;
            if (maxw > 0 && (*p == '-' || *p == '+'))
            {
                neg = *p == '-';
                p++;
                used++;
            }
            if ((base == 0 || base == 16) && used + 1 < maxw && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
                digit_value((unsigned char)p[2]) < 16)
            {
                p += 2;
                used += 2;
                base = 16;
            }
            else if (base == 0)
                base = p[0] == '0' ? 8 : 10;
            unsigned long long v = 0;
            int nd = 0;
            while (used < maxw && digit_value((unsigned char)*p) < base)
            {
                v = v * (unsigned)base + (unsigned)digit_value((unsigned char)*p);
                p++;
                used++;
                nd++;
            }
            if (nd == 0)
                return done;
            s = p;
            if (neg)
                v = (unsigned long long)(-(long long)v);
            if (!suppress)
            {
                if (lng >= 2 || conv == 'p')
                    *va_arg(ap, unsigned long long *) = v;
                else if (lng == 1)
                    *va_arg(ap, unsigned long *) = (unsigned long)v;
                else if (lng == -1)
                    *va_arg(ap, unsigned short *) = (unsigned short)v;
                else if (lng == -2)
                    *va_arg(ap, unsigned char *) = (unsigned char)v;
                else
                    *va_arg(ap, unsigned *) = (unsigned)v;
                done++;
            }
        }
        else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G' || conv == 'a')
        {
            char buf[80];
            int n = 0;
            while (n < maxw && n < (int)sizeof buf - 1 &&
                   (isalnum((unsigned char)s[n]) || s[n] == '.' ||
                    ((s[n] == '+' || s[n] == '-') &&
                     (n == 0 || tolower((unsigned char)s[n - 1]) == 'e' || tolower((unsigned char)s[n - 1]) == 'p'))))
            {
                buf[n] = s[n];
                n++;
            }
            buf[n] = 0;
            char *end;
            double v = strtod(buf, &end);
            if (end == buf)
                return done;
            s += end - buf;
            if (!suppress)
            {
                if (lng >= 1)
                    *va_arg(ap, double *) = v;
                else
                    *va_arg(ap, float *) = (float)v;
                done++;
            }
        }
        else if (conv == 's')
        {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while (n < maxw && *s && !isspace((unsigned char)*s))
            {
                if (out)
                    out[n] = *s;
                s++;
                n++;
            }
            if (out)
            {
                out[n] = 0;
                done++;
            }
        }
        else if (conv == 'c')
        {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = width ? width : 1;
            for (int i = 0; i < n; i++)
            {
                if (!*s)
                    return done == 0 ? EOF : done;
                if (out)
                    out[i] = *s;
                s++;
            }
            if (out)
                done++;
        }
        else if (conv == '[')
        {
            int negate = 0;
            if (*fmt == '^')
            {
                negate = 1;
                fmt++;
            }
            const char *set = fmt;
            if (*fmt == ']')
                fmt++; /* a leading ] is a member */
            while (*fmt && *fmt != ']')
                fmt++;
            const char *setend = fmt;
            if (*fmt == ']')
                fmt++;
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while (n < maxw && *s)
            {
                int in = 0;
                for (const char *q = set; q < setend; q++)
                {
                    if (q + 2 < setend && q[1] == '-')
                    {
                        if ((unsigned char)*s >= (unsigned char)q[0] && (unsigned char)*s <= (unsigned char)q[2])
                            in = 1;
                        q += 2;
                    }
                    else if (*q == *s)
                        in = 1;
                }
                if (in == negate)
                    break;
                if (out)
                    out[n] = *s;
                s++;
                n++;
            }
            if (n == 0)
                return done;
            if (out)
            {
                out[n] = 0;
                done++;
            }
        }
        else if (conv == 'n')
        {
            if (!suppress)
                *va_arg(ap, int *) = (int)(s - start);
        }
        else
            return done;
    }
    return done;
}

int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- iconv --------------------------------------------------------------------------------------------------------------------- */
enum { ENC_UTF8, ENC_ASCII, ENC_LATIN1, ENC_CP1252 };
struct iconv_cd
{
    int to, from;
    int translit, ignore;
};

static const unsigned short cp1252_hi[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                             0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                             0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

static int enc_by_name(const char *name, int *translit, int *ignore)
{
    char n[40];
    usize i = 0;
    for (; name[i] && name[i] != '/' && i < sizeof n - 1; i++)
    {
        int c = (unsigned char)name[i];
        n[i] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    n[i] = 0;
    if (strstr(name, "//TRANSLIT"))
        *translit = 1;
    if (strstr(name, "//IGNORE"))
        *ignore = 1;
    if (!strcmp(n, "UTF-8") || !strcmp(n, "UTF8"))
        return ENC_UTF8;
    if (!strcmp(n, "ASCII") || !strcmp(n, "US-ASCII") || !strcmp(n, "ANSI_X3.4-1968"))
        return ENC_ASCII;
    if (!strcmp(n, "ISO-8859-1") || !strcmp(n, "ISO8859-1") || !strcmp(n, "LATIN1") || !strcmp(n, "ISO_8859-1"))
        return ENC_LATIN1;
    if (!strcmp(n, "WINDOWS-1252") || !strcmp(n, "CP1252"))
        return ENC_CP1252;
    return -1;
}

iconv_t iconv_open(const char *to, const char *from)
{
    struct iconv_cd *cd = calloc(1, sizeof *cd);
    if (!cd)
        return (iconv_t)-1;
    cd->to = enc_by_name(to, &cd->translit, &cd->ignore);
    cd->from = enc_by_name(from, &cd->translit, &cd->ignore);
    if (cd->to < 0 || cd->from < 0)
    {
        free(cd);
        errno = EINVAL;
        return (iconv_t)-1;
    }
    return cd;
}

int iconv_close(iconv_t cd)
{
    free(cd);
    return 0;
}

/* One character from p: its code point, or -1 (invalid), -2 (the input ends inside a character). *used = bytes taken. */
static long decode_one(int enc, const unsigned char *p, usize left, usize *used)
{
    *used = 1;
    if (enc == ENC_LATIN1)
        return p[0];
    if (enc == ENC_ASCII)
        return p[0] < 0x80 ? p[0] : -1;
    if (enc == ENC_CP1252)
        return p[0] >= 0x80 && p[0] < 0xA0 ? cp1252_hi[p[0] - 0x80] : p[0];
    if (p[0] < 0x80)
        return p[0];
    int n = p[0] >= 0xF0 && p[0] < 0xF5 ? 4 : p[0] >= 0xE0 && p[0] < 0xF0 ? 3 : p[0] >= 0xC2 && p[0] < 0xE0 ? 2 : 0;
    if (n == 0)
        return -1;
    if (left < (usize)n)
        return -2;
    long cp = p[0] & (0xFF >> (n + 1));
    for (int i = 1; i < n; i++)
    {
        if ((p[i] & 0xC0) != 0x80)
            return -1;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp < 0xE000))
        return -1;
    *used = (usize)n;
    return cp;
}

/* Writes the character in enc; its length, or 0 when it cannot be represented. */
static int encode_one(int enc, long cp, unsigned char *o)
{
    if (enc == ENC_ASCII)
    {
        if (cp >= 0x80)
            return 0;
        o[0] = (unsigned char)cp;
        return 1;
    }
    if (enc == ENC_LATIN1)
    {
        if (cp >= 0x100)
            return 0;
        o[0] = (unsigned char)cp;
        return 1;
    }
    if (enc == ENC_CP1252)
    {
        if (cp < 0x80 || (cp >= 0xA0 && cp < 0x100))
        {
            o[0] = (unsigned char)cp;
            return 1;
        }
        for (int i = 0; i < 32; i++)
            if (cp == cp1252_hi[i] && cp != 0xFFFD)
            {
                o[0] = (unsigned char)(0x80 + i);
                return 1;
            }
        return 0;
    }
    if (cp < 0x80)
    {
        o[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        o[0] = (unsigned char)(0xC0 | (cp >> 6));
        o[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        o[0] = (unsigned char)(0xE0 | (cp >> 12));
        o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (unsigned char)(0xF0 | (cp >> 18));
    o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t iconv(iconv_t h, char **in, size_t *inleft, char **out, size_t *outleft)
{
    struct iconv_cd *cd = h;
    size_t irrev = 0;
    if (!in || !*in)
        return 0; /* reset: these encodings have no state */
    while (*inleft > 0)
    {
        usize used;
        long cp = decode_one(cd->from, (const unsigned char *)*in, *inleft, &used);
        if (cp == -2)
        {
            errno = EINVAL;
            return (size_t)-1;
        }
        if (cp < 0)
        {
            if (cd->ignore)
            {
                *in += 1;
                *inleft -= 1;
                continue;
            }
            errno = EILSEQ;
            return (size_t)-1;
        }
        unsigned char tmp[4];
        int n = encode_one(cd->to, cp, tmp);
        if (n == 0)
        {
            if (cd->translit)
            {
                n = encode_one(cd->to, '?', tmp);
                irrev++;
            }
            else if (cd->ignore)
            {
                *in += used;
                *inleft -= used;
                continue;
            }
            else
            {
                errno = EILSEQ;
                return (size_t)-1;
            }
        }
        if ((size_t)n > *outleft)
        {
            errno = E2BIG;
            return (size_t)-1;
        }
        memcpy(*out, tmp, (usize)n);
        *out += n;
        *outleft -= (size_t)n;
        *in += used;
        *inleft -= used;
    }
    return irrev;
}

/* ---- select -------------------------------------------------------------------------------------------------------------------- */
/* No descriptor can be waited for yet, so this reports "nothing ready" and spends the timeout busy-waiting on the millisecond clock. */
int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout)
{
    (void)nfds;
    if (r)
        FD_ZERO(r);
    if (w)
        FD_ZERO(w);
    if (e)
        FD_ZERO(e);
    if (timeout)
    {
        long long end = sys_clock() + timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
        while (sys_clock() < end)
            ;
    }
    return 0;
}
