/* HarisOS user program: play FILE.WAV or FILE.MP3.
 *
 * A WAV file is read whole and handed to the kernel (SYS_AUDIO_PLAY), which returns when it has finished playing.
 * An MP3 file is decoded here in user mode (mp3dec.c: Layer III, float math, which the kernel does not use): the file is read through a
 * small window, decoded frame by frame, and the PCM is handed to the kernel in pieces of about 30 seconds, each wrapped in a WAV
 * header because that is what SYS_AUDIO_PLAY takes (it allows 8 MiB per call). There is a short pause between two pieces while the
 * next one is made ready. */
#include "libc.h"
#include "mp3dec.c" /* one program, one translation unit: only play needs the decoder, so it is not part of the user library */

#define MAX_WAV (8 << 20)                  /* SYS_AUDIO_MAX */
#define PIECE_PCM_BYTES (6 << 20)          /* PCM per call: ~34 s of 44.1 kHz stereo; header and rounding stay far below 8 MiB */
#define WINDOW 32768                       /* MP3 bytes in memory at a time: more than the 10 frames the sync check looks at */

static const char *why(long long r)
{
    return r == -101 ? "not a WAV file" : r == -102 ? "not PCM sound" : r == -103 ? "format not supported (PCM 8/16 bit, mono/stereo, 8-96 kHz)"
         : r == -104 ? "no samples" : r == -110 ? "no sound hardware" : r == -111 ? "the sound device is busy" : r == -112 ? "too long" : "bad request";
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, unsigned v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
}

/* A canonical 44 byte WAV header for `bytes` of 16 bit PCM. */
static void wav_header(unsigned char *h, unsigned rate, unsigned channels, unsigned bytes)
{
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1); /* PCM */
    put16(h + 22, channels);
    put32(h + 24, rate);
    put32(h + 28, rate * channels * 2);
    put16(h + 32, channels * 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
}

static void die(const char *msg)
{
    fdprintf(2, "play: %s\n", msg);
    sys_exit(1);
}

static void play_wav(long long fd, const char *name)
{
    long long size = sys_seek((int)fd, 0, 2);
    sys_seek((int)fd, 0, 0);
    if (size <= 0 || size > MAX_WAV)
    {
        fdprintf(2, "play: '%s' is empty or larger than %d bytes\n", name, MAX_WAV);
        sys_exit(1);
    }
    unsigned char *buf = malloc((usize)size);
    if (!buf)
        die("out of memory");
    long long have = 0, got;
    while (have < size && (got = sys_read((int)fd, buf + have, (usize)(size - have))) > 0)
        have += got;
    sys_close((int)fd);
    if (have != size)
        die("read error");
    long long r = sys_audio_play(buf, (usize)size);
    if (r != 0)
        die(why(r));
    sys_exit(0);
}

/* Skip an ID3v2 tag at the start of the file (size is 4 bytes of 7 bits each after "ID3", version, flags; +10 with a footer). */
static void skip_id3(long long fd)
{
    unsigned char h[10];
    sys_seek((int)fd, 0, 0);
    if (sys_read((int)fd, h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3' && !((h[6] | h[7] | h[8] | h[9]) & 0x80))
    {
        long long n = 10 + (((long long)h[6] << 21) | ((long long)h[7] << 14) | ((long long)h[8] << 7) | h[9]) + ((h[5] & 0x10) ? 10 : 0);
        sys_seek((int)fd, n, 0);
    }
    else
        sys_seek((int)fd, 0, 0);
}

static void play_mp3(long long fd, const char *name)
{
    Mp3Dec *dec = malloc(sizeof *dec);
    unsigned char *win = malloc(WINDOW);
    unsigned char *piece = malloc(44 + PIECE_PCM_BYTES + MP3_MAX_SAMPLES * 2); /* header, PCM, room for one more frame */
    short *frame_pcm = malloc(MP3_MAX_SAMPLES * 2);
    if (!dec || !win || !piece || !frame_pcm)
        die("out of memory");
    mp3_init(dec);
    skip_id3(fd);

    int have = 0, eof = 0, channels = 0, hz = 0, announced = 0, skipped_layers = 0;
    unsigned pcm_bytes = 0;
    long long total_frames = 0;

    for (;;)
    {
        if (have < WINDOW / 2 && !eof) /* refill: the window always holds at least several frames while there is more file */
        {
            long long got = sys_read((int)fd, win + have, (usize)(WINDOW - have));
            if (got <= 0)
                eof = 1;
            else
                have += (int)got;
            if (!eof && have < WINDOW / 2)
                continue;
        }
        if (have < 4)
            break;

        Mp3Info info;
        int n = mp3_decode_frame(dec, win, have, frame_pcm, &info);
        if (n > 0)
        {
            if (info.channels != channels || info.hz != hz) /* the format changed (or the first frame): play what we have first */
            {
                if (pcm_bytes)
                {
                    wav_header(piece, (unsigned)hz, (unsigned)channels, pcm_bytes);
                    long long r = sys_audio_play(piece, 44 + pcm_bytes);
                    if (r != 0)
                        die(why(r));
                    pcm_bytes = 0;
                }
                channels = info.channels;
                hz = info.hz;
                if (!announced)
                {
                    fdprintf(1, "play: %s: %d Hz, %s, %d kbps\n", name, hz, channels == 1 ? "mono" : "stereo", info.bitrate_kbps);
                    announced = 1;
                }
            }
            unsigned bytes = (unsigned)n * (unsigned)channels * 2;
            memcpy(piece + 44 + pcm_bytes, frame_pcm, bytes);
            pcm_bytes += bytes;
            total_frames++;
            if (pcm_bytes >= PIECE_PCM_BYTES)
            {
                wav_header(piece, (unsigned)hz, (unsigned)channels, pcm_bytes);
                long long r = sys_audio_play(piece, 44 + pcm_bytes);
                if (r != 0)
                    die(why(r));
                pcm_bytes = 0;
            }
        }
        else if (info.layer == 1 || info.layer == 2)
            skipped_layers++;

        int used = info.frame_bytes;
        if (used <= 0) /* no frame in the window: drop most of it and read on (or stop at the end of the file) */
        {
            if (eof)
                break;
            used = have > 4 ? have - 4 : have;
        }
        if (used > have)
            used = have;
        memmove(win, win + used, (usize)(have - used));
        have -= used;
    }
    sys_close((int)fd);

    if (pcm_bytes)
    {
        wav_header(piece, (unsigned)hz, (unsigned)channels, pcm_bytes);
        long long r = sys_audio_play(piece, 44 + pcm_bytes);
        if (r != 0)
            die(why(r));
    }
    if (total_frames == 0)
        die(skipped_layers ? "MPEG Layer I / II is not supported (only Layer III, the usual .mp3)" : "not a WAV or MP3 file");
    sys_exit(0);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    if (argc < 2)
    {
        fdprintf(2, "usage: play FILE.WAV|FILE.MP3\n");
        sys_exit(2);
    }
    long long fd = sys_open(argv[1]);
    if (fd < 0)
    {
        fdprintf(2, "play: cannot open '%s'\n", argv[1]);
        sys_exit(1);
    }
    unsigned char magic[4] = {0, 0, 0, 0};
    long long m = sys_read((int)fd, magic, 4);
    sys_seek((int)fd, 0, 0);
    if (m == 4 && magic[0] == 'R' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == 'F')
        play_wav(fd, argv[1]);
    else
        play_mp3(fd, argv[1]);
    sys_exit(0);
}
