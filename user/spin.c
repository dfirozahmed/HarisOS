/* HarisOS user program: a process that does nothing useful and never ends by itself, for trying signals and for tests.
 *   spin          computes forever (a process the timer has to take the CPU from)
 *   spin read     waits for a line from the keyboard that nobody types
 *   spin pipe     waits on a pipe that nobody writes to
 *   spin int      ignores SIGINT (Ctrl+C), then computes
 *   spin all      ignores SIGINT and SIGTERM (only "kill -KILL" ends it), then computes */
#include "libc.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    char buf[8];
    if (strcmp(mode, "read") == 0)
        sys_read(0, buf, sizeof(buf));
    else if (strcmp(mode, "pipe") == 0)
    {
        int fds[2];
        if (sys_pipe(fds) == 0)
            sys_read(fds[0], buf, sizeof(buf)); /* we hold the write end ourselves: this never returns */
    }
    else if (strcmp(mode, "int") == 0)
        sys_sigignore(1u << SIGINT);
    else if (strcmp(mode, "all") == 0)
        sys_sigignore((1u << SIGINT) | (1u << SIGTERM));
    volatile unsigned long long n = 0;
    for (;;)
        n++;
}
