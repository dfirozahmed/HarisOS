/* HarisOS user program: wifiscan
 * Prints the result of the Wi-Fi scan that the RTL8821CE driver made when the system started: the networks around (channel, security,
 * BSSID, name) and a few counters. Nothing here talks to the radio; it reads the kernel's list with SYS_WIFISCAN. */
#include "libc.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    (void)argc;
    (void)argv;
    char line[256];
    long long n = sys_wifiscan(0, line, sizeof line);
    if (n < 0)
    {
        fdprintf(2, "wifiscan: no scan result (no Wi-Fi adapter, or it did not start)\n");
        sys_exit(1);
    }
    fdprintf(1, "%s\n", line);
    for (unsigned i = 1; (n = sys_wifiscan(i, line, sizeof line)) >= 0; i++)
        fdprintf(1, "%s\n", line);
    sys_exit(0);
}
