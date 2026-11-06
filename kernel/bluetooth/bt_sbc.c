/* SBC encoder (stage 8). See include/bt_sbc.h for what the codec does.
 *
 * The 80 numbers of the prototype window of the filter bank are fixed by the A2DP specification (appendix B, "analysis window"). They are taken
 * here from libsbc, the SBC library of BlueZ (https://git.kernel.org/pub/scm/bluetooth/sbc.git, LGPL-2.1-or-later, Copyright (C) 2004-2010
 * Marcel Holtmann, Henryk Ploetz, Brad Midgley, Nokia Corporation): the decimal constants of analysis_consts_fixed8_simd_even in sbc_tables.h,
 * put in the order of the window by measuring libsbc's own analysis, and kept here as integers (times 2^28). The loudness offsets are the table
 * of the same appendix. Everything else (the filter bank, scale factors, bit allocation, quantizing and packing) is written from the
 * specification; the result was compared with libsbc, see tests/test_bt_sbc.c. */
#include "bt_sbc.h"

/* prototype window C[0 .. 79] x 2^28; C[40] = 0.1469550680 is the middle. C[i] for i = 12, 28, 44, 60, 76 never reaches the output of the
 * analysis (their cosine factor is 0) and is given by the symmetry of the window. */
static const int WIN[80] = {
    0, 42030, 92142, 148880, 221169, 305996, 396319, 478813,
    540045, 564713, 535407, 433943, 242170, -47998, -442846, -938766,
    1519209, 2155379, 2807417, 3421809, 3933258, 4269348, 4354250, 4112005,
    3472798, 2377687, 784928, -1319570, -3930004, -7008816, -10489153, -14277358,
    18253327, 22276051, 26192695, 29849134, 33088575, 35772894, 37783231, 39027790,
    39447951, 39027790, 37783231, 35772894, 33088575, 29849134, 26192695, 22276051,
    -18253327, -14277358, -10489153, -7008816, -3930004, -1319570, 784928, 2377687,
    3472798, 4112005, 4354250, 4269348, 3933258, 3421809, 2807417, 2155379,
    -1519209, -938766, -442846, -47998, 242170, 433943, 535407, 564713,
    540045, 478813, 396319, 305996, 221169, 148880, 92142, 42030,
};

/* cos(n * pi / 16) x 2^15 for n = 0 .. 16: the matrixing factor M[k][i] = cos((2k + 1)(i - 4) pi / 16) is one of these */
static const int COS16[17] = {32768, 32138, 30274, 27246, 23170, 18205, 12540, 6393, 0, -6393, -12540, -18205, -23170, -27246, -30274, -32138, -32768};

/* bit allocation offsets for loudness, 8 subbands, per sampling frequency (16, 32, 44.1, 48 kHz) */
static const signed char OFFSET8[4][8] = {
    {-2, 0, 0, 0, 0, 0, 0, 1},
    {-3, 0, 0, 0, 0, 0, 1, 2},
    {-4, 0, 0, 0, 0, 0, 1, 2},
    {-4, 0, 0, 0, 0, 0, 1, 2},
};

#define BANDS 8
#define SAMPLE_SHIFT 28 /* window (2^28) x cosine (2^15) / 2^15: the subband samples come out as PCM value x 2^15 */

static int matrix(int k, int i)
{
    int a = ((2 * k + 1) * (i - 4)) % 32;
    if (a < 0)
        a += 32;
    if (a > 16)
        a = 32 - a;
    return COS16[a];
}

int bt_sbc_init(BtSbcEnc *e, const BtSbcConfig *cfg)
{
    e->freq_code = cfg->freq == 1 ? 3 : cfg->freq == 2 ? 2 : cfg->freq == 4 ? 1 : cfg->freq == 8 ? 0 : -1;
    e->blocks = cfg->blocks == 1 ? 16 : cfg->blocks == 2 ? 12 : cfg->blocks == 4 ? 8 : cfg->blocks == 8 ? 4 : 0;
    e->mode_code = cfg->mode == 1 ? 3 : cfg->mode == 2 ? 2 : -1;
    e->alloc_code = cfg->alloc == 1 ? 0 : cfg->alloc == 2 ? 1 : -1;
    e->bitpool = (int)cfg->bitpool;
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < 80; i++)
            e->hist[c][i] = 0;
    if (e->freq_code < 0 || !e->blocks || e->mode_code < 0 || e->alloc_code < 0 || cfg->bands != 1 || e->bitpool < 2 || e->bitpool > 250)
        return -1;
    if (bt_sbc_frame_bytes(e) > BT_SBC_MAX_FRAME)
        return -1;
    return 0;
}

unsigned bt_sbc_samples(const BtSbcEnc *e) { return (unsigned)(e->blocks * BANDS); }

unsigned bt_sbc_frame_bytes(const BtSbcEnc *e)
{
    unsigned bits = (unsigned)(e->blocks * e->bitpool) + (e->mode_code == 3 ? BANDS : 0);
    return 4 + (4 * BANDS * 2) / 8 + (bits + 7) / 8;
}

