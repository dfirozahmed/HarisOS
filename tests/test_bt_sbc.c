/* Host test of the SBC encoder (kernel/bluetooth/bt_sbc.c).
 *
 * Three kinds of checks:
 *  - exact: the frames of a fixed signal are, byte for byte, what libsbc (the SBC library of BlueZ) makes for it (golden frames; the frames are
 *    the output of libsbc, run once when this test was written), and the sizes and headers of many settings;
 *  - structure: syncword, header fields, CRC (computed another way than in the encoder), bit allocation adds up, bands that cannot be joint;
 *  - quality: the frames are decoded with a reference decoder (tests/bt_sbc_ref.h, compared with libsbc's decoder) and the signal must come
 *    back (signal to noise ratio, delay of 73 samples). The thresholds are a little under what libsbc's own encoder reaches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_sbc_ref.h"

static int failures, checks;
#define CHECK(c)                                                  \
    do                                                            \
    {                                                             \
        checks++;                                                 \
        if (!(c))                                                 \
        {                                                         \
            failures++;                                           \
            printf("FAIL line %d: %s\n", __LINE__, #c);           \
        }                                                         \
    } while (0)

/* ---- the fixed signal of the golden frames (integer arithmetic only: the same on every machine) ---- */
#define GFR (128 * 40)
static short GPCM[GFR * 2];

static void make_golden_signal(void)
{
    unsigned lcg = 12345;
    for (int i = 0; i < GFR; i++)
    {
        lcg = lcg * 1103515245u + 12345u;
        int noise = (int)((lcg >> 16) & 0x7FF) - 1024;
        int tri1 = ((i * 97) % 4000) - 2000;
        int tri2 = ((i * 31) % 6000) - 3000;
        GPCM[2 * i] = (short)(tri1 * 4 + noise);
        GPCM[2 * i + 1] = (short)(tri2 * 3 + noise / 2);
    }
}

static const unsigned char GOLDEN_JOINT16_BP38[][89] = {
    {
        0x9C, 0xFD, 0x26, 0xC7, 0xD6, 0xCA, 0xB9, 0xA9, 0x8A, 0xCA, 0x99, 0x89, 0x89, 0x7D, 0xB5, 0xAB,
        0xED, 0x55, 0xF6, 0xD6, 0xAF, 0xB5, 0x57, 0x1B, 0x5A, 0xBE, 0xD5, 0x5B, 0x8D, 0x8B, 0x03, 0x19,
        0x10, 0x39, 0x94, 0x8E, 0x54, 0x76, 0xD4, 0xB5, 0x39, 0x15, 0x1B, 0x58, 0xDC, 0xD5, 0x58, 0x6D,
        0x6B, 0x83, 0x55, 0x7D, 0xB5, 0x2E, 0x6D, 0x55, 0x20, 0x44, 0xD2, 0x84, 0x52, 0xEA, 0x68, 0xB5,
        0x55, 0x1B, 0x4D, 0x6B, 0x3B, 0x55, 0x79, 0xB5, 0xAD, 0x0D, 0x56, 0x56, 0xD6, 0xB5, 0xB5, 0x57,
        0x92, 0x0A, 0x42, 0x91, 0x12, 0xCA, 0x2A, 0x96, 0x65},
    {
        0x9C, 0xFD, 0x26, 0x63, 0x7E, 0xCB, 0xA9, 0x99, 0x9A, 0xDA, 0xA9, 0x99, 0x9A, 0x7D, 0xB5, 0x58,
        0x1B, 0x56, 0x76, 0xE5, 0x62, 0x6D, 0x5C, 0x1B, 0x15, 0x8D, 0xB5, 0x66, 0x72, 0x6A, 0x57, 0x2A,
        0x06, 0x18, 0x99, 0xB1, 0x85, 0xE6, 0xD5, 0x68, 0x4D, 0x59, 0xDB, 0x95, 0xA6, 0x35, 0x6E, 0x6D,
        0x66, 0xA6, 0xD5, 0xAA, 0x59, 0x5A, 0xED, 0x94, 0x18, 0xC8, 0x6D, 0xAC, 0x86, 0x93, 0x55, 0xB1,
        0xB5, 0x65, 0x6D, 0x56, 0xB6, 0xD5, 0xB4, 0x90, 0xA6, 0x35, 0x97, 0x2A, 0x85, 0x88, 0x6D, 0x21,
        0xD5, 0x46, 0x49, 0x54, 0x52, 0x72, 0x55, 0x35, 0x25},
    {
        0x9C, 0xFD, 0x26, 0xF2, 0x00, 0xCC, 0xBA, 0xA9, 0x9A, 0xC7, 0x76, 0x77, 0x78, 0x98, 0xE7, 0x66,
        0xC9, 0x62, 0xB3, 0x9E, 0x1A, 0xA6, 0x5C, 0xD1, 0x67, 0x8A, 0xA0, 0x0B, 0xA2, 0x35, 0x84, 0x95,
        0x3A, 0xF9, 0x85, 0x4A, 0xCA, 0x43, 0xA2, 0x1A, 0xAC, 0x4A, 0x6E, 0x77, 0x74, 0xB6, 0x72, 0xC1,
        0xA5, 0xB2, 0xF1, 0x46, 0x74, 0xAD, 0x24, 0x24, 0xB4, 0x64, 0xDC, 0x32, 0x58, 0xAE, 0x78, 0x68,
        0xCE, 0x6A, 0x3D, 0xDD, 0x2B, 0x56, 0xCA, 0xF7, 0x87, 0x2D, 0x95, 0x82, 0x48, 0x53, 0x57, 0xD1,
        0x74, 0x65, 0xA4, 0xE5, 0x60, 0x39, 0xDD, 0xAB, 0xA9},
};

