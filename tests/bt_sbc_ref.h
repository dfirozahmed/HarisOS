/* A reference SBC decoder for the tests of the encoder (floating point, written from the A2DP specification: synthesis filter bank, appendix B).
 * It was compared with libsbc, the SBC library of BlueZ, on the frames of the encoder at many settings (same samples to within rounding). The 80
 * window numbers are the specification's (taken from libsbc, see bt_sbc.c) as decimals, not copied from the integer table of the encoder, so a
 * rounding mistake in that table shows. Only what the encoder makes is understood: 8 subbands, stereo or joint stereo. */
#ifndef HARIS_TEST_BT_SBC_REF_H
#define HARIS_TEST_BT_SBC_REF_H

#include <math.h>
#include <string.h>

#include "bt_sbc.h"

static const double REF_C[80] = {
    0.000000000e+00, 1.565753980e-04, 3.432564250e-04, 5.546202020e-04,
    8.239195060e-04, 1.139925070e-03, 1.476401690e-03, 1.783717250e-03,
    2.011825420e-03, 2.103719890e-03, 1.994545540e-03, 1.616562830e-03,
    9.021545020e-04, -1.788053610e-04, -1.649730980e-03, -3.497174540e-03,
    5.659494730e-03, 8.029411630e-03, 1.045844430e-02, 1.274723350e-02,
    1.465252630e-02, 1.590456030e-02, 1.622084710e-02, 1.531841060e-02,
    1.293718060e-02, 8.857575400e-03, 2.924084420e-03, -4.915780240e-03,
    -1.464040760e-02, -2.610987520e-02, -3.907513810e-02, -5.318730320e-02,
    6.799894310e-02, 8.298475780e-02, 9.757539180e-02, 1.111966890e-01,
    1.232645480e-01, 1.332644150e-01, 1.407535050e-01, 1.453898470e-01,
    1.469550680e-01, 1.453898470e-01, 1.407535050e-01, 1.332644150e-01,
    1.232645480e-01, 1.111966890e-01, 9.757539180e-02, 8.298475780e-02,
    -6.799894310e-02, -5.318730320e-02, -3.907513810e-02, -2.610987520e-02,
    -1.464040760e-02, -4.915780240e-03, 2.924084420e-03, 8.857575400e-03,
    1.293718060e-02, 1.531841060e-02, 1.622084710e-02, 1.590456030e-02,
    1.465252630e-02, 1.274723350e-02, 1.045844430e-02, 8.029411630e-03,
    -5.659494730e-03, -3.497174540e-03, -1.649730980e-03, -1.788053610e-04,
    9.021545020e-04, 1.616562830e-03, 1.994545540e-03, 2.103719890e-03,
    2.011825420e-03, 1.783717250e-03, 1.476401690e-03, 1.139925070e-03,
    8.239195060e-04, 5.546202020e-04, 3.432564250e-04, 1.565753980e-04,
};

typedef struct
{
    double v[2][160];
} RefDec;

static void ref_init(RefDec *d) { memset(d, 0, sizeof *d); }

/* CRC-8 over bytes (polynomial 0x1D, start 0x0F), table driven: a different way to the bit by bit one of the encoder */
static unsigned ref_crc(const unsigned char *p, unsigned n)
{
    static unsigned table[256];
    static int ready;
    if (!ready)
    {
        for (unsigned i = 0; i < 256; i++)
        {
            unsigned c = i;
            for (int b = 0; b < 8; b++)
                c = (c & 0x80) ? ((c << 1) ^ 0x1D) & 0xFF : (c << 1) & 0xFF;
            table[i] = c;
        }
        ready = 1;
    }
    unsigned crc = 0x0F;
    for (unsigned i = 0; i < n; i++)
        crc = table[crc ^ p[i]];
    return crc;
}

typedef struct
{
    int freq_code, blocks, mode, alloc, bitpool;
    int joint;
    int sf[2][8];
    int bits[2][8];
    int crc_ok;
} RefFrame;