/* One block of one channel: 8 new samples (oldest first) in, one sample per subband out (PCM value x 2^15). */
static void analyze(short *x, const short *in, int stride, int *out)
{
    for (int i = 79; i >= 8; i--)
        x[i] = x[i - 8];
    for (int n = 0; n < 8; n++)
        x[7 - n] = in[n * stride]; /* the oldest of the new samples is X[7], the newest X[0] */
    long long y[16];
    for (int i = 0; i < 16; i++)
    {
        long long a = 0;
        for (int j = 0; j < 5; j++)
            a += (long long)WIN[i + 16 * j] * x[i + 16 * j];
        y[i] = a;
    }
    for (int k = 0; k < BANDS; k++)
    {
        long long s = 0;
        for (int i = 0; i < 16; i++)
            s += (long long)matrix(k, i) * y[i];
        out[k] = (int)((s + (1LL << (SAMPLE_SHIFT - 1))) >> SAMPLE_SHIFT);
    }
}

/* The scale factor of a set of samples: the smallest sf >= 0 with |sample| <= 2^(sf + 15) (samples are PCM x 2^15, so sf = 15 is full scale). */
static int scale_factor(const int *s, int n)
{
    unsigned acc = 1u << 15;
    for (int i = 0; i < n; i++)
    {
        int v = s[i] < 0 ? -s[i] : s[i];
        if (v)
            acc |= (unsigned)(v - 1);
    }
    return (31 - __builtin_clz(acc)) - 15;
}

/* Bit allocation (the specification's procedure): how many bits each sample of each subband gets, adding up to the bitpool. */
void bt_sbc_allocate(const BtSbcEnc *e, const int sf[2][BANDS], int bits[2][BANDS])
{
    int need[2][BANDS], max_need = 0;
    for (int ch = 0; ch < 2; ch++)
        for (int sb = 0; sb < BANDS; sb++)
        {
            int v;
            if (e->alloc_code == 1) /* SNR: as many bits as the scale factor says */
                v = sf[ch][sb];
            else if (sf[ch][sb] == 0)
                v = -5;
            else
            {
                int loud = sf[ch][sb] - OFFSET8[e->freq_code][sb];
                v = loud > 0 ? loud / 2 : loud;
            }
            need[ch][sb] = v;
            if (v > max_need)
                max_need = v;
        }
    /* find the "slice": the level above which bands get bits, so that the total just fits the bitpool */
    int count = 0, slices = 0, slice = max_need + 1;
    do
    {
        slice--;
        count += slices;
        slices = 0;
        for (int ch = 0; ch < 2; ch++)
            for (int sb = 0; sb < BANDS; sb++)
            {
                if (need[ch][sb] > slice + 1 && need[ch][sb] < slice + 16)
                    slices++;
                else if (need[ch][sb] == slice + 1)
                    slices += 2;
            }
    } while (count + slices < e->bitpool);
    if (count + slices == e->bitpool)
    {
        count += slices;
        slice--;
    }
    for (int ch = 0; ch < 2; ch++)
        for (int sb = 0; sb < BANDS; sb++)
        {
            if (need[ch][sb] < slice + 2)
                bits[ch][sb] = 0;
            else
            {
                int b = need[ch][sb] - slice;
                bits[ch][sb] = b > 16 ? 16 : b;
            }
        }
    /* what is left of the bitpool goes to the bands in order (low bands first, left before right): first one more bit where there
     * are already two to 15, or two bits to a band that just misses ... */
    for (int sb = 0; sb < BANDS && count < e->bitpool; sb++)
        for (int ch = 0; ch < 2 && count < e->bitpool; ch++)
        {
            if (bits[ch][sb] >= 2 && bits[ch][sb] < 16)
            {
                bits[ch][sb]++;
                count++;
            }
            else if (need[ch][sb] == slice + 1 && e->bitpool > count + 1)
            {
                bits[ch][sb] = 2;
                count += 2;
            }
        }
    /* ... then one more bit wherever there is room */
    for (int sb = 0; sb < BANDS && count < e->bitpool; sb++)
        for (int ch = 0; ch < 2 && count < e->bitpool; ch++)
            if (bits[ch][sb] < 16)
            {
                bits[ch][sb]++;
                count++;
            }
}

typedef struct
{
    unsigned char *p;
    unsigned cap, n;
    unsigned long long acc;
    int have; /* bits in acc */
    int over;
} BitWriter;

static void put_bits(BitWriter *w, unsigned v, int n)
{
    w->acc = (w->acc << n) | (v & ((1u << n) - 1));
    w->have += n;
    while (w->have >= 8)
    {
        w->have -= 8;
        if (w->n < w->cap)
            w->p[w->n++] = (unsigned char)(w->acc >> w->have);
        else
            w->over = 1;
    }
}

