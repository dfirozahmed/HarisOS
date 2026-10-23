/* HarisOS user program: kill [-INT | -TERM | -KILL | -2 | -15 | -9] PID ... Ends processes (default: TERM). "ps" shows the pids. */
#include "libc.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    int sig = SIGTERM, status = 0, pids = 0;
    for (long long i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (a[0] == '-' && a[1])
        {
            if (strcmp(a, "-INT") == 0 || strcmp(a, "-2") == 0)
                sig = SIGINT;
            else if (strcmp(a, "-TERM") == 0 || strcmp(a, "-15") == 0)
                sig = SIGTERM;
            else if (strcmp(a, "-KILL") == 0 || strcmp(a, "-9") == 0)
                sig = SIGKILL;
            else
            {
                fdprintf(2, "kill: unknown signal '%s' (INT, TERM, KILL)\n", a);
                sys_exit(2);
            }
            continue;
        }
        pids++;
        char *end;
        long long pid = strtoll(a, &end, 10);
        if (end == a || *end || sys_kill(pid, sig) < 0)
        {
            fdprintf(2, "kill: no such process '%s'\n", a);
            status = 1;
        }
    }
    if (pids == 0)
    {
        fdprintf(2, "usage: kill [-INT|-TERM|-KILL] PID ...\n");
        status = 2;
    }
    sys_exit(status);
}