static const unsigned char GOLDEN_STEREO8_BP30[][42] = {
    {
        0x9C, 0xD9, 0x1E, 0xF9, 0xCA, 0x99, 0x99, 0x89, 0xDB, 0x99, 0x89, 0x89, 0x7D, 0xAA, 0xFB, 0x55,
        0xF6, 0xAB, 0xED, 0x57, 0x5A, 0xAE, 0xB5, 0x5C, 0x82, 0xB9, 0x06, 0x24, 0x52, 0x79, 0xA5, 0x25,
        0x41, 0xCE, 0x48, 0x9A, 0xA9, 0x35, 0x69, 0x4A, 0xA6, 0xD5},
    {
        0x9C, 0xD9, 0x1E, 0x3D, 0xCB, 0xBA, 0xAA, 0x9A, 0xC7, 0x77, 0x67, 0x78, 0xD2, 0xDB, 0x4D, 0x2D,
        0x88, 0x21, 0x48, 0xD1, 0xB5, 0x2A, 0x64, 0x23, 0x36, 0xDB, 0x93, 0xA0, 0xDA, 0x6D, 0x57, 0x2B,
        0x69, 0xA1, 0x98, 0x09, 0x12, 0x87, 0x04, 0xE5, 0x4C, 0x1F},
    {
        0x9C, 0xD9, 0x1E, 0x3F, 0xCB, 0xAA, 0x99, 0x9A, 0xC7, 0x68, 0x78, 0x87, 0x7E, 0xDB, 0x58, 0x56,
        0x7B, 0x71, 0x64, 0x9C, 0x2D, 0xA5, 0x9C, 0x66, 0x39, 0x6A, 0xA5, 0x07, 0x85, 0x2B, 0x55, 0xEA,
        0x6D, 0x70, 0x59, 0xD1, 0xC5, 0xCE, 0x6E, 0xB6, 0x9B, 0x56},
};

