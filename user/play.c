/* HarisOS user program: play FILE.WAV. Reads the whole file and asks the kernel to play it (SYS_AUDIO_PLAY); returns when it has finished. */
#include "libc.h"

#define MAX_WAV (8 << 20) /* SYS_AUDIO_MAX */

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    if (argc < 2)
    {
        fdprintf(2, "usage: play FILE.WAV\n");
        sys_exit(2);
    }
    long long fd = sys_open(argv[1]);
    if (fd < 0)
    {
        fdprintf(2, "play: cannot open '%s'\n", argv[1]);
        sys_exit(1);
    }
    long long size = sys_seek((int)fd, 0, 2);
    sys_seek((int)fd, 0, 0);
    if (size <= 0 || size > MAX_WAV)
    {
        fdprintf(2, "play: '%s' is empty or larger than %d bytes\n", argv[1], MAX_WAV);
        sys_exit(1);
    }
    unsigned char *buf = malloc((usize)size);
    if (!buf)
    {
        fdprintf(2, "play: out of memory\n");
        sys_exit(1);
    }
    long long have = 0, got;
    while (have < size && (got = sys_read((int)fd, buf + have, (usize)(size - have))) > 0)
        have += got;
    sys_close((int)fd);
    if (have != size)
    {
        fdprintf(2, "play: read error\n");
        sys_exit(1);
    }
    long long r = sys_audio_play(buf, (usize)size);
    if (r != 0)
    {
        const char *why = r == -101 ? "not a WAV file" : r == -102 ? "not PCM sound" : r == -103 ? "format not supported (PCM 8/16 bit, mono/stereo, 8-96 kHz)"
                        : r == -104 ? "no samples" : r == -110 ? "no sound hardware" : r == -111 ? "the sound device is busy" : r == -112 ? "too long" : "bad request";
        fdprintf(2, "play: %s\n", why);
        sys_exit(1);
    }
    sys_exit(0);
}
