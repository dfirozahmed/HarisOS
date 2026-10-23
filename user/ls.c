/* HarisOS user program: ls [-l] [FOLDER ...]. Lists a folder (the working folder by default), one entry per line; folders
 * have a trailing '/'. With -l: size and name. With a file as argument it is listed by itself. */
#include "libc.h"

static void show(const DirEntry *e, int longfmt)
{
    if (longfmt)
        printf("%c %10u  %s%s\n", e->is_dir ? 'd' : '-', e->size, e->name, e->is_dir ? "/" : "");
    else
        printf("%s%s\n", e->name, e->is_dir ? "/" : "");
}

static int list(const char *path, int longfmt, int title)
{
    DirEntry e;
    if (sys_stat(path, &e) < 0)
    {
        fdprintf(2, "ls: cannot access '%s'\n", path);
        return 1;
    }
    if (!e.is_dir)
    {
        show(&e, longfmt);
        return 0;
    }
    if (title)
        printf("%s:\n", path);
    for (unsigned i = 0; sys_readdir(path, i, &e) == 1; i++)
        show(&e, longfmt);
    return 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    int longfmt = 0, status = 0, paths = 0;
    for (long long i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-l") == 0)
            longfmt = 1;
        else
            paths++;
    }
    if (paths == 0)
        status = list(".", longfmt, 0);
    for (long long i = 1; i < argc; i++)
        if (strcmp(argv[i], "-l") != 0)
            status |= list(argv[i], longfmt, paths > 1);
    sys_exit(status);
}