/* SNR allocation instead of loudness (the same signal, 48 kHz, joint stereo, 16 blocks, bitpool 38) */
static const unsigned char GOLDEN_SNR_JOINT16_BP38[][89] = {
    {
        0x9C, 0xFF, 0x26, 0x26, 0xD6, 0xCA, 0xB9, 0xA9, 0x8A, 0xCA, 0x99, 0x89, 0x89, 0x7B, 0x75, 0xAD,
        0xED, 0x55, 0xED, 0xD6, 0xB7, 0xB5, 0x57, 0x37, 0x5A, 0xDE, 0xD5, 0x5B, 0x19, 0x8B, 0x83, 0x15,
        0x10, 0x89, 0x92, 0x4E, 0x58, 0x6E, 0x14, 0xBA, 0x39, 0x54, 0xB7, 0x58, 0xAC, 0xD5, 0x58, 0xDD,
        0x6B, 0xBB, 0x55, 0x7B, 0x75, 0x2F, 0x2D, 0x55, 0x20, 0x84, 0xD9, 0x04, 0x52, 0xD6, 0x68, 0x9B,
        0x54, 0x5A, 0x9D, 0x6B, 0x9B, 0x55, 0x7B, 0x65, 0xB2, 0x6D, 0x56, 0x4D, 0xD6, 0xBA, 0xB5, 0x57,
        0x24, 0x0A, 0x60, 0x90, 0x51, 0x92, 0x2B, 0x46, 0x65},
    {
        0x9C, 0xFF, 0x26, 0x82, 0x7E, 0xCB, 0xA9, 0x99, 0x9A, 0xDA, 0xA9, 0x99, 0x9A, 0x7B, 0xB5, 0x58,
        0x1B, 0x56, 0x70, 0xE5, 0x62, 0x6D, 0x5B, 0xB3, 0x15, 0x8D, 0xB5, 0x64, 0xF2, 0x6A, 0x57, 0x29,
        0x05, 0x18, 0x99, 0xB1, 0x85, 0xCC, 0xD5, 0x68, 0x4D, 0x59, 0xC3, 0x95, 0xA6, 0x35, 0x6C, 0xED,
        0x66, 0xA6, 0xD5, 0xA4, 0xD9, 0x4A, 0xED, 0x94, 0x12, 0xC8, 0x2D, 0xAC, 0x96, 0xB3, 0x55, 0xB1,
        0xB5, 0x64, 0xED, 0x56, 0xB6, 0xD5, 0xB1, 0x90, 0x96, 0x35, 0x9B, 0x38, 0x85, 0x48, 0x6D, 0x11,
        0xAD, 0x44, 0x49, 0x54, 0x52, 0xF2, 0x55, 0x35, 0x25},
    {
        0x9C, 0xFF, 0x26, 0x13, 0x00, 0xCC, 0xBA, 0xA9, 0x9A, 0xC7, 0x76, 0x77, 0x78, 0x95, 0xE7, 0xB3,
        0x34, 0x2E, 0xA7, 0x1F, 0x10, 0xB0, 0xCC, 0xE3, 0x63, 0xCA, 0xC3, 0xCB, 0x46, 0x3B, 0x01, 0x52,
        0x39, 0xFA, 0x42, 0x52, 0x56, 0x37, 0x63, 0x0E, 0xAD, 0x8A, 0x5D, 0x83, 0xC5, 0x36, 0xF2, 0x85,
        0xD2, 0xED, 0x1D, 0x44, 0xF4, 0x56, 0xC9, 0x80, 0xA8, 0xE6, 0x71, 0x12, 0x48, 0x9E, 0x7C, 0x3A,
        0x39, 0xA9, 0x7E, 0x0E, 0xCA, 0xEA, 0xC9, 0xE7, 0xC3, 0x4B, 0xB1, 0x84, 0x8A, 0x2A, 0xD2, 0xF1,
        0x69, 0x62, 0xD9, 0x2C, 0xA0, 0x79, 0xCE, 0xEA, 0xF4},
};

