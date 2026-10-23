/* HarisOS init: the first program the kernel starts (at the end of boot, in a Terminal window: terminal_open("INIT.ELF")).
 *
 * It runs the shell and keeps it running: when the user types "exit" the shell is started again; when the shell crashes it is
 * started again too, unless it crashed five times in a row (then something is wrong with it, and init says so and ends instead of
 * spinning). When the window is closed the shell ends with code 130 (end of input) and init ends with it. */
#include "libc.h"

#define SH_HANGUP 130 /* the code user/sh.c exits with when its input is gone */
#define MAX_CRASHES 5

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    puts("init: HarisOS is up. Starting the shell ('exit' restarts it).");
    int crashes = 0;
    for (;;)
    {
        long long pid = sys_spawn("SH.ELF", 0);
        if (pid < 0)
        {
            puts("init: cannot start SH.ELF");
            sys_exit(1);
        }
        int status = 0;
        if (sys_wait(pid, &status) < 0)
        {
            puts("init: lost the shell");
            sys_exit(1);
        }
        if (status == SH_HANGUP)
            sys_exit(0);
        if (status == -1)
        {
            if (++crashes >= MAX_CRASHES)
            {
                puts("init: the shell keeps crashing, giving up");
                sys_exit(1);
            }
            puts("init: the shell crashed, starting it again");
        }
        else
        {
            crashes = 0;
            printf("init: the shell ended (%d), starting it again\n", status);
        }
    }
}
