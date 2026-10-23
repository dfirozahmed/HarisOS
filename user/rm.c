/* HarisOS user program: rm [-r] [-f] PATH ... Deletes files; with -r also folders and everything in them. With -f a path that
 * does not exist is not an error. (A drive's root and the folder you are in cannot be deleted.) */
#include "fsutil.h"

#define MAX_DEPTH 8

static int remove_path(const char *path, int recursive, int depth)
{
    if (!fs_is_dir(path))
        return sys_unlink(path) < 0 ? -1 : 0;
    if (!recursive || depth >= MAX_DEPTH)
        return -1;
    /* Empty the folder: the first entry is taken until none is left (deleting one renumbers the rest). An entry that
     * cannot be deleted ends the loop. */
    for (;;)
    {
        DirEntry e;
        if (sys_readdir(path, 0, &e) != 1)
            break;
        char child[FS_PATH_MAX];
        if (fs_join(child, sizeof(child), path, e.name) < 0 || remove_path(child, recursive, depth + 1) < 0)
            return -1;
    }
    return sys_rmdir(path) < 0 ? -1 : 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    int recursive = 0, force = 0, status = 0, names = 0;
    for (long long i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-r") == 0)
            recursive = 1;
        else if (strcmp(argv[i], "-f") == 0)
            force = 1;
        else if (strcmp(argv[i], "-rf") == 0 || strcmp(argv[i], "-fr") == 0)
            recursive = force = 1;
    }
    for (long long i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-' && argv[i][1])
            continue;
        names++;
        if (!fs_exists(argv[i]))
        {
            if (!force)
            {
                fdprintf(2, "rm: no such file or folder '%s'\n", argv[i]);
                status = 1;
            }
        }
        else if (remove_path(argv[i], recursive, 0) < 0)
        {
            fdprintf(2, "rm: cannot remove '%s'%s\n", argv[i], fs_is_dir(argv[i]) && !recursive ? " (a folder: use -r)" : "");
            status = 1;
        }
    }
    if (names == 0)
    {
        fdprintf(2, "usage: rm [-r] [-f] PATH ...\n");
        status = 2;
    }
    sys_exit(status);
}