/* 44.1 kHz, stereo, 12 blocks, bitpool 53, loudness: the offsets of the 44.1 kHz row, a frame that does not end on a byte boundary */
static const unsigned char GOLDEN_441_STEREO12_BP53[][92] = {
    {
        0x9C, 0xA9, 0x35, 0x18, 0xCB, 0xBA, 0xAA, 0x9A, 0xDB, 0x99, 0x89, 0x89, 0x7E, 0xEE, 0xEE, 0xEA,
        0xFD, 0xDB, 0x6B, 0xF7, 0x77, 0x77, 0x57, 0xEE, 0xDB, 0x5D, 0x3B, 0xBB, 0xBA, 0xBC, 0x66, 0xDA,
        0xE6, 0x19, 0xE1, 0xD5, 0xD4, 0x17, 0x52, 0x69, 0x14, 0xEA, 0x47, 0xCE, 0x66, 0x25, 0x68, 0x85,
        0x75, 0x3A, 0xF6, 0x06, 0x33, 0xBB, 0xBA, 0xAA, 0x57, 0x6D, 0xB4, 0xD9, 0xDE, 0x1D, 0x53, 0xBC,
        0x71, 0xD2, 0xEE, 0xEA, 0xEA, 0xAD, 0xDB, 0x4B, 0x11, 0x21, 0x59, 0xA5, 0xAE, 0x9B, 0x46, 0xDB,
        0x35, 0x9C, 0xB0, 0x76, 0xDB, 0x19, 0x9D, 0xDD, 0xE5, 0x9B, 0xB6, 0xD0},
    {
        0x9C, 0xA9, 0x35, 0x30, 0xCB, 0xAA, 0xA9, 0x9A, 0xC7, 0x78, 0x78, 0x88, 0xA1, 0x0A, 0xAE, 0x9C,
        0x57, 0x09, 0x6E, 0x56, 0x76, 0x74, 0x9B, 0x25, 0x4A, 0x20, 0x20, 0x99, 0x92, 0x5C, 0xA3, 0x6A,
        0x27, 0x86, 0xD2, 0x96, 0xFD, 0x9A, 0xD7, 0xEE, 0xF0, 0xE9, 0xB8, 0x73, 0x14, 0xCF, 0x86, 0xA6,
        0x71, 0xC9, 0x5A, 0x37, 0x0B, 0x3A, 0xC3, 0x8E, 0x78, 0xA5, 0xD3, 0x22, 0xAA, 0x76, 0xB5, 0x95,
        0x8D, 0x07, 0xA1, 0x67, 0xA3, 0xB4, 0xAC, 0x93, 0xD6, 0x77, 0x66, 0xDE, 0x26, 0x8B, 0x67, 0x43,
        0xD3, 0xB4, 0xF3, 0xAC, 0x5B, 0x75, 0x9D, 0x9E, 0xA7, 0xB1, 0x53, 0x50},
    {
        0x9C, 0xA9, 0x35, 0x01, 0xCB, 0xBA, 0xAA, 0xAA, 0xDB, 0xBA, 0x9A, 0x9A, 0xA9, 0x56, 0xC6, 0x96,
        0x1D, 0xB5, 0xA8, 0x3A, 0x66, 0x94, 0xB6, 0xED, 0xAD, 0x5A, 0xAB, 0x6D, 0xAD, 0x96, 0x6D, 0x6B,
        0x2A, 0x1B, 0x6D, 0x6C, 0x3B, 0x64, 0x5B, 0x4A, 0xE2, 0x8A, 0xC0, 0x08, 0x25, 0x66, 0xB6, 0x54,
        0x52, 0x38, 0xD4, 0x64, 0x79, 0xD6, 0x64, 0x25, 0x66, 0xD6, 0xA9, 0x9A, 0x46, 0x95, 0x34, 0x36,
        0xB5, 0x98, 0xCD, 0xB6, 0xAA, 0x9D, 0xB5, 0xAD, 0x66, 0x71, 0xA5, 0x58, 0xED, 0xAD, 0x73, 0x4A,
        0x71, 0xAA, 0xF7, 0x6D, 0x6A, 0x5C, 0x1E, 0x65, 0x99, 0x3B, 0x6B, 0x50},
};

static BtSbcConfig cfg(unsigned freq, unsigned mode, unsigned blocks, unsigned alloc, unsigned bitpool)
{
    BtSbcConfig c;
    memset(&c, 0, sizeof c);
    c.freq = freq;
    c.mode = mode;
    c.blocks = blocks;
    c.bands = 1;
    c.alloc = alloc;
    c.bitpool = bitpool;
    c.min_bitpool = 2;
    return c;
}

/* ---- test signals ---- */
static unsigned RNG = 99;
static unsigned rnd(void)
{
    RNG = RNG * 1664525u + 1013904223u;
    return RNG >> 8;
}

/* kind 0: two sines (1000 Hz left, 1500 Hz right); 1: three tones plus noise; 2: the same sine on both channels; 3: silence */
static void make_signal(short *pcm, int frames, int kind)
{
    for (int i = 0; i < frames; i++)
    {
        double l = 0, r = 0;
        if (kind == 0)
        {
            l = 0.5 * sin(2 * 3.14159265358979 * 1000 * i / 48000.0);
            r = 0.3 * sin(2 * 3.14159265358979 * 1500 * i / 48000.0);
        }
        else if (kind == 1)
        {
            double noise = 0.05 * ((rnd() & 0xFFFF) / 65535.0 - 0.5);
            l = 0.4 * sin(2 * 3.14159265358979 * 440 * i / 48000.0) + 0.2 * sin(2 * 3.14159265358979 * 3000 * i / 48000.0) + noise;
            r = 0.7 * l + 0.1 * sin(2 * 3.14159265358979 * 7000 * i / 48000.0);
        }
        else if (kind == 2)
            l = r = 0.9 * sin(2 * 3.14159265358979 * 220 * i / 48000.0);
        pcm[2 * i] = (short)lrint(l * 32767);
        pcm[2 * i + 1] = (short)lrint(r * 32767);
    }
}

