/* HarisOS user program: lock [-u] [PATH]. Drives start locked: nothing can be written to them (a mistake in a file system driver
 * cannot damage a disk nobody has unlocked). "lock" lists the drives and their state; "lock -u PATH" unlocks the drive that PATH
 * is on ("lock -u ." = the drive you are in); "lock PATH" locks it again. */
#include "libc.h"

static void show(const char *path, const char *name)
{
    long long st = sys_lock(path, 2);
    printf("%-30s %s\n", name, st < 0 ? "(not a drive)" : st ? "locked" : "unlocked: writing is allowed");
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    int unlock = 0;
    const char *path = 0;
    for (long long i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-u") == 0)
            unlock = 1;
        else
            path = argv[i];
    }
    if (!path)
    {
        if (unlock)
        {
            fdprintf(2, "usage: lock [-u] [PATH]\n");
            sys_exit(2);
        }
        DirEntry e;
        for (unsigned i = 0; sys_readdir("/", i, &e) == 1; i++)
        {
            char p[128];
            snprintf(p, sizeof(p), "/%s", e.name);
            show(p, e.name);
        }
        sys_exit(0);
    }
    if (sys_lock(path, unlock ? 0 : 1) < 0)
    {
        fdprintf(2, "lock: cannot %s '%s' (not on a drive, or the drive cannot be written)\n", unlock ? "unlock" : "lock", path);
        sys_exit(1);
    }
    show(path, path);
    sys_exit(0);
}
