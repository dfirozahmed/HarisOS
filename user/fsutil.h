/* Small helpers for the file programs (cp, mv, rm, mkdir, touch). Included by them; needs libc.h. */
#ifndef HARIS_USER_FSUTIL_H
#define HARIS_USER_FSUTIL_H

#include "libc.h"

#define FS_PATH_MAX 256

static inline int fs_is_dir(const char *path)
{
    DirEntry e;
    return sys_stat(path, &e) == 0 && e.is_dir;
}

static inline int fs_exists(const char *path)
{
    DirEntry e;
    return sys_stat(path, &e) == 0;
}

/* the last component of a path, ignoring slashes at the end ("a/b/" -> "b"); points into path or into out */
static inline const char *fs_basename(const char *path, char *out, usize cap)
{
    usize len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;
    usize start = len;
    while (start > 0 && path[start - 1] != '/')
        start--;
    usize n = len - start;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, path + start, n);
    out[n] = 0;
    return out;
}

/* dir + "/" + name into out; 0, or -1 if it does not fit */
static inline int fs_join(char *out, usize cap, const char *dir, const char *name)
{
    usize len = strlen(dir);
    int n = snprintf(out, cap, len > 0 && dir[len - 1] == '/' ? "%s%s" : "%s/%s", dir, name);
    return n < 0 || (usize)n >= cap ? -1 : 0;
}

/* Copies the bytes of file src into file dst (made, or emptied first). 0, or -1 with the reason printed by the caller. */
static inline int fs_copy_file(const char *src, const char *dst)
{
    long long in = sys_open(src);
    if (in < 0)
        return -1;
    long long out = sys_open_flags(dst, SYS_O_WRITE | SYS_O_CREATE | SYS_O_TRUNC);
    if (out < 0)
    {
        sys_close((int)in);
        return -1;
    }
    char buf[2048];
    int status = 0;
    for (;;)
    {
        long long got = sys_read((int)in, buf, sizeof(buf));
        if (got < 0)
            status = -1;
        if (got <= 0)
            break;
        long long put = sys_write((int)out, buf, (usize)got);
        if (put != got)
        {
            status = -1;
            break;
        }
    }
    sys_close((int)in);
    sys_close((int)out);
    return status;
}

#endif
