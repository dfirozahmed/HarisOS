/* HarisOS user program: btscan [off|sw|pta]
 * Looks for Bluetooth devices around (phones, headphones, keyboards ... that are in "discoverable" / pairing mode) and prints them: address,
 * signal strength, kind and name. The scan runs in the kernel (SYS_BTSCAN) and takes about 10 seconds plus the time to ask for names.
 * The optional word says how the Bluetooth chip shares the antenna with the Wi-Fi card (for trying out): off = Bluetooth gets no radio,
 * sw = it may always use it (the default), pta = hardware arbitration. */
#include "libc.h"

#define WAIT_MS 60000

static int is_word(const char *a, const char *w)
{
    while (*a && *a == *w)
    {
        a++;
        w++;
    }
    return *a == 0 && *w == 0;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    long long mode = 0;
    if (argc >= 2)
    {
        if (is_word(argv[1], "off"))
            mode = 1;
        else if (is_word(argv[1], "sw"))
            mode = 2;
        else if (is_word(argv[1], "pta"))
            mode = 3;
        else
        {
            fdprintf(2, "usage: btscan [off|sw|pta]\n");
            sys_exit(1);
        }
    }
    char line[256];
    long long r = syscall3(SYS_BTSCAN, SYS_BTSCAN_START, mode, 0);
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