static void flush_bits(BitWriter *w)
{
    if (w->have)
        put_bits(w, 0, 8 - w->have);
}

/* CRC-8 (polynomial x^8 + x^4 + x^3 + x^2 + 1, start value 0x0F) over a stream of bits */
static unsigned crc_bit(unsigned crc, unsigned bit)
{
    unsigned top = ((crc >> 7) & 1) ^ (bit & 1);
    crc = (crc << 1) & 0xFF;
    if (top)
        crc ^= 0x1D;
    return crc;
}

static unsigned crc_bits(unsigned crc, unsigned v, int n)
{
    for (int i = n - 1; i >= 0; i--)
        crc = crc_bit(crc, (v >> i) & 1);
    return crc;
}

int bt_sbc_encode(BtSbcEnc *e, const short *pcm, unsigned char *out, unsigned cap)
{
    unsigned flen = bt_sbc_frame_bytes(e);
    if (cap < flen)
        return -1;
    static int sb[16][2][BANDS]; /* PCM x 2^15; one encoder at a time (the kernel is single threaded here) */
    int blocks = e->blocks;
    for (int b = 0; b < blocks; b++)
        for (int ch = 0; ch < 2; ch++)
            analyze(e->hist[ch], pcm + (b * 8) * 2 + ch, 2, sb[b][ch]);

    int sf[2][BANDS], joint = 0;
    for (int sbd = 0; sbd < BANDS; sbd++)
    {
        int l[16], r[16];
        for (int b = 0; b < blocks; b++)
        {
            l[b] = sb[b][0][sbd];
            r[b] = sb[b][1][sbd];
        }
        sf[0][sbd] = scale_factor(l, blocks);
        sf[1][sbd] = scale_factor(r, blocks);
        if (e->mode_code == 3 && sbd < BANDS - 1) /* the last band is never joint */
        {
            int m[16], s[16];
            for (int b = 0; b < blocks; b++)
            {
                m[b] = (l[b] >> 1) + (r[b] >> 1);
                s[b] = (l[b] >> 1) - (r[b] >> 1);
            }
            int sm = scale_factor(m, blocks), ss = scale_factor(s, blocks);
            if (sf[0][sbd] + sf[1][sbd] > sm + ss) /* sum and difference need fewer bits than left and right */
            {
                joint |= 1 << (BANDS - 1 - sbd);
                sf[0][sbd] = sm;
                sf[1][sbd] = ss;
                for (int b = 0; b < blocks; b++)
                {
                    sb[b][0][sbd] = m[b];
                    sb[b][1][sbd] = s[b];
                }
            }
        }
    }

    unsigned char hdr1 = (unsigned char)(((e->freq_code & 3) << 6) | (((blocks / 4 - 1) & 3) << 4) | ((e->mode_code & 3) << 2) | ((e->alloc_code & 1) << 1) | 1);
    /* block length code: 4 -> 0, 8 -> 1, 12 -> 2, 16 -> 3; the last bit says 8 subbands */
    hdr1 = (unsigned char)((hdr1 & ~0x30) | ((((blocks / 4) - 1) & 3) << 4));
    BitWriter w = {out, cap, 0, 0, 0, 0};
    out[0] = 0x9C;
    out[1] = hdr1;
    out[2] = (unsigned char)e->bitpool;
    w.n = 4; /* the CRC byte is filled in at the end */
    unsigned crc = 0x0F;
    crc = crc_bits(crc, hdr1, 8);
    crc = crc_bits(crc, (unsigned)e->bitpool, 8);
    if (e->mode_code == 3)
    {
        put_bits(&w, (unsigned)joint, BANDS);
        crc = crc_bits(crc, (unsigned)joint, BANDS);
    }
    for (int ch = 0; ch < 2; ch++)
        for (int sbd = 0; sbd < BANDS; sbd++)
        {
            put_bits(&w, (unsigned)sf[ch][sbd], 4);
            crc = crc_bits(crc, (unsigned)sf[ch][sbd], 4);
        }
    out[3] = (unsigned char)crc;

    int bits[2][BANDS];
    bt_sbc_allocate(e, sf, bits);
    for (int b = 0; b < blocks; b++)
        for (int ch = 0; ch < 2; ch++)
            for (int sbd = 0; sbd < BANDS; sbd++)
            {
                int nb = bits[ch][sbd];
                if (!nb)
                    continue;
                /* q = floor((2^nb - 1) * (1/2 + s / 2^(sf + 17))), s in PCM x 2^15 */
                long long levels = (1LL << nb) - 1;
                long long q = (levels * ((1LL << (sf[ch][sbd] + 16)) + sb[b][ch][sbd])) >> (sf[ch][sbd] + 17);
                if (q < 0)
                    q = 0;
                if (q > levels)
                    q = levels;
                put_bits(&w, (unsigned)q, nb);
            }
    flush_bits(&w);
    if (w.over || w.n != flen)
        return -1;
    return (int)w.n;
}
