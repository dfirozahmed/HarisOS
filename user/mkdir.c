/* HarisOS user program: mkdir [-p] FOLDER ... Makes folders. With -p the folders above are made as needed and a folder that
 * already exists is not an error. */
#include "fsutil.h"

static int make(const char *path, int parents)
{
    if (parents)
    {
        char part[FS_PATH_MAX];
        usize n = strlen(path);
        if (n >= sizeof(part))
            return -1;
        for (usize i = 1; i <= n; i++)
            if (path[i] == '/' || path[i] == 0)
            {
                memcpy(part, path, i);
                part[i] = 0;
                if (!fs_exists(part) && sys_mkdir(part) < 0)
                    return -1;
            }
        return fs_is_dir(path) ? 0 : -1;
    }
    return sys_mkdir(path) < 0 ? -1 : 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    int parents = 0, status = 0, names = 0;
    for (long long i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-p") == 0)
        {
            parents = 1;
            continue;
        }
        names++;
        if (make(argv[i], parents) < 0)
        {
            fdprintf(2, "mkdir: cannot create '%s'\n", argv[i]);
            status = 1;
        }
    }
    if (names == 0)
    {
        fdprintf(2, "usage: mkdir [-p] FOLDER ...\n");
        status = 2;
    }
    sys_exit(status);
}
