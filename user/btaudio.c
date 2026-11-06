/* HarisOS user program: btaudio AA:BB:CC:DD:EE:FF | btaudio off | btaudio
 * Connects a Bluetooth headset for music and leaves it connected: from then on `play FILE` and the Player send their sound to the headset
 * (the volume keys change a software volume). The link is paired (the headset must be in pairing mode the first time), the music stream is
 * opened but not started; it is started when sound comes and suspended after a few quiet seconds.
 *   btaudio AA:BB:CC:DD:EE:FF   connect (the address from btscan) and stay connected
 *   btaudio off                 close the stream and the link: sound goes to the speakers again
 *   btaudio                     show the state */
#include "libc.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static long long parse_addr(const char *s)
{
    long long v = 0;
    for (int i = 0; i < 6; i++)
    {
        int h = hexval(s[0]), l = h < 0 ? -1 : hexval(s[1]);
        if (h < 0 || l < 0)
            return -1;
        v = (v << 8) | (h << 4) | l;
        s += 2;
        if (i < 5)
        {
            if (*s != ':' && *s != '-')
                return -1;
            s++;
        }
    }
    return *s ? -1 : v;
}

static int starts(const char *s, const char *w)
{
    while (*w && *s == *w)
    {
        s++;
        w++;
    }
    return *w == 0;
}

static void show_state(void)
{
    char line[256];
    if (sys_btlink(0, line, sizeof line) >= 0)
        fdprintf(1, "%s\n", line);
    for (unsigned i = 0xA0; sys_btlink(i, line, sizeof line) >= 0; i++)
        fdprintf(1, "%s\n", line);
    if (sys_btlink(0xB0, line, sizeof line) >= 0)
        fdprintf(1, "%s\n", line);
}

/* waits until line `idx` starts with `ok` or one of the failure words, or `ms` pass; 1 = ok */
static int wait_line(unsigned idx, const char *ok, const char *bad1, const char *bad2, long long ms)
{
    char line[256];
    long long t0 = sys_clock();
    for (;;)
    {
        sys_sleep(250);
        if (sys_btlink(idx, line, sizeof line) < 0)
            return 0;
        if (starts(line, ok))
            return 1;
        if (starts(line, bad1) || starts(line, bad2))
            return 0;
        if (sys_clock() - t0 > ms)
            return 0;
    }
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    char line[256];
    if (argc < 2)
    {
        show_state();
        sys_exit(0);
    }
    if (starts(argv[1], "off"))
    {
        syscall3(SYS_BTLINK, SYS_BTLINK_A2DP_STOP, 0, 0);
        for (int i = 0; i < 40; i++)
        {
            sys_sleep(250);
            if (sys_btlink(0xA0, line, sizeof line) < 0 || starts(line, "a2dp: closed") || starts(line, "a2dp: failed") || starts(line, "a2dp: -"))
                break;
        }
        syscall3(SYS_BTLINK, SYS_BTLINK_DISCONNECT, 0, 0);
        sys_sleep(1000);
        show_state();
        sys_exit(0);
    }
    long long addr = parse_addr(argv[1]);
    if (addr < 0)
    {
        fdprintf(2, "usage: btaudio AA:BB:CC:DD:EE:FF | btaudio off | btaudio   (the address from btscan)\n");
        sys_exit(1);
    }
    long long r = syscall3(SYS_BTLINK, SYS_BTLINK_CONNECT, addr, 0);
    if (r != 0)
    {
        fdprintf(2, r == -100 ? "btaudio: no working Bluetooth chip\n" : r == -101 ? "btaudio: a scan is running, try again when btscan is done\n" : r == -102 ? "btaudio: a link exists already (btaudio off ends it)\n" : "btaudio: the chip refused (%d)\n", (int)r);
        sys_exit(1);
    }
    fdprintf(1, "connecting (up to 25 seconds) ...\n");
    int ok = 0;
    long long t0 = sys_clock();
    while (sys_clock() - t0 < 40000)
    {
        sys_sleep(250);
        if (sys_btlink(0, line, sizeof line) < 0 || starts(line, "failed") || starts(line, "closed") || starts(line, "idle"))
            break;
        if (starts(line, "connected") && sys_btlink(1, line, sizeof line) >= 0 && !starts(line, "echo: waiting") && !starts(line, "echo: -"))
        {
            ok = 1;
            break;
        }
    }
    if (ok)
    {
        long long pr = syscall3(SYS_BTLINK, SYS_BTLINK_PAIR, 0, 0);
        if (pr == 0)
        {
            fdprintf(1, "pairing (up to 30 seconds) ...\n");
            ok = wait_line(0x80, "pair: done", "pair: failed", "pair: failed", 40000);
            if (!ok)
                fdprintf(2, "btaudio: the pairing did not work\n");
        }
        else
        {
            fdprintf(2, "btaudio: could not start the pairing (%d)\n", (int)pr);
            ok = 0;
        }
    }
    if (ok)
    {
        long long ar = syscall3(SYS_BTLINK, SYS_BTLINK_A2DP, 1, 0); /* 1 = keep: open, but start only when sound comes */
        if (ar == 0)
        {
            fdprintf(1, "preparing the music stream (up to 40 seconds) ...\n");
            ok = wait_line(0xA0, "a2dp: ready", "a2dp: failed", "a2dp: closed", 40000);
        }
        else
        {
            fdprintf(2, "btaudio: could not start the stream (%d)\n", (int)ar);
            ok = 0;
        }
    }
    show_state();
    if (!ok)
    {
        syscall3(SYS_BTLINK, SYS_BTLINK_A2DP_STOP, 0, 0);
        syscall3(SYS_BTLINK, SYS_BTLINK_DISCONNECT, 0, 0);
        for (unsigned i = 0x90; sys_btlink(i, line, sizeof line) >= 0; i++)
            fdprintf(1, "%s\n", line);
        if (sys_btlink(0xC0, line, sizeof line) >= 0)
            fdprintf(1, "%s\n", line);
        sys_exit(1);
    }
    fdprintf(1, "Bluetooth audio is on: play a sound with `play FILE` (turn the headset's volume down first). `btaudio off` ends it.\n");
    sys_exit(0);
}
