/* HarisOS user program: btlink AA:BB:CC:DD:EE:FF [pair|a2dp|tone]
 * Connects to a Bluetooth device (a headset, a phone ...: it must be switched on, in range and ready to be connected, which is often the same
 * "pairing" mode that btscan needs), tests that data flows both ways on the L2CAP signalling channel, asks it for its list of services (SDP:
 * does it play stereo music (A2DP), is it only for calls ...), prints what the device told, and ends the link again. Take the address from
 * btscan. With the word "pair" it also pairs with the device first (the device must be in pairing mode: for a headset usually a blinking
 * light after holding the button): secure simple pairing, or the old PIN 0000. The link key is kept in memory only, until the next restart.
 * With "a2dp" it pairs, lists the services and then prepares a music stream with the device (AVDTP: finds its audio endpoint, chooses an SBC
 * setting, opens the stream and the media channel, starts it), holds it for two seconds and closes it.
 * With "tone" it does the same and plays a test tune of three notes for six seconds on the open stream (SBC audio in RTP packets, made in the
 * kernel): this is the first sound over Bluetooth. Turn the headset's volume down a little first: the tune is a quarter of full scale. */
#include "libc.h"

#define WAIT_MS 40000

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

/* "d3:04:df:0c:47:41" -> 0xd304df0c4741 (first byte highest). -1 if it is not that. */
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