#define NF 24000
static short SIG[NF * 2], DEC[NF * 2 + 4000];
static unsigned char ENC[100000];

/* encodes the whole signal, decodes it with the reference decoder. Returns the number of frames made; fills DEC. */
static int roundtrip(const BtSbcConfig *c, int *bytes, int *crc_bad)
{
    BtSbcEnc e;
    if (bt_sbc_init(&e, c))
        return -1;
    unsigned spf = bt_sbc_samples(&e);
    int len = 0, frames = 0;
    for (unsigned f = 0; (f + 1) * spf <= NF; f++)
    {
        int r = bt_sbc_encode(&e, SIG + f * spf * 2, ENC + len, BT_SBC_MAX_FRAME);
        if (r != (int)bt_sbc_frame_bytes(&e))
            return -1;
        len += r;
        frames++;
    }
    RefDec d;
    ref_init(&d);
    int pos = 0, n = 0;
    *crc_bad = 0;
    while (pos < len)
    {
        RefFrame fi;
        int u = ref_decode(&d, ENC + pos, (unsigned)(len - pos), DEC + n * 2, &fi);
        if (u <= 0)
            return -1;
        *crc_bad += !fi.crc_ok;
        pos += u;
        n += (int)spf;
    }
    if (bytes)
        *bytes = len;
    return frames;
}

/* signal to noise ratio of the decoded signal against the original at the codec's delay */
static double snr_at(int delay)
{
    double s = 0, e = 0;
    for (int i = 3000; i < NF - 3000; i++)
        for (int ch = 0; ch < 2; ch++)
        {
            double a = SIG[2 * i + ch], b = DEC[2 * (i + delay) + ch];
            s += a * a;
            e += (a - b) * (a - b);
        }
    return 10 * log10(s / (e + 1e-9));
}

static double best_snr(int *delay)
{
    double best = -1e9;
    int bd = 0;
    for (int d = 60; d < 90; d++)
    {
        double r = snr_at(d);
        if (r > best)
        {
            best = r;
            bd = d;
        }
    }
    *delay = bd;
    return best;
}

