/* HarisOS user program: ps. Lists the running processes: pid, the parent's pid, R (ready or running) or S (waiting for input, a
 * pipe or a child), the window (console) it writes to, and its name. */
#include "libc.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    puts("  PID  PPID S CON NAME");
    PsInfo p;
    for (unsigned i = 0; sys_psinfo(i, &p) == 1; i++)
        printf("%5d %5d %c %3d %s\n", p.pid, p.ppid, p.running ? 'R' : 'S', p.console, p.name);
    sys_exit(0);
}