static void show(void)
{
    char line[256];
    for (unsigned i = 0; sys_btlink(i, line, sizeof line) >= 0; i++)
        fdprintf(1, "%s\n", line);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    if (argc < 2 || parse_addr(argv[1]) < 0 || (argc >= 3 && !starts(argv[2], "pair") && !starts(argv[2], "a2dp") && !starts(argv[2], "tone")))
    {
        fdprintf(2, "usage: btlink AA:BB:CC:DD:EE:FF [pair|a2dp|tone]   (the address from btscan)\n");
        sys_exit(1);
    }
    int want_tone = argc >= 3 && starts(argv[2], "tone");
    int want_a2dp = argc >= 3 && (starts(argv[2], "a2dp") || want_tone);
    int want_pair = argc >= 3; /* a music stream needs the link paired and encrypted */
    long long r = syscall3(SYS_BTLINK, SYS_BTLINK_CONNECT, parse_addr(argv[1]), 0);
    if (r == -100)
    {
        fdprintf(2, "btlink: no working Bluetooth chip\n");
        sys_exit(1);
    }
    if (r == -101)
    {
        fdprintf(2, "btlink: a scan is running, try again when btscan is done\n");
        sys_exit(1);
    }
    if (r == -102)
    {
        fdprintf(2, "btlink: a link exists already\n");
        sys_exit(1);
    }
    if (r != 0)
    {
        fdprintf(2, "btlink: the chip refused (%d)\n", (int)r);
        sys_exit(1);
    }
    fdprintf(1, "connecting (up to 25 seconds) ...\n");
    char line[256];
    long long start = sys_clock();
    int ok = 0;
    for (;;)
    {
        sys_sleep(250);
        if (sys_btlink(0, line, sizeof line) < 0)
            break;
        if (starts(line, "failed") || starts(line, "closed") || starts(line, "idle"))
            break;
        if (starts(line, "connected"))
        {
            long long n = sys_btlink(1, line, sizeof line);
            if (n >= 0 && !starts(line, "echo: waiting") && !starts(line, "echo: -")) /* the probe has an answer or gave up */
            {
                ok = 1;
                break;
            }
        }
        if (sys_clock() - start > WAIT_MS)
        {
            fdprintf(2, "btlink: gave up waiting\n");
            break;
        }
    }
    if (ok && want_pair) /* pairing: authenticate and encrypt the link; lines 0x80 and 0x81 say how it went */
    {
        long long pr = syscall3(SYS_BTLINK, SYS_BTLINK_PAIR, 0, 0);
        if (pr == 0)
        {
            fdprintf(1, "pairing (up to 30 seconds) ...\n");
            long long t0 = sys_clock();
            for (;;)
            {
                sys_sleep(250);
                if (sys_btlink(0x80, line, sizeof line) < 0 || starts(line, "pair: done") || starts(line, "pair: failed"))
                    break;
                if (sys_clock() - t0 > 40000)
                {
                    fdprintf(2, "btlink: gave up waiting for the pairing\n");
                    break;
                }
            }
            for (unsigned i = 0x80; sys_btlink(i, line, sizeof line) >= 0; i++)
                fdprintf(1, "%s\n", line);
        }
        else
            fdprintf(2, "btlink: could not start the pairing (%d)\n", (int)pr);
    }
    if (ok) /* the link is up: the list of services. It runs in the kernel; line 4 tells when it is over. */
    {
        long long sr = syscall3(SYS_BTLINK, SYS_BTLINK_SDP, 0, 0);
        if (sr == 0)
        {
            fdprintf(1, "asking for the services ...\n");
            long long t0 = sys_clock();
            for (;;)
            {
                sys_sleep(250);
                if (sys_btlink(4, line, sizeof line) < 0 || starts(line, "sdp: failed") || (line[5] >= '0' && line[5] <= '9'))
                    break; /* "sdp: N services" or a failure */
                if (sys_clock() - t0 > 20000)
                {
                    fdprintf(2, "btlink: gave up waiting for the services\n");
                    break;
                }
            }
        }
        else
            fdprintf(2, "btlink: could not ask for the services (%d)\n", (int)sr);
    }
    if (ok && want_a2dp) /* the music stream: lines 0xA0 .. 0xA2 */
    {
        long long ar = syscall3(SYS_BTLINK, SYS_BTLINK_A2DP, 0, 0);
        if (ar == 0)
        {
            fdprintf(1, "preparing the music stream (up to 40 seconds) ...\n");
            long long t0 = sys_clock();
            for (;;)
            {
                sys_sleep(250);
                if (sys_btlink(0xA0, line, sizeof line) < 0 || starts(line, "a2dp: streaming") || starts(line, "a2dp: failed") || starts(line, "a2dp: closed"))
                    break;
                if (sys_clock() - t0 > 40000)
                {
                    fdprintf(2, "btlink: gave up waiting for the stream\n");
                    break;
                }
            }
            for (unsigned i = 0xA0; sys_btlink(i, line, sizeof line) >= 0; i++)
                fdprintf(1, "%s\n", line);
            if (sys_btlink(0xA0, line, sizeof line) >= 0 && starts(line, "a2dp: streaming"))
            {
                if (want_tone)
                {
                    long long tr = syscall3(SYS_BTLINK, SYS_BTLINK_TONE, 6, 0);
                    if (tr == 0)
                    {
                        fdprintf(1, "playing the test tune for 6 seconds ...\n");
                        long long t1 = sys_clock();
                        for (;;)
                        {
                            sys_sleep(250);
                            if (sys_btlink(0xB0, line, sizeof line) < 0 || !starts(line, "audio: playing") || sys_clock() - t1 > 15000)
                                break;
                        }
                        sys_sleep(300); /* the sink plays what it has buffered */
                        if (sys_btlink(0xB0, line, sizeof line) >= 0)
                            fdprintf(1, "%s\n", line);
                    }
                    else
                        fdprintf(2, "btlink: could not play the tune (%d)\n", (int)tr);
                }
                else
                    sys_sleep(2000); /* hold the stream open a moment */
            }
            syscall3(SYS_BTLINK, SYS_BTLINK_A2DP_STOP, 0, 0);
            for (int i = 0; i < 40; i++)
            {
                sys_sleep(250);
                if (sys_btlink(0xA0, line, sizeof line) < 0 || starts(line, "a2dp: closed") || starts(line, "a2dp: failed"))
                    break;
            }
            if (sys_btlink(0xA0, line, sizeof line) >= 0)
                fdprintf(1, "%s\n", line);
        }
        else
            fdprintf(2, "btlink: could not start the stream (%d)\n", (int)ar);
    }
    show();
    syscall3(SYS_BTLINK, SYS_BTLINK_DISCONNECT, 0, 0);
    for (int i = 0; i < 40; i++) /* wait up to 10 s for the link to end */
    {
        sys_sleep(250);
        if (sys_btlink(0, line, sizeof line) >= 0 && !starts(line, "connected") && !starts(line, "closing") && !starts(line, "connecting"))
            break;
    }
    if (sys_btlink(0, line, sizeof line) >= 0)
        fdprintf(1, "%s\n", line);
    for (unsigned i = 0x90; sys_btlink(i, line, sizeof line) >= 0; i++) /* diagnostics: what was said on the signalling channel */
        fdprintf(1, "%s\n", line);
    if (sys_btlink(0xC0, line, sizeof line) >= 0) /* ... and the chip with its USB link */
        fdprintf(1, "%s\n", line);
    sys_exit(ok ? 0 : 1);
}
