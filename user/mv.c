/* HarisOS user program: mv SOURCE DEST. Renames a file or folder, or moves it into DEST if that is a folder. Between two folders
 * of one drive the entry is moved; a file that must also change drive (or change name and folder at once) is copied and then
 * deleted. A folder cannot do that. */
#include "fsutil.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    if (argc != 3)
    {
        fdprintf(2, "usage: mv SOURCE DEST\n");
        sys_exit(2);
    }
    const char *src = argv[1], *dst = argv[2];
    char base[96], target[FS_PATH_MAX];
    if (!fs_exists(src))
    {
        fdprintf(2, "mv: no such file or folder '%s'\n", src);
        sys_exit(1);
    }
    if (fs_is_dir(dst))
    {
        fs_basename(src, base, sizeof(base));
        if (fs_join(target, sizeof(target), dst, base) < 0)
        {
            fdprintf(2, "mv: path too long\n");
            sys_exit(1);
        }
        dst = target;
    }
    if (fs_exists(dst))
    {
        fdprintf(2, "mv: '%s' already exists (remove it first)\n", dst); /* never overwrite: a move that fails half way would lose it */
        sys_exit(1);
    }
    if (sys_rename(src, dst) == 0)
        sys_exit(0);
    if (fs_is_dir(src))
    {
        fdprintf(2, "mv: cannot move the folder '%s' to '%s' (it would change drive, or name and folder at once)\n", src, dst);
        sys_exit(1);
    }
    if (fs_copy_file(src, dst) < 0 || sys_unlink(src) < 0)
    {
        fdprintf(2, "mv: could not move '%s' to '%s'\n", src, dst);
        sys_exit(1);
    }
    sys_exit(0);
}
