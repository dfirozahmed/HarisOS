/* HarisOS user program: btscan
 * Looks for Bluetooth devices around (phones, headphones, keyboards ... that are in "discoverable" / pairing mode) and prints them: address,
 * signal strength, kind and name. The scan runs in the kernel (SYS_BTSCAN) and takes about 10 seconds plus the time to ask for names. */
#include "libc.h"

#define WAIT_MS 60000

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    (void)argc;
    (void)argv;
    char line[256];
    long long r = sys_btscan(SYS_BTSCAN_START, line, sizeof line);
    if (r == -100)
    {
        fdprintf(2, "btscan: no working Bluetooth chip\n");
        sys_exit(1);
    }
    if (r == -101)
        fdprintf(1, "btscan: a scan is already running, waiting for it\n");
    else if (r != 0)
    {
        fdprintf(2, "btscan: the chip refused the scan (%d)\n", (int)r);
        sys_exit(1);
    }
    fdprintf(1, "scanning for Bluetooth devices (about 10 seconds) ...\n");
    long long start = sys_clock();
    for (;;)
    {
        long long n = sys_btscan(0, line, sizeof line);
        if (n < 0 || (line[0] == 'd' && line[1] == 'o' && line[2] == 'n' && line[3] == 'e'))
            break;
        if (sys_clock() - start > WAIT_MS)
        {
            fdprintf(2, "btscan: gave up waiting\n");
            break;
        }
        sys_sleep(250);
    }
    long long n = sys_btscan(0, line, sizeof line);
    if (n < 0)
        sys_exit(1);
    fdprintf(1, "%s\n", line);
    for (unsigned i = 1; (n = sys_btscan(i, line, sizeof line)) >= 0; i++)
        fdprintf(1, "%s\n", line);
    sys_exit(0);
}
