/* Host test for wav.c: header parsing (chunk order, padding, extensible format, damaged and unsupported files) and the conversion
 * to 48 kHz stereo 16 bit (channel layout, 8 bit scaling, rate change). */
#include <stdio.h>
#include <string.h>
#include "../kernel/audio/wav.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static unsigned char F[4096];
static unsigned long N;

static void put32(unsigned v) { for (int i = 0; i < 4; i++) F[N++] = (unsigned char)(v >> (8 * i)); }
static void put16(unsigned v) { F[N++] = (unsigned char)v; F[N++] = (unsigned char)(v >> 8); }
static void tagw(const char *t) { memcpy(F + N, t, 4); N += 4; }

static void header(void) { N = 0; tagw("RIFF"); put32(0); tagw("WAVE"); }
static void fmt(unsigned format, unsigned ch, unsigned rate, unsigned bits)
{
    tagw("fmt "); put32(16); put16(format); put16(ch); put32(rate); put32(rate * ch * bits / 8); put16(ch * bits / 8); put16(bits);
}
static void data16(const short *s, unsigned n)
{
    tagw("data"); put32(n * 2);
    for (unsigned i = 0; i < n; i++) put16((unsigned short)s[i]);
}

int main(void)
{
    WavInfo wi;
    short out[64];

    /* ---- a plain 16-bit stereo 48 kHz file: converted as it is ---- */
    static const short st[] = {100, -100, 200, -200, 300, -300};
    header(); fmt(1, 2, 48000, 16); data16(st, 6);
    CHECK(wav_parse(F, N, &wi) == 0);
    CHECK(wi.rate == 48000 && wi.channels == 2 && wi.bits == 16 && wi.data_len == 12 && wi.data_off == 44);
    CHECK(wav_frames_48k(&wi) == 3);
    wav_to_48k_stereo16(F, &wi, out, 3);
    CHECK(memcmp(out, st, sizeof st) == 0);

    /* ---- mono 16 bit: both channels get the sample ---- */
    static const short mo[] = {1000, -2000, 3000};
    header(); fmt(1, 1, 48000, 16); data16(mo, 3);
    CHECK(wav_parse(F, N, &wi) == 0 && wav_frames_48k(&wi) == 3);
    wav_to_48k_stereo16(F, &wi, out, 3);
    CHECK(out[0] == 1000 && out[1] == 1000 && out[2] == -2000 && out[3] == -2000 && out[4] == 3000 && out[5] == 3000);

    /* ---- 8 bit unsigned mono: 128 = silence, scaled to 16 bit ---- */
    header(); fmt(1, 1, 48000, 8); tagw("data"); put32(3); F[N++] = 128; F[N++] = 255; F[N++] = 0;
    CHECK(wav_parse(F, N, &wi) == 0 && wi.bits == 8 && wav_frames_48k(&wi) == 3);
    wav_to_48k_stereo16(F, &wi, out, 3);
    CHECK(out[0] == 0 && out[2] == 127 * 256 && out[4] == -32768);

    /* ---- 24 kHz: every source frame twice (48000 / 24000) ---- */
    header(); fmt(1, 1, 24000, 16); data16(mo, 3);
    CHECK(wav_parse(F, N, &wi) == 0 && wav_frames_48k(&wi) == 6);
    wav_to_48k_stereo16(F, &wi, out, 6);
    CHECK(out[0] == 1000 && out[2] == 1000 && out[4] == -2000 && out[6] == -2000 && out[8] == 3000 && out[10] == 3000);

    /* ---- 96 kHz: every second source frame ---- */
    static const short fast[] = {1, 2, 3, 4};
    header(); fmt(1, 1, 96000, 16); data16(fast, 4);
    CHECK(wav_parse(F, N, &wi) == 0 && wav_frames_48k(&wi) == 2);
    wav_to_48k_stereo16(F, &wi, out, 2);
    CHECK(out[0] == 1 && out[2] == 3);

    /* ---- 44.1 kHz: 441 source frames give 480 ---- */
    header(); fmt(1, 1, 44100, 16); tagw("data"); put32(882); N += 882;
    CHECK(wav_parse(F, N, &wi) == 0 && wav_frames_48k(&wi) == 480);

    /* ---- another chunk with an odd size (padded) in front of the format ---- */
    header(); tagw("LIST"); put32(5); memcpy(F + N, "abcde", 5); N += 5; F[N++] = 0; /* + pad byte */
    fmt(1, 2, 48000, 16); data16(st, 6);
    CHECK(wav_parse(F, N, &wi) == 0 && wi.data_len == 12 && wi.data_off == 12 + 8 + 6 + 24 + 8);

    /* ---- WAVE_FORMAT_EXTENSIBLE with a PCM sub-format ---- */
    header(); tagw("fmt "); put32(40); put16(0xFFFE); put16(2); put32(48000); put32(192000); put16(4); put16(16);
    put16(22); put16(16); put32(3); put16(1); for (int i = 0; i < 14; i++) F[N++] = 0; /* sub-format GUID starting with 1 */
    data16(st, 6);
    CHECK(wav_parse(F, N, &wi) == 0 && wi.channels == 2);
    F[12 + 8 + 24] = 3; /* a float sub-format */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_CODEC);

    /* ---- the data chunk claims more than the file holds (cut file): cut to a whole number of frames ---- */
    header(); fmt(1, 2, 48000, 16); tagw("data"); put32(1000); put16(1); put16(2); put16(3); put16(4); put16(5); /* 10 bytes */
    CHECK(wav_parse(F, N, &wi) == 0 && wi.data_len == 8);

    /* ---- errors ---- */
    header(); fmt(3, 2, 48000, 32); data16(st, 6);           /* IEEE float */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_CODEC);
    header(); fmt(1, 2, 48000, 24); data16(st, 6);           /* 24 bit */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_PARAMS);
    header(); fmt(1, 6, 48000, 16); data16(st, 6);           /* 5.1 */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_PARAMS);
    header(); fmt(1, 2, 1000, 16); data16(st, 6);            /* too slow */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_PARAMS);
    header(); fmt(1, 2, 192000, 16); data16(st, 6);          /* too fast */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_PARAMS);
    header(); fmt(1, 2, 48000, 16); tagw("data"); put32(0);  /* nothing in it */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_EMPTY);
    header(); fmt(1, 2, 48000, 16); tagw("data"); put32(3); put16(1); put16(2); /* less than one frame */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_EMPTY);
    header(); data16(st, 6); fmt(1, 2, 48000, 16);           /* data before fmt */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_FORMAT);
    header(); fmt(1, 2, 48000, 16);                          /* no data chunk */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_FORMAT);
    header(); tagw("fmt "); put32(16); put16(1);             /* fmt cut short */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_FORMAT);
    header(); F[8] = 'X';                                    /* not WAVE */
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_FORMAT);
    CHECK(wav_parse(F, 11, &wi) == WAV_ERR_FORMAT);
    /* a chunk size that points far past the end must not loop or read outside the file */
    header(); tagw("junk"); put32(0xFFFFFFF0u);
    CHECK(wav_parse(F, N, &wi) == WAV_ERR_FORMAT);

    if (!fails)
        printf("wav tests passed\n");
    return fails ? 1 : 0;
}