/* Decodes one frame: its 8 * blocks sample frames (left, right interleaved) into `out`. Returns the frame length, or -1 if it is not a frame we make. */
static int ref_decode(RefDec *d, const unsigned char *f, unsigned len, short *out, RefFrame *info)
{
    if (len < 4 || f[0] != 0x9C || !(f[1] & 1))
        return -1;
    RefFrame r;
    memset(&r, 0, sizeof r);
    r.freq_code = (f[1] >> 6) & 3;
    r.blocks = ((f[1] >> 4) & 3) * 4 + 4;
    r.mode = (f[1] >> 2) & 3;
    r.alloc = (f[1] >> 1) & 1;
    r.bitpool = f[2];
    if (r.mode != 2 && r.mode != 3)
        return -1;
    unsigned pos = 4 * 8; /* bit position */
    if (r.mode == 3)
    {
        r.joint = f[4];
        pos += 8;
    }
    for (int ch = 0; ch < 2; ch++)
        for (int sb = 0; sb < 8; sb++)
        {
            r.sf[ch][sb] = (f[pos >> 3] >> (4 - (pos & 7))) & 15;
            pos += 4;
        }
    unsigned char crcin[16];
    unsigned n = 0;
    crcin[n++] = f[1];
    crcin[n++] = f[2];
    for (unsigned i = 4; i < pos / 8; i++)
        crcin[n++] = f[i];
    r.crc_ok = ref_crc(crcin, n) == f[3];

    BtSbcEnc shape;
    memset(&shape, 0, sizeof shape);
    shape.freq_code = r.freq_code;
    shape.alloc_code = r.alloc;
    shape.bitpool = r.bitpool;
    bt_sbc_allocate(&shape, r.sf, r.bits);

    for (int blk = 0; blk < r.blocks; blk++)
    {
        double s[2][8];
        for (int ch = 0; ch < 2; ch++)
            for (int sb = 0; sb < 8; sb++)
            {
                int nb = r.bits[ch][sb];
                if (!nb)
                {
                    s[ch][sb] = 0;
                    continue;
                }
                unsigned q = 0;
                for (int i = 0; i < nb; i++, pos++)
                    q = (q << 1) | ((f[pos >> 3] >> (7 - (pos & 7))) & 1);
                double levels = (double)((1u << nb) - 1);
                s[ch][sb] = ldexp(1.0, r.sf[ch][sb] + 1) * ((2.0 * q + 1.0) / levels - 1.0);
            }
        if (r.mode == 3)
            for (int sb = 0; sb < 7; sb++)
                if (r.joint & (1 << (7 - sb)))
                {
                    double m = s[0][sb], dd = s[1][sb];
                    s[0][sb] = m + dd;
                    s[1][sb] = m - dd;
                }
        for (int ch = 0; ch < 2; ch++)
        {
            double *v = d->v[ch];
            memmove(v + 16, v, 144 * sizeof(double));
            for (int i = 0; i < 16; i++)
            {
                double a = 0;
                for (int k = 0; k < 8; k++)
                    a += cos((k + 0.5) * (i + 4) * 3.14159265358979323846 / 8) * s[ch][k];
                v[i] = a;
            }
            double u[80], x[8];
            for (int i = 0; i < 5; i++)
                for (int j = 0; j < 8; j++)
                {
                    u[16 * i + j] = v[32 * i + j];
                    u[16 * i + 8 + j] = v[32 * i + 24 + j];
                }
            for (int j = 0; j < 8; j++)
            {
                double a = 0;
                for (int i = 0; i < 10; i++)
                    a -= u[j + 8 * i] * 8.0 * REF_C[j + 8 * i]; /* the synthesis window is -8 C[i] in this form of the matrixing (checked against libsbc) */
                x[j] = a;
            }
            for (int j = 0; j < 8; j++)
            {
                long v2 = lrint(x[j]);
                out[(blk * 8 + j) * 2 + ch] = (short)(v2 > 32767 ? 32767 : v2 < -32768 ? -32768 : v2);
            }
        }
    }
    if (info)
        *info = r;
    return (int)((pos + 7) / 8);
}

#endif
