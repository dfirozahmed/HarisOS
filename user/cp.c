/* HarisOS user program: cp [-r] SOURCE DEST. Copies a file to DEST, or into DEST if that is a folder. With -r a folder is copied
 * with everything in it. The copy of a file is made of the bytes the drive gives (RAM Disk files have none). */
#include "fsutil.h"

#define MAX_DEPTH 8

static int copy_path(const char *src, const char *dst, int recursive, int depth)
{
    if (!fs_is_dir(src))
        return fs_copy_file(src, dst);
    if (!recursive || depth >= MAX_DEPTH)
        return -1;
    if (!fs_is_dir(dst) && sys_mkdir(dst) < 0)
        return -1;
    int status = 0;
    for (unsigned i = 0;; i++)
    {
        DirEntry e;
        if (sys_readdir(src, i, &e) != 1)
            break;
        char from[FS_PATH_MAX], to[FS_PATH_MAX];
        if (fs_join(from, sizeof(from), src, e.name) < 0 || fs_join(to, sizeof(to), dst, e.name) < 0 ||
            copy_path(from, to, recursive, depth + 1) < 0)
            status = -1; /* go on with the others */
    }
    return status;
}

/* is `inner` the same as `outer` or below it? (a textual check of the paths as typed: copying a folder into itself would
 * never end) */
static int inside(const char *inner, const char *outer)
{
    usize n = strlen(outer);
    while (n > 1 && outer[n - 1] == '/')
        n--;
    return strncmp(inner, outer, n) == 0 && (inner[n] == 0 || inner[n] == '/');
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    int recursive = 0;
    const char *names[2];
    int n = 0;
    for (long long i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-r") == 0)
            recursive = 1;
        else if (n < 2)
            names[n++] = argv[i];
        else
            n = 3;
    }
    if (n != 2)
    {
        fdprintf(2, "usage: cp [-r] SOURCE DEST\n");
        sys_exit(2);
    }
    const char *src = names[0], *dst = names[1];
    char base[96], target[FS_PATH_MAX];
    if (fs_is_dir(dst)) /* into the folder, under the source's name */
    {
        fs_basename(src, base, sizeof(base));
        if (fs_join(target, sizeof(target), dst, base) < 0)
        {
            fdprintf(2, "cp: path too long\n");
            sys_exit(1);
        }
        dst = target;
    }
    if (!fs_exists(src))
    {
        fdprintf(2, "cp: no such file or folder '%s'\n", src);
        sys_exit(1);
    }
    if (fs_is_dir(src) && !recursive)
    {
        fdprintf(2, "cp: '%s' is a folder (use -r)\n", src);
        sys_exit(1);
    }
    if (fs_is_dir(src) && inside(dst, src))
    {
        fdprintf(2, "cp: cannot copy a folder into itself\n");
        sys_exit(1);
    }
    if (copy_path(src, dst, recursive, 0) < 0)
    {
        fdprintf(2, "cp: could not copy '%s' to '%s' (the drive may be read only or full)\n", src, dst);
        sys_exit(1);
    }
    sys_exit(0);
}
