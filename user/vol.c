/* HarisOS user program: vol [+N | -N | N]. No argument: shows the master volume. "+5" / "-5": louder / quieter by that many percent points
 * (a bare "+" or "-" is 5). "70": sets it to 70 percent. */
#include "libc.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    long long now = sys_audio_volume(-1);
    if (now < 0)
    {
        fdprintf(2, "vol: no sound hardware\n");
        sys_exit(1);
    }
    if (argc >= 2)
    {
        const char *a = argv[1];
        long long target;
        if (a[0] == '+' || a[0] == '-')
        {
            long long step = a[1] ? atoi(a + 1) : 5;
            target = a[0] == '+' ? now + step : now - step;
        }
        else if (a[0] >= '0' && a[0] <= '9')
            target = atoi(a);
        else
        {
            fdprintf(2, "usage: vol [+N | -N | N]\n");
            sys_exit(2);
        }
        now = sys_audio_volume(target < 0 ? 0 : target > 100 ? 100 : target);
    }
    fdprintf(1, "volume %d%%\n", (int)now);
    sys_exit(0);
}
