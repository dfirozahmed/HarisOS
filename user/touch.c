/* HarisOS user program: touch FILE ... Makes empty files that do not exist yet (an existing file is left as it is). */
#include "fsutil.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    int status = 0;
    if (argc < 2)
    {
        fdprintf(2, "usage: touch FILE ...\n");
        sys_exit(2);
    }
    for (long long i = 1; i < argc; i++)
    {
        long long fd = sys_open_flags(argv[i], SYS_O_WRITE | SYS_O_CREATE);
        if (fd < 0)
        {
            fdprintf(2, "touch: cannot create '%s'\n", argv[i]);
            status = 1;
            continue;
        }
        sys_close((int)fd);
    }
    sys_exit(status);
}
