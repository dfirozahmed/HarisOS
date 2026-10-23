/* HarisOS user program: cat FILE ... Prints files to the console. With no argument it prints what is typed, line by line,
 * until the window is closed. */
#include "libc.h"

static int cat_fd(int fd)
{
    char buf[512];
    long long got;
    while ((got = sys_read(fd, buf, sizeof(buf))) > 0)
        sys_write(1, buf, (usize)got);
    return got < 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    int status = 0;
    if (argc < 2)
        sys_exit(cat_fd(0));
    for (long long i = 1; i < argc; i++)
    {
        long long fd = sys_open(argv[i]);
        if (fd < 0)
        {
            fdprintf(2, "cat: cannot open '%s'\n", argv[i]);
            status = 1;
            continue;
        }
        status |= cat_fd((int)fd);
        sys_close((int)fd);
    }
    sys_exit(status);
}