int main(void)
{
    BtSbcEnc e;
    unsigned char out[BT_SBC_MAX_FRAME];

    /* ---- settings: what is accepted, how long a frame is (the lengths are libsbc's) ---- */
    {
        static const struct { BtSbcConfig c; unsigned samples, bytes; } T[] = {
            {{1, 1, 1, 1, 1, 38, 2}, 128, 89},   /* 48 kHz, joint stereo, 16 blocks, 8 subbands, loudness, bitpool 38 */
            {{1, 1, 1, 1, 1, 53, 2}, 128, 119},
            {{1, 2, 1, 1, 1, 38, 2}, 128, 88},   /* stereo: no joint stereo flags (one byte less) */
            {{1, 1, 4, 1, 1, 38, 2}, 64, 51},    /* 8 blocks */
            {{1, 1, 2, 1, 1, 38, 2}, 96, 70},    /* 12 blocks */
            {{1, 1, 8, 1, 1, 38, 2}, 32, 32},    /* 4 blocks */
            {{1, 2, 1, 1, 1, 80, 2}, 128, 172},
            {{1, 1, 1, 1, 1, 2, 2}, 128, 17},
            {{1, 1, 1, 1, 1, 20, 2}, 128, 53},
            {{2, 1, 1, 1, 2, 53, 2}, 128, 119},  /* 44.1 kHz, SNR allocation */
            {{4, 2, 4, 1, 1, 30, 2}, 64, 42},    /* 32 kHz */
            {{8, 2, 4, 1, 1, 30, 2}, 64, 42},    /* 16 kHz */
            {{1, 1, 8, 1, 1, 53, 2}, 32, 40},    /* 4 blocks, bitpool 53, joint: 212 + 8 bits = 27.5 bytes, rounded up */
            {{1, 2, 8, 1, 1, 53, 2}, 32, 39},    /* stereo: 212 bits = 26.5 bytes, rounded up */
            {{2, 2, 2, 1, 1, 53, 2}, 96, 92},
        };
        for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        {
            CHECK(bt_sbc_init(&e, &T[i].c) == 0);
            CHECK(bt_sbc_samples(&e) == T[i].samples && bt_sbc_frame_bytes(&e) == T[i].bytes);
        }
        /* what is refused */
        BtSbcConfig c;
        c = cfg(1, 8, 1, 1, 38); /* mono */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 4, 1, 1, 38); /* dual channel */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 1, 1, 1, 38);
        c.bands = 2; /* 4 subbands */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(3, 1, 1, 1, 38); /* two sampling rates at once: not a choice */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 3, 1, 1, 38); /* two modes */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 1, 0, 1, 38); /* no block length */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 1, 3, 1, 38); /* two */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 1, 1, 0, 38); /* no allocation method */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 1, 1, 3, 38);
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 1, 1, 1, 1); /* bitpool too small */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 1, 1, 1, 251); /* ... too large */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 2, 1, 1, 250); /* a frame of 512 bytes: more than the encoder makes */
        CHECK(bt_sbc_init(&e, &c) == -1);
        c = cfg(1, 2, 1, 1, 120); /* 252 bytes: still fine */
        CHECK(bt_sbc_init(&e, &c) == 0 && bt_sbc_frame_bytes(&e) == 252);
    }

    /* ---- the golden frames: byte for byte what libsbc makes ---- */
    make_golden_signal();
    {
        BtSbcConfig c = cfg(1, 1, 1, 1, 38);
        CHECK(bt_sbc_init(&e, &c) == 0);
        for (unsigned f = 0; f < 3; f++)
        {
            CHECK(bt_sbc_encode(&e, GPCM + f * 128 * 2, out, sizeof out) == 89);
            CHECK(memcmp(out, GOLDEN_JOINT16_BP38[f], 89) == 0);
        }
        c = cfg(1, 2, 4, 1, 30);
        CHECK(bt_sbc_init(&e, &c) == 0);
        for (unsigned f = 0; f < 3; f++)
        {
            CHECK(bt_sbc_encode(&e, GPCM + f * 64 * 2, out, sizeof out) == 42);
            CHECK(memcmp(out, GOLDEN_STEREO8_BP30[f], 42) == 0);
        }
    }

    {
        BtSbcConfig c = cfg(1, 1, 1, 2, 38);
        CHECK(bt_sbc_init(&e, &c) == 0);
        for (unsigned f = 0; f < 3; f++)
        {
            CHECK(bt_sbc_encode(&e, GPCM + f * 128 * 2, out, sizeof out) == 89);
            CHECK(memcmp(out, GOLDEN_SNR_JOINT16_BP38[f], 89) == 0);
        }
        c = cfg(2, 2, 2, 1, 53);
        CHECK(bt_sbc_init(&e, &c) == 0);
        for (unsigned f = 0; f < 3; f++)
        {
            CHECK(bt_sbc_encode(&e, GPCM + f * 96 * 2, out, sizeof out) == 92);
            CHECK(memcmp(out, GOLDEN_441_STEREO12_BP53[f], 92) == 0);
        }
    }

    /* ---- the header ---- */
    {
        BtSbcConfig c = cfg(1, 1, 1, 1, 38);
        bt_sbc_init(&e, &c);
        bt_sbc_encode(&e, GPCM, out, sizeof out);
        CHECK(out[0] == 0x9C && out[1] == 0xFD && out[2] == 38); /* 48 kHz | 16 blocks | joint | loudness | 8 subbands */
        c = cfg(2, 2, 4, 2, 53);
        bt_sbc_init(&e, &c);
        bt_sbc_encode(&e, GPCM, out, sizeof out);
        CHECK(out[0] == 0x9C && out[1] == 0x9B && out[2] == 53); /* 44.1 kHz | 8 blocks | stereo | SNR | 8 subbands */
        c = cfg(8, 1, 8, 1, 20);
        bt_sbc_init(&e, &c);
        bt_sbc_encode(&e, GPCM, out, sizeof out);
        CHECK(out[1] == 0x0D); /* 16 kHz | 4 blocks | joint | loudness | 8 subbands */
    }

    /* ---- every frame of several signals: structure ---- */
    {
        static const struct { BtSbcConfig c; } S[] = {
            {{1, 1, 1, 1, 1, 38, 2}}, {{1, 2, 1, 1, 1, 38, 2}}, {{1, 1, 4, 1, 1, 53, 2}}, {{2, 1, 2, 1, 2, 30, 2}}, {{1, 1, 8, 1, 1, 2, 2}}, {{1, 2, 1, 1, 1, 80, 2}},
        };
        for (int kind = 0; kind < 4; kind++)
        {
            make_signal(SIG, NF, kind);
            for (unsigned si = 0; si < sizeof S / sizeof S[0]; si++)
            {
                BtSbcConfig c = S[si].c;
                CHECK(bt_sbc_init(&e, &c) == 0);
                unsigned spf = bt_sbc_samples(&e), fb = bt_sbc_frame_bytes(&e);
                int ones = 0, joint_used = 0, frames = 0, sum_ok = 1, crc_ok = 1, joint_last = 1;
                for (unsigned f = 0; (f + 1) * spf <= 4000; f++)
                {
                    int r = bt_sbc_encode(&e, SIG + f * spf * 2, out, sizeof out);
                    CHECK(r == (int)fb);
                    RefDec d;
                    RefFrame fi;
                    ref_init(&d);
                    short pcm[256 * 2];
                    CHECK(ref_decode(&d, out, fb, pcm, &fi) == (int)fb);
                    crc_ok &= fi.crc_ok;
                    int total = 0;
                    for (int ch = 0; ch < 2; ch++)
                        for (int sb = 0; sb < 8; sb++)
                        {
                            ones |= fi.bits[ch][sb] == 1; /* one bit per sample is never given: the quantizer needs at least 3 levels, so 2 bits */
                            CHECK(fi.bits[ch][sb] >= 0 && fi.bits[ch][sb] <= 16);
                            total += fi.bits[ch][sb];
                        }
                    sum_ok &= total == (int)c.bitpool;
                    if (fi.mode == 3)
                    {
                        joint_used |= (fi.joint & 0xFE) != 0;
                        joint_last &= (fi.joint & 1) == 0; /* the highest band is never joint */
                    }
                    else
                        joint_last &= fi.joint == 0;
                    frames++;
                }
                CHECK(frames > 10 && crc_ok && sum_ok && !ones && joint_last);
                if (kind == 2 && c.mode == 1)
                    CHECK(joint_used); /* the same on both sides: sum and difference are cheaper */
                if (c.mode == 2)
                    CHECK(!joint_used);
            }
        }
    }

    /* ---- silence ---- */
    {
        BtSbcConfig c = cfg(1, 1, 1, 1, 38);
        bt_sbc_init(&e, &c);
        short zero[128 * 2];
        memset(zero, 0, sizeof zero);
        CHECK(bt_sbc_encode(&e, zero, out, sizeof out) == 89);
        RefDec d;
        RefFrame fi;
        ref_init(&d);
        short pcm[128 * 2];
        CHECK(ref_decode(&d, out, 89, pcm, &fi) == 89 && fi.crc_ok);
        int sfs = 0;
        for (int ch = 0; ch < 2; ch++)
            for (int sb = 0; sb < 8; sb++)
                sfs += fi.sf[ch][sb];
        CHECK(sfs == 0 && fi.joint == 0);
        int maxabs = 0;
        for (int i = 0; i < 256; i++)
            maxabs = abs(pcm[i]) > maxabs ? abs(pcm[i]) : maxabs;
        CHECK(maxabs <= 2); /* silence comes back as (almost) silence */
    }

    /* ---- quality: the signal comes back ---- */
    {
        int delay, bytes, bad;
        BtSbcConfig c = cfg(1, 1, 1, 1, 38);
        make_signal(SIG, NF, 0);
        CHECK(roundtrip(&c, &bytes, &bad) > 100 && bad == 0);
        double s0 = best_snr(&delay);
        CHECK(delay == 73 && s0 > 55); /* libsbc: 63.8 dB */
        make_signal(SIG, NF, 1);
        CHECK(roundtrip(&c, &bytes, &bad) > 100 && bad == 0);
        double s1 = best_snr(&delay);
        CHECK(delay == 73 && s1 > 28); /* libsbc: 30.3 dB, the noise in the signal is not coded well at this bitpool */
        make_signal(SIG, NF, 2);
        CHECK(roundtrip(&c, &bytes, &bad) > 100 && bad == 0);
        double s2 = best_snr(&delay);
        CHECK(delay == 73 && s2 > 68); /* libsbc: 74.7 dB */
        BtSbcConfig cs = cfg(1, 2, 1, 1, 38); /* the same without joint stereo: worse for equal channels */
        CHECK(roundtrip(&cs, &bytes, &bad) > 100 && bad == 0);
        CHECK(s2 > best_snr(&delay) + 3);
        /* a higher bitpool is better, a lower one worse (the noisy signal shows it) */
        make_signal(SIG, NF, 1);
        BtSbcConfig hi = cfg(1, 1, 1, 1, 53), lo = cfg(1, 1, 1, 1, 30);
        CHECK(roundtrip(&hi, &bytes, &bad) > 100);
        double shi = best_snr(&delay);
        CHECK(roundtrip(&lo, &bytes, &bad) > 100);
        double slo = best_snr(&delay);
        CHECK(shi > s1 + 2 && slo < s1 - 2 && shi > 32 && slo > 22);
        /* other block lengths, other allocation, other rates keep the quality */
        static const BtSbcConfig O[] = {{1, 1, 4, 1, 1, 38, 2}, {1, 1, 2, 1, 1, 38, 2}, {1, 1, 8, 1, 1, 38, 2}, {1, 1, 1, 1, 2, 38, 2}, {2, 1, 1, 1, 1, 38, 2}, {1, 2, 4, 1, 2, 38, 2}};
        for (unsigned i = 0; i < sizeof O / sizeof O[0]; i++)
        {
            CHECK(roundtrip(&O[i], &bytes, &bad) > 100 && bad == 0);
            double so = best_snr(&delay);
            CHECK(delay == 73 && so > 26);
        }
    }

    /* ---- the signal at its limits ---- */
    {
        BtSbcConfig c = cfg(1, 1, 1, 1, 38);
        /* full scale square wave, the most negative sample, white noise at full scale: valid frames, no overflow */
        for (int kind = 0; kind < 3; kind++)
        {
            for (int i = 0; i < NF; i++)
            {
                short l, r;
                if (kind == 0)
                    l = r = (short)(((i / 24) & 1) ? 32767 : -32768);
                else if (kind == 1)
                {
                    l = -32768;
                    r = 32767;
                }
                else
                {
                    l = (short)(rnd() & 0xFFFF);
                    r = (short)(rnd() & 0xFFFF);
                }
                SIG[2 * i] = l;
                SIG[2 * i + 1] = r;
            }
            int bytes, bad, delay;
            CHECK(roundtrip(&c, &bytes, &bad) > 100 && bad == 0);
            if (kind == 0)
                CHECK(best_snr(&delay) > 15);
        }
    }

    /* ---- state, errors ---- */
    {
        BtSbcConfig c = cfg(1, 1, 1, 1, 38);
        make_signal(SIG, NF, 1);
        unsigned char a[BT_SBC_MAX_FRAME], b[BT_SBC_MAX_FRAME], a2[BT_SBC_MAX_FRAME];
        bt_sbc_init(&e, &c);
        bt_sbc_encode(&e, SIG, a, sizeof a);
        bt_sbc_encode(&e, SIG + 256, a2, sizeof a2);
        bt_sbc_init(&e, &c); /* again from nothing: the same */
        bt_sbc_encode(&e, SIG, b, sizeof b);
        CHECK(memcmp(a, b, 89) == 0);
        bt_sbc_init(&e, &c);
        bt_sbc_encode(&e, SIG + 256, b, sizeof b); /* the second frame without the first before it: not the same (the filter remembers) */
        CHECK(memcmp(a2, b, 89) != 0);
        bt_sbc_init(&e, &c);
        CHECK(bt_sbc_encode(&e, SIG, b, 88) == -1);  /* one byte too little */
        CHECK(bt_sbc_encode(&e, SIG, b, 89) == 89);  /* exactly enough */
        CHECK(bt_sbc_encode(&e, SIG, b, 0) == -1);
    }

    printf("bt_sbc: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
