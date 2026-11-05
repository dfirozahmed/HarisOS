/* HarisOS MP3 decoder (Layer III). See mp3dec.h for the overview and the interface.
 *
 * The order of the steps, the shape of the Huffman code books and of the synthesis window follow minimp3 (public domain,
 * https://github.com/lieff/minimp3), whose tables are in mp3tab.h; this file is the plain scalar C version, MPEG audio Layer III only. */
#include "mp3dec.h"

typedef unsigned char mp3_u8;
typedef short mp3_s16;
typedef unsigned short mp3_u16;
typedef unsigned int mp3_u32;
typedef int mp3_i32;

#include "mp3tab.h"

#define SHORT_BLOCK_TYPE 2
#define STOP_BLOCK_TYPE 3
#define HDR_SIZE 4

/* header fields: h = the 4 header bytes */
#define HDR_IS_MONO(h) (((h[3]) & 0xC0) == 0xC0)
#define HDR_IS_MS_STEREO(h) (((h[3]) & 0xE0) == 0x60)
#define HDR_IS_CRC(h) (!((h[1]) & 1))
#define HDR_TEST_PADDING(h) ((h[2]) & 0x2)
#define HDR_TEST_MPEG1(h) ((h[1]) & 0x8)
#define HDR_TEST_NOT_MPEG25(h) ((h[1]) & 0x10)
#define HDR_TEST_I_STEREO(h) ((h[3]) & 0x10)
#define HDR_TEST_MS_STEREO(h) ((h[3]) & 0x20)
#define HDR_GET_LAYER(h) (((h[1]) >> 1) & 3)
#define HDR_GET_BITRATE(h) ((h[2]) >> 4)
#define HDR_GET_SAMPLE_RATE(h) (((h[2]) >> 2) & 3)
#define HDR_GET_MY_SAMPLE_RATE(h) (HDR_GET_SAMPLE_RATE(h) + (((h[1] >> 3) & 1) + ((h[1] >> 4) & 1)) * 3)
#define HDR_IS_FRAME_576(h) ((h[1] & 14) == 2)
#define HDR_IS_LAYER_1(h) ((h[1] & 6) == 6)

#define BITS_DEQUANTIZER_OUT -1
#define MAX_SCF (255 + BITS_DEQUANTIZER_OUT * 4 - 210)
#define MAX_SCFI ((MAX_SCF + 3) & ~3)
#define MP3_MIN(a, b) ((a) > (b) ? (b) : (a))
#define MP3_MAX(a, b) ((a) < (b) ? (b) : (a))
#define MAX_FRAME_SYNC_MATCHES 10

/* no libc here: plain loops (the user programs are built with -fno-builtin, the host test does not care) */
static void mp3_copy(void *dst, const void *src, int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

static void mp3_move_down(void *dst, const void *src, int n) /* dst below src, may overlap */
{
    mp3_copy(dst, src, n);
}

static void mp3_zero(void *p, int n)
{
    unsigned char *b = (unsigned char *)p;
    for (int i = 0; i < n; i++)
        b[i] = 0;
}

/* ---- bit reader: n <= 32 bits, most significant first ---- */
static void bs_init(Mp3Bits *bs, const unsigned char *data, int bytes)
{
    bs->buf = data;
    bs->pos = 0;
    bs->limit = bytes * 8;
}

static mp3_u32 get_bits(Mp3Bits *bs, int n)
{
    mp3_u32 next, cache = 0, s = (mp3_u32)(bs->pos & 7);
    int shl = n + (int)s;
    const unsigned char *p = bs->buf + (bs->pos >> 3);
    if ((bs->pos += n) > bs->limit)
        return 0;
    next = *p++ & (255u >> s);
    while ((shl -= 8) > 0)
    {
        cache |= next << shl;
        next = *p++;
    }
    return cache | (next >> -shl);
}

/* ---- frame header ---- */
static int hdr_valid(const unsigned char *h)
{
    return h[0] == 0xff && ((h[1] & 0xF0) == 0xf0 || (h[1] & 0xFE) == 0xe2) && HDR_GET_LAYER(h) != 0 && HDR_GET_BITRATE(h) != 15 &&
           HDR_GET_BITRATE(h) != 0 /* free format is not read */ && HDR_GET_SAMPLE_RATE(h) != 3;
}

/* the same stream: same version / layer / sample rate */
static int hdr_compare(const unsigned char *h1, const unsigned char *h2)
{
    return hdr_valid(h2) && ((h1[1] ^ h2[1]) & 0xFE) == 0 && ((h1[2] ^ h2[2]) & 0x0C) == 0;
}

static unsigned hdr_bitrate_kbps(const unsigned char *h)
{
    return 2u * halfrate[!!HDR_TEST_MPEG1(h)][HDR_GET_LAYER(h) - 1][HDR_GET_BITRATE(h)];
}

static unsigned hdr_sample_rate_hz(const unsigned char *h)
{
    return g_hz[HDR_GET_SAMPLE_RATE(h)] >> (int)!HDR_TEST_MPEG1(h) >> (int)!HDR_TEST_NOT_MPEG25(h);
}

static unsigned hdr_frame_samples(const unsigned char *h) { return HDR_IS_LAYER_1(h) ? 384 : (1152 >> (int)HDR_IS_FRAME_576(h)); }

static int hdr_frame_bytes(const unsigned char *h)
{
    int bytes = (int)(hdr_frame_samples(h) * hdr_bitrate_kbps(h) * 125 / hdr_sample_rate_hz(h));
    if (HDR_IS_LAYER_1(h))
        bytes &= ~3; /* slot aligned */
    return bytes;
}

static int hdr_padding(const unsigned char *h) { return HDR_TEST_PADDING(h) ? (HDR_IS_LAYER_1(h) ? 4 : 1) : 0; }

/* ---- side information: where the data of each granule is and how it is coded ---- */
static int L3_read_side_info(Mp3Bits *bs, Mp3Gr *gr, const unsigned char *hdr)
{
    unsigned tables, scfsi = 0;
    int main_data_begin, part_23_sum = 0;
    int sr_idx = (int)HDR_GET_MY_SAMPLE_RATE(hdr);
    sr_idx -= (sr_idx != 0);
    int gr_count = HDR_IS_MONO(hdr) ? 1 : 2;

    if (HDR_TEST_MPEG1(hdr))
    {
        gr_count *= 2;
        main_data_begin = (int)get_bits(bs, 9);
        scfsi = get_bits(bs, 7 + gr_count);
    }
    else
        main_data_begin = (int)(get_bits(bs, 8 + gr_count) >> gr_count);

    do
    {
        if (HDR_IS_MONO(hdr))
            scfsi <<= 4;
        gr->part_23_length = (mp3_u16)get_bits(bs, 12);
        part_23_sum += gr->part_23_length;
        gr->big_values = (mp3_u16)get_bits(bs, 9);
        if (gr->big_values > 288)
            return -1;
        gr->global_gain = (mp3_u8)get_bits(bs, 8);
        gr->scalefac_compress = (mp3_u16)get_bits(bs, HDR_TEST_MPEG1(hdr) ? 4 : 9);
        gr->sfbtab = g_scf_long[sr_idx];
        gr->n_long_sfb = 22;
        gr->n_short_sfb = 0;
        if (get_bits(bs, 1)) /* window switching: a block type other than "normal" */
        {
            gr->block_type = (mp3_u8)get_bits(bs, 2);
            if (!gr->block_type)
                return -1;
            gr->mixed_block_flag = (mp3_u8)get_bits(bs, 1);
            gr->region_count[0] = 7;
            gr->region_count[1] = 255;
            if (gr->block_type == SHORT_BLOCK_TYPE)
            {
                scfsi &= 0x0F0F;
                if (!gr->mixed_block_flag)
                {
                    gr->region_count[0] = 8;
                    gr->sfbtab = g_scf_short[sr_idx];
                    gr->n_long_sfb = 0;
                    gr->n_short_sfb = 39;
                }
                else
                {
                    gr->sfbtab = g_scf_mixed[sr_idx];
                    gr->n_long_sfb = HDR_TEST_MPEG1(hdr) ? 8 : 6;
                    gr->n_short_sfb = 30;
                }
            }
            tables = get_bits(bs, 10);
            tables <<= 5;
            gr->subblock_gain[0] = (mp3_u8)get_bits(bs, 3);
            gr->subblock_gain[1] = (mp3_u8)get_bits(bs, 3);
            gr->subblock_gain[2] = (mp3_u8)get_bits(bs, 3);
        }
        else
        {
            gr->block_type = 0;
            gr->mixed_block_flag = 0;
            tables = get_bits(bs, 15);
            gr->region_count[0] = (mp3_u8)get_bits(bs, 4);
            gr->region_count[1] = (mp3_u8)get_bits(bs, 3);
            gr->region_count[2] = 255;
        }
        gr->table_select[0] = (mp3_u8)(tables >> 10);
        gr->table_select[1] = (mp3_u8)((tables >> 5) & 31);
        gr->table_select[2] = (mp3_u8)(tables & 31);
        gr->preflag = (mp3_u8)(HDR_TEST_MPEG1(hdr) ? get_bits(bs, 1) : (gr->scalefac_compress >= 500));
        gr->scalefac_scale = (mp3_u8)get_bits(bs, 1);
        gr->count1_table = (mp3_u8)get_bits(bs, 1);
        gr->scfsi = (mp3_u8)((scfsi >> 12) & 15);
        scfsi <<= 4;
        gr++;
    } while (--gr_count);

    if (part_23_sum + bs->pos > bs->limit + main_data_begin * 8)
        return -1;
    return main_data_begin;
}

/* ---- scale factors ---- */
static void L3_read_scalefactors(mp3_u8 *scf, mp3_u8 *ist_pos, const mp3_u8 *scf_size, const mp3_u8 *scf_count, Mp3Bits *bitbuf, int scfsi)
{
    int i, k;
    for (i = 0; i < 4 && scf_count[i]; i++, scfsi *= 2)
    {
        int cnt = scf_count[i];
        if (scfsi & 8) /* the same scale factors as in the first granule */
            mp3_copy(scf, ist_pos, cnt);
        else
        {
            int bits = scf_size[i];
            if (!bits)
            {
                mp3_zero(scf, cnt);
                mp3_zero(ist_pos, cnt);
            }
            else
            {
                int max_scf = (scfsi < 0) ? (1 << bits) - 1 : -1; /* MPEG-2 intensity stereo: the all-ones value means "no position" */
                for (k = 0; k < cnt; k++)
                {
                    int s = (int)get_bits(bitbuf, bits);
                    ist_pos[k] = (mp3_u8)(s == max_scf ? -1 : s);
                    scf[k] = (mp3_u8)s;
                }
            }
        }
        ist_pos += cnt;
        scf += cnt;
    }
    scf[0] = scf[1] = scf[2] = 0;
}

/* y * 2^(-exp_q2 / 4), by repeated multiplication (no libm) */
static float L3_ldexp_q2(float y, int exp_q2)
{
    int e;
    do
    {
        e = MP3_MIN(30 * 4, exp_q2);
        y *= g_expfrac[e & 3] * (float)(1 << 30 >> (e >> 2));
    } while ((exp_q2 -= e) > 0);
    return y;
}

static void L3_decode_scalefactors(const unsigned char *hdr, mp3_u8 *ist_pos, Mp3Bits *bs, const Mp3Gr *gr, float *scf, int ch)
{
    const mp3_u8 *scf_partition = g_scf_partitions[!!gr->n_short_sfb + !gr->n_long_sfb];
    mp3_u8 scf_size[4], iscf[40];
    int i, scf_shift = gr->scalefac_scale + 1, gain_exp, scfsi = gr->scfsi;
    float gain;

    if (HDR_TEST_MPEG1(hdr))
    {
        int part = g_scfc_decode[gr->scalefac_compress];
        scf_size[1] = scf_size[0] = (mp3_u8)(part >> 2);
        scf_size[3] = scf_size[2] = (mp3_u8)(part & 3);
    }
    else
    {
        int k, modprod, sfc, ist = HDR_TEST_I_STEREO(hdr) && ch;
        sfc = gr->scalefac_compress >> ist;
        for (k = ist * 3 * 4; sfc >= 0; sfc -= modprod, k += 4)
        {
            for (modprod = 1, i = 3; i >= 0; i--)
            {
                scf_size[i] = (mp3_u8)(sfc / modprod % g_mod[k + i]);
                modprod *= g_mod[k + i];
            }
        }
        scf_partition += k;
        scfsi = -16;
    }
    L3_read_scalefactors(iscf, ist_pos, scf_size, scf_partition, bs, scfsi);

    if (gr->n_short_sfb)
    {
        int sh = 3 - scf_shift;
        for (i = 0; i < gr->n_short_sfb; i += 3)
        {
            iscf[gr->n_long_sfb + i + 0] = (mp3_u8)(iscf[gr->n_long_sfb + i + 0] + (gr->subblock_gain[0] << sh));
            iscf[gr->n_long_sfb + i + 1] = (mp3_u8)(iscf[gr->n_long_sfb + i + 1] + (gr->subblock_gain[1] << sh));
            iscf[gr->n_long_sfb + i + 2] = (mp3_u8)(iscf[gr->n_long_sfb + i + 2] + (gr->subblock_gain[2] << sh));
        }
    }
    else if (gr->preflag)
    {
        for (i = 0; i < 10; i++)
            iscf[11 + i] = (mp3_u8)(iscf[11 + i] + g_preamp[i]);
    }

    gain_exp = gr->global_gain + BITS_DEQUANTIZER_OUT * 4 - 210 - (HDR_IS_MS_STEREO(hdr) ? 2 : 0);
    gain = L3_ldexp_q2((float)(1 << (MAX_SCFI / 4)), MAX_SCFI - gain_exp);
    for (i = 0; i < (int)(gr->n_long_sfb + gr->n_short_sfb); i++)
        scf[i] = L3_ldexp_q2(gain, iscf[i] << scf_shift);
}

/* ---- Huffman decoding and requantization ---- */

/* x^(4/3) for x up to 8191: a table for small x, a Taylor step around the nearest table entry for the rest */
static float L3_pow_43(int x)
{
    float frac;
    int sign, mult = 256;

    if (x < 129)
        return g_pow43[16 + x];
    if (x < 1024)
    {
        mult = 16;
        x <<= 3;
    }
    sign = 2 * x & 64;
    frac = (float)((x & 63) - sign) / (float)((x & ~63) + sign);
    return g_pow43[16 + ((x + sign) >> 6)] * (1.f + frac * ((4.f / 3) + frac * (2.f / 9))) * (float)mult;
}

/* Reads the 576 spectral lines of one channel of one granule into dst. The code books in tabs[] are small binary trees packed into
 * 16 bit numbers: a non-negative leaf holds the decoded pair (two 4 bit values) and the number of code bits; a negative number
 * says "continue with w more bits at this offset". bs_cache holds the next bits of the stream, left aligned. */
static void L3_huffman(float *dst, Mp3Bits *bs, const Mp3Gr *gr_info, const float *scf, int layer3gr_limit)
{
#define PEEK_BITS(n) (bs_cache >> (32 - n))
#define FLUSH_BITS(n) \
    {                 \
        bs_cache <<= (n); \
        bs_sh += (n); \
    }
#define CHECK_BITS                                       \
    while (bs_sh >= 0)                                   \
    {                                                    \
        bs_cache |= (mp3_u32)*bs_next_ptr++ << bs_sh;    \
        bs_sh -= 8;                                      \
    }
#define BSPOS ((bs_next_ptr - bs->buf) * 8 - 24 + bs_sh)

    float one = 0.0f;
    int ireg = 0, big_val_cnt = gr_info->big_values;
    const mp3_u8 *sfb = gr_info->sfbtab;
    const mp3_u8 *bs_next_ptr = bs->buf + bs->pos / 8;
    mp3_u32 bs_cache = (((bs_next_ptr[0] * 256u + bs_next_ptr[1]) * 256u + bs_next_ptr[2]) * 256u + bs_next_ptr[3]) << (bs->pos & 7);
    int pairs_to_decode, np, bs_sh = (bs->pos & 7) - 8;
    bs_next_ptr += 4;

    while (big_val_cnt > 0)
    {
        int tab_num = gr_info->table_select[ireg];
        int sfb_cnt = gr_info->region_count[ireg++];
        const mp3_s16 *codebook = tabs + tabindex[tab_num];
        int linbits = g_linbits[tab_num];
        do
        {
            np = *sfb++ / 2;
            pairs_to_decode = MP3_MIN(big_val_cnt, np);
            one = *scf++;
            do
            {
                int j, w = 5;
                int leaf = codebook[PEEK_BITS(w)];
                while (leaf < 0)
                {
                    FLUSH_BITS(w);
                    w = leaf & 7;
                    leaf = codebook[PEEK_BITS(w) - (leaf >> 3)];
                }
                FLUSH_BITS(leaf >> 8);

                for (j = 0; j < 2; j++, dst++, leaf >>= 4)
                {
                    int lsb = leaf & 0x0F;
                    if (linbits && lsb == 15) /* escape: the rest of the magnitude follows in `linbits` extra bits */
                    {
                        lsb += (int)PEEK_BITS(linbits);
                        FLUSH_BITS(linbits);
                        CHECK_BITS;
                        *dst = one * L3_pow_43(lsb) * ((mp3_i32)bs_cache < 0 ? -1.f : 1.f);
                    }
                    else
                        *dst = g_pow43[16 + lsb - 16 * (int)(bs_cache >> 31)] * one; /* the sign bit picks the negative half of the table */
                    FLUSH_BITS(lsb ? 1 : 0);
                }
                CHECK_BITS;
            } while (--pairs_to_decode);
        } while ((big_val_cnt -= np) > 0 && --sfb_cnt >= 0);
    }

    /* the "count1" region: groups of four values that are each 0 or +-1 */
    for (np = 1 - big_val_cnt;; dst += 4)
    {
        const mp3_u8 *codebook_count1 = (gr_info->count1_table) ? tab33 : tab32;
        int leaf = codebook_count1[PEEK_BITS(4)];
        if (!(leaf & 8))
            leaf = codebook_count1[(leaf >> 3) + (int)(bs_cache << 4 >> (32 - (leaf & 3)))];
        FLUSH_BITS(leaf & 7);
        if (BSPOS > layer3gr_limit)
            break;
#define RELOAD_SCALEFACTOR \
    if (!--np)             \
    {                      \
        np = *sfb++ / 2;   \
        if (!np)           \
            break;         \
        one = *scf++;      \
    }
#define DEQ_COUNT1(s)                                          \
    if (leaf & (128 >> s))                                     \
    {                                                          \
        dst[s] = ((mp3_i32)bs_cache < 0) ? -one : one;         \
        FLUSH_BITS(1)                                          \
    }
        RELOAD_SCALEFACTOR;
        DEQ_COUNT1(0);
        DEQ_COUNT1(1);
        RELOAD_SCALEFACTOR;
        DEQ_COUNT1(2);
        DEQ_COUNT1(3);
        CHECK_BITS;
    }
    bs->pos = layer3gr_limit;
#undef PEEK_BITS
#undef FLUSH_BITS
#undef CHECK_BITS
#undef BSPOS
#undef RELOAD_SCALEFACTOR
#undef DEQ_COUNT1
}

/* ---- joint stereo ---- */
static void L3_midside_stereo(float *left, int n)
{
    float *right = left + 576;
    for (int i = 0; i < n; i++)
    {
        float a = left[i], b = right[i];
        left[i] = a + b;
        right[i] = a - b;
    }
}

static void L3_intensity_stereo_band(float *left, int n, float kl, float kr)
{
    for (int i = 0; i < n; i++)
    {
        left[i + 576] = left[i] * kr;
        left[i] = left[i] * kl;
    }
}

/* The highest band of the right channel that has a non-zero line: above it intensity stereo applies. */
static void L3_stereo_top_band(const float *right, const mp3_u8 *sfb, int nbands, int max_band[3])
{
    max_band[0] = max_band[1] = max_band[2] = -1;
    for (int i = 0; i < nbands; i++)
    {
        for (int k = 0; k < sfb[i]; k += 2)
            if (right[k] != 0 || right[k + 1] != 0)
            {
                max_band[i % 3] = i;
                break;
            }
        right += sfb[i];
    }
}

static void L3_stereo_process(float *left, const mp3_u8 *ist_pos, const mp3_u8 *sfb, const unsigned char *hdr, int max_band[3], int mpeg2_sh)
{
    unsigned i, max_pos = HDR_TEST_MPEG1(hdr) ? 7 : 64;
    for (i = 0; sfb[i]; i++)
    {
        unsigned ipos = ist_pos[i];
        if ((int)i > max_band[i % 3] && ipos < max_pos)
        {
            float kl, kr, s = HDR_TEST_MS_STEREO(hdr) ? 1.41421356f : 1;
            if (HDR_TEST_MPEG1(hdr))
            {
                kl = g_pan[2 * ipos];
                kr = g_pan[2 * ipos + 1];
            }
            else
            {
                kl = 1;
                kr = L3_ldexp_q2(1, (int)((ipos + 1) >> 1 << mpeg2_sh));
                if (ipos & 1)
                {
                    kl = kr;
                    kr = 1;
                }
            }
            L3_intensity_stereo_band(left, sfb[i], kl * s, kr * s);
        }
        else if (HDR_TEST_MS_STEREO(hdr))
            L3_midside_stereo(left, sfb[i]);
        left += sfb[i];
    }
}

static void L3_intensity_stereo(float *left, mp3_u8 *ist_pos, const Mp3Gr *gr, const unsigned char *hdr)
{
    int max_band[3], n_sfb = gr->n_long_sfb + gr->n_short_sfb;
    int i, max_blocks = gr->n_short_sfb ? 3 : 1;

    L3_stereo_top_band(left + 576, gr->sfbtab, n_sfb, max_band);
    if (gr->n_long_sfb)
        max_band[0] = max_band[1] = max_band[2] = MP3_MAX(MP3_MAX(max_band[0], max_band[1]), max_band[2]);
    for (i = 0; i < max_blocks; i++)
    {
        int default_pos = HDR_TEST_MPEG1(hdr) ? 3 : 0;
        int itop = n_sfb - max_blocks + i;
        int prev = itop - max_blocks;
        ist_pos[itop] = (mp3_u8)(max_band[i] >= prev ? default_pos : ist_pos[prev]);
    }
    L3_stereo_process(left, ist_pos, gr->sfbtab, hdr, max_band, gr[1].scalefac_compress & 1);
}

/* ---- short blocks: the three windows of a band are stored one after the other; the transform wants them interleaved ---- */
static void L3_reorder(float *grbuf, float *scratch, const mp3_u8 *sfb)
{
    int i, len;
    float *src = grbuf, *dst = scratch;
    for (; 0 != (len = *sfb); sfb += 3, src += 2 * len)
        for (i = 0; i < len; i++, src++)
        {
            *dst++ = src[0 * len];
            *dst++ = src[1 * len];
            *dst++ = src[2 * len];
        }
    mp3_copy(grbuf, scratch, (int)((dst - scratch) * (int)sizeof(float)));
}

/* ---- alias reduction between neighbouring subbands (8 butterflies at each boundary) ---- */
static void L3_antialias(float *grbuf, int nbands)
{
    for (; nbands > 0; nbands--, grbuf += 18)
        for (int i = 0; i < 8; i++)
        {
            float u = grbuf[18 + i];
            float d = grbuf[17 - i];
            grbuf[18 + i] = u * g_aa[0][i] - d * g_aa[1][i];
            grbuf[17 - i] = u * g_aa[1][i] + d * g_aa[0][i];
        }
}

/* ---- inverse MDCT ---- */
static void L3_dct3_9(float *y)
{
    float s0, s1, s2, s3, s4, s5, s6, s7, s8, t0, t2, t4;

    s0 = y[0]; s2 = y[2]; s4 = y[4]; s6 = y[6]; s8 = y[8];
    t0 = s0 + s6 * 0.5f;
    s0 -= s6;
    t4 = (s4 + s2) * 0.93969262f;
    t2 = (s8 + s2) * 0.76604444f;
    s6 = (s4 - s8) * 0.17364818f;
    s4 += s8 - s2;

    s2 = s0 - s4 * 0.5f;
    y[4] = s4 + s0;
    s8 = t0 - t2 + s6;
    s0 = t0 - t4 + t2;
    s4 = t0 + t4 - s6;

    s1 = y[1]; s3 = y[3]; s5 = y[5]; s7 = y[7];

    s3 *= 0.86602540f;
    t0 = (s5 + s1) * 0.98480775f;
    t4 = (s5 - s7) * 0.34202014f;
    t2 = (s1 + s7) * 0.64278761f;
    s1 = (s1 - s5 - s7) * 0.86602540f;

    s5 = t0 - s3 - t2;
    s7 = t4 - s3 - t0;
    s3 = t4 + s3 - t2;

    y[0] = s4 - s7;
    y[1] = s2 + s1;
    y[2] = s0 - s3;
    y[3] = s8 + s5;
    y[5] = s8 - s5;
    y[6] = s0 + s3;
    y[7] = s2 - s1;
    y[8] = s4 + s7;
}

/* long blocks: 18 lines -> 36 time values, the first 18 (plus the overlap of the previous granule) are output, the last 18 are kept */
static void L3_imdct36(float *grbuf, float *overlap, const float *window, int nbands)
{
    int i, j;
    for (j = 0; j < nbands; j++, grbuf += 18, overlap += 9)
    {
        float co[9], si[9];
        co[0] = -grbuf[0];
        si[0] = grbuf[17];
        for (i = 0; i < 4; i++)
        {
            si[8 - 2 * i] = grbuf[4 * i + 1] - grbuf[4 * i + 2];
            co[1 + 2 * i] = grbuf[4 * i + 1] + grbuf[4 * i + 2];
            si[7 - 2 * i] = grbuf[4 * i + 4] - grbuf[4 * i + 3];
            co[2 + 2 * i] = -(grbuf[4 * i + 3] + grbuf[4 * i + 4]);
        }
        L3_dct3_9(co);
        L3_dct3_9(si);

        si[1] = -si[1];
        si[3] = -si[3];
        si[5] = -si[5];
        si[7] = -si[7];

        for (i = 0; i < 9; i++)
        {
            float ovl = overlap[i];
            float sum = co[i] * g_twid9[9 + i] + si[i] * g_twid9[0 + i];
            overlap[i] = co[i] * g_twid9[0 + i] - si[i] * g_twid9[9 + i];
            grbuf[i] = ovl * window[0 + i] - sum * window[9 + i];
            grbuf[17 - i] = ovl * window[9 + i] + sum * window[0 + i];
        }
    }
}

static void L3_idct3(float x0, float x1, float x2, float *dst)
{
    float m1 = x1 * 0.86602540f;
    float a1 = x0 - x2 * 0.5f;
    dst[1] = x0 + x2;
    dst[0] = a1 + m1;
    dst[2] = a1 - m1;
}

static void L3_imdct12(float *x, float *dst, float *overlap)
{
    float co[3], si[3];
    int i;

    L3_idct3(-x[0], x[6] + x[3], x[12] + x[9], co);
    L3_idct3(x[15], x[12] - x[9], x[6] - x[3], si);
    si[1] = -si[1];

    for (i = 0; i < 3; i++)
    {
        float ovl = overlap[i];
        float sum = co[i] * g_twid3[3 + i] + si[i] * g_twid3[0 + i];
        overlap[i] = co[i] * g_twid3[0 + i] - si[i] * g_twid3[3 + i];
        dst[i] = ovl * g_twid3[2 - i] - sum * g_twid3[5 - i];
        dst[5 - i] = ovl * g_twid3[5 - i] + sum * g_twid3[2 - i];
    }
}

/* short blocks: three 12 point transforms per subband, each shifted by 6 samples against the previous one */
static void L3_imdct_short(float *grbuf, float *overlap, int nbands)
{
    for (; nbands > 0; nbands--, overlap += 9, grbuf += 18)
    {
        float tmp[18];
        mp3_copy(tmp, grbuf, (int)sizeof tmp);
        mp3_copy(grbuf, overlap, 6 * (int)sizeof(float));
        L3_imdct12(tmp, grbuf + 6, overlap + 6);
        L3_imdct12(tmp + 1, grbuf + 12, overlap + 6);
        L3_imdct12(tmp + 2, overlap, overlap + 6);
    }
}

/* every second sample of every second subband is negated (frequency inversion of the polyphase filter bank) */
static void L3_change_sign(float *grbuf)
{
    int b, i;
    for (b = 0, grbuf += 18; b < 32; b += 2, grbuf += 36)
        for (i = 1; i < 18; i += 2)
            grbuf[i] = -grbuf[i];
}

static void L3_imdct_gr(float *grbuf, float *overlap, unsigned block_type, unsigned n_long_bands)
{
    if (n_long_bands)
    {
        L3_imdct36(grbuf, overlap, g_mdct_window[0], (int)n_long_bands);
        grbuf += 18 * n_long_bands;
        overlap += 9 * n_long_bands;
    }
    if (block_type == SHORT_BLOCK_TYPE)
        L3_imdct_short(grbuf, overlap, (int)(32 - n_long_bands));
    else
        L3_imdct36(grbuf, overlap, g_mdct_window[block_type == STOP_BLOCK_TYPE], (int)(32 - n_long_bands));
}

/* ---- bit reservoir: the data of a frame may start in the frames before it ---- */
static void L3_save_reservoir(Mp3Dec *h, Mp3Scratch *s)
{
    int pos = (s->bs.pos + 7) / 8;
    int remains = s->bs.limit / 8 - pos;
    if (remains > MP3_RESERVOIR)
    {
        pos += remains - MP3_RESERVOIR;
        remains = MP3_RESERVOIR;
    }
    if (remains > 0)
        mp3_move_down(h->reserv_buf, s->maindata + pos, remains);
    h->reserv = remains > 0 ? remains : 0;
}

static int L3_restore_reservoir(Mp3Dec *h, Mp3Bits *bs, Mp3Scratch *s, int main_data_begin)
{
    int frame_bytes = (bs->limit - bs->pos) / 8;
    int bytes_have = MP3_MIN(h->reserv, main_data_begin);
    mp3_copy(s->maindata, h->reserv_buf + MP3_MAX(0, h->reserv - main_data_begin), bytes_have);
    mp3_copy(s->maindata + bytes_have, bs->buf + bs->pos / 8, frame_bytes);
    bs_init(&s->bs, s->maindata, bytes_have + frame_bytes);
    return h->reserv >= main_data_begin;
}

static void L3_decode(Mp3Dec *h, Mp3Scratch *s, Mp3Gr *gr_info, int nch)
{
    int ch;
    for (ch = 0; ch < nch; ch++)
    {
        int layer3gr_limit = s->bs.pos + gr_info[ch].part_23_length;
        L3_decode_scalefactors(h->header, s->ist_pos[ch], &s->bs, gr_info + ch, s->scf, ch);
        L3_huffman(s->grbuf[ch], &s->bs, gr_info + ch, s->scf, layer3gr_limit);
    }

    if (HDR_TEST_I_STEREO(h->header))
        L3_intensity_stereo(s->grbuf[0], s->ist_pos[1], gr_info, h->header);
    else if (HDR_IS_MS_STEREO(h->header))
        L3_midside_stereo(s->grbuf[0], 576);

    for (ch = 0; ch < nch; ch++, gr_info++)
    {
        int aa_bands = 31;
        int n_long_bands = (gr_info->mixed_block_flag ? 2 : 0) << (int)(HDR_GET_MY_SAMPLE_RATE(h->header) == 2);

        if (gr_info->n_short_sfb)
        {
            aa_bands = n_long_bands - 1;
            L3_reorder(s->grbuf[ch] + n_long_bands * 18, s->syn[0], gr_info->sfbtab + gr_info->n_long_sfb);
        }
        L3_antialias(s->grbuf[ch], aa_bands);
        L3_imdct_gr(s->grbuf[ch], h->mdct_overlap[ch], gr_info->block_type, (unsigned)n_long_bands);
        L3_change_sign(s->grbuf[ch]);
    }
}

/* ---- polyphase synthesis: 32 subband values -> 32 PCM samples, for 18 time slots ---- */

/* 32 point DCT-II down the columns of the 32 x 18 subband matrix */
static void mp3d_DCT_II(float *grbuf, int n)
{
    int i, k;
    for (k = 0; k < n; k++)
    {
        float t[4][8], *x, *y = grbuf + k;

        for (x = t[0], i = 0; i < 8; i++, x++)
        {
            float x0 = y[i * 18];
            float x1 = y[(15 - i) * 18];
            float x2 = y[(16 + i) * 18];
            float x3 = y[(31 - i) * 18];
            float t0 = x0 + x3;
            float t1 = x1 + x2;
            float t2 = (x1 - x2) * g_sec[3 * i + 0];
            float t3 = (x0 - x3) * g_sec[3 * i + 1];
            x[0] = t0 + t1;
            x[8] = (t0 - t1) * g_sec[3 * i + 2];
            x[16] = t3 + t2;
            x[24] = (t3 - t2) * g_sec[3 * i + 2];
        }
        for (x = t[0], i = 0; i < 4; i++, x += 8)
        {
            float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4], x5 = x[5], x6 = x[6], x7 = x[7], xt;
            xt = x0 - x7; x0 += x7;
            x7 = x1 - x6; x1 += x6;
            x6 = x2 - x5; x2 += x5;
            x5 = x3 - x4; x3 += x4;
            x4 = x0 - x3; x0 += x3;
            x3 = x1 - x2; x1 += x2;
            x[0] = x0 + x1;
            x[4] = (x0 - x1) * 0.70710677f;
            x5 = x5 + x6;
            x6 = (x6 + x7) * 0.70710677f;
            x7 = x7 + xt;
            x3 = (x3 + x4) * 0.70710677f;
            x5 -= x7 * 0.198912367f; /* rotate by PI/8 */
            x7 += x5 * 0.382683432f;
            x5 -= x7 * 0.198912367f;
            x0 = xt - x6; xt += x6;
            x[1] = (xt + x7) * 0.50979561f;
            x[2] = (x4 + x3) * 0.54119611f;
            x[3] = (x0 - x5) * 0.60134488f;
            x[5] = (x0 + x5) * 0.89997619f;
            x[6] = (x4 - x3) * 1.30656302f;
            x[7] = (xt - x7) * 2.56291556f;
        }
        for (i = 0; i < 7; i++, y += 4 * 18)
        {
            y[0 * 18] = t[0][i];
            y[1 * 18] = t[2][i] + t[3][i] + t[3][i + 1];
            y[2 * 18] = t[1][i] + t[1][i + 1];
            y[3 * 18] = t[2][i + 1] + t[3][i] + t[3][i + 1];
        }
        y[0 * 18] = t[0][7];
        y[1 * 18] = t[2][7] + t[3][7];
        y[2 * 18] = t[1][7];
        y[3 * 18] = t[3][7];
    }
}

/* float -> 16 bit with rounding away from zero, clipped */
static short mp3d_scale_pcm(float sample)
{
    if (sample >= 32766.5f)
        return 32767;
    if (sample <= -32767.5f)
        return -32768;
    short s = (short)(sample + .5f);
    s -= (s < 0);
    return s;
}

static void mp3d_synth_pair(short *pcm, int nch, const float *z)
{
    float a;
    a = (z[14 * 64] - z[0]) * 29;
    a += (z[1 * 64] + z[13 * 64]) * 213;
    a += (z[12 * 64] - z[2 * 64]) * 459;
    a += (z[3 * 64] + z[11 * 64]) * 2037;
    a += (z[10 * 64] - z[4 * 64]) * 5153;
    a += (z[5 * 64] + z[9 * 64]) * 6574;
    a += (z[8 * 64] - z[6 * 64]) * 37489;
    a += z[7 * 64] * 75038;
    pcm[0] = mp3d_scale_pcm(a);

    z += 2;
    a = z[14 * 64] * 104;
    a += z[12 * 64] * 1567;
    a += z[10 * 64] * 9727;
    a += z[8 * 64] * 64019;
    a += z[6 * 64] * -9975;
    a += z[4 * 64] * -45;
    a += z[2 * 64] * 146;
    a += z[0 * 64] * -5;
    pcm[16 * nch] = mp3d_scale_pcm(a);
}

/* two adjacent time slots (columns) at a time; xl / xr are the left / right DCT output, lins the filter's history */
static void mp3d_synth(float *xl, short *dstl, int nch, float *lins)
{
    int i;
    float *xr = xl + 576 * (nch - 1);
    short *dstr = dstl + (nch - 1);
    float *zlin = lins + 15 * 64;
    const float *w = g_win;

    zlin[4 * 15] = xl[18 * 16];
    zlin[4 * 15 + 1] = xr[18 * 16];
    zlin[4 * 15 + 2] = xl[0];
    zlin[4 * 15 + 3] = xr[0];

    zlin[4 * 31] = xl[1 + 18 * 16];
    zlin[4 * 31 + 1] = xr[1 + 18 * 16];
    zlin[4 * 31 + 2] = xl[1];
    zlin[4 * 31 + 3] = xr[1];

    mp3d_synth_pair(dstr, nch, lins + 4 * 15 + 1);
    mp3d_synth_pair(dstr + 32 * nch, nch, lins + 4 * 15 + 64 + 1);
    mp3d_synth_pair(dstl, nch, lins + 4 * 15);
    mp3d_synth_pair(dstl + 32 * nch, nch, lins + 4 * 15 + 64);

    for (i = 14; i >= 0; i--)
    {
#define LOAD(k)                                   \
    float w0 = *w++;                              \
    float w1 = *w++;                              \
    float *vz = &zlin[4 * i - k * 64];            \
    float *vy = &zlin[4 * i - (15 - k) * 64];
#define S0(k)                                                              \
    {                                                                      \
        int j;                                                             \
        LOAD(k);                                                           \
        for (j = 0; j < 4; j++)                                            \
            b[j] = vz[j] * w1 + vy[j] * w0, a[j] = vz[j] * w0 - vy[j] * w1; \
    }
#define S1(k)                                                               \
    {                                                                       \
        int j;                                                              \
        LOAD(k);                                                            \
        for (j = 0; j < 4; j++)                                             \
            b[j] += vz[j] * w1 + vy[j] * w0, a[j] += vz[j] * w0 - vy[j] * w1; \
    }
#define S2(k)                                                               \
    {                                                                       \
        int j;                                                              \
        LOAD(k);                                                            \
        for (j = 0; j < 4; j++)                                             \
            b[j] += vz[j] * w1 + vy[j] * w0, a[j] += vy[j] * w1 - vz[j] * w0; \
    }
        float a[4], b[4];

        zlin[4 * i] = xl[18 * (31 - i)];
        zlin[4 * i + 1] = xr[18 * (31 - i)];
        zlin[4 * i + 2] = xl[1 + 18 * (31 - i)];
        zlin[4 * i + 3] = xr[1 + 18 * (31 - i)];
        zlin[4 * (i + 16)] = xl[1 + 18 * (1 + i)];
        zlin[4 * (i + 16) + 1] = xr[1 + 18 * (1 + i)];
        zlin[4 * (i - 16) + 2] = xl[18 * (1 + i)];
        zlin[4 * (i - 16) + 3] = xr[18 * (1 + i)];

        S0(0) S2(1) S1(2) S2(3) S1(4) S2(5) S1(6) S2(7)

        dstr[(15 - i) * nch] = mp3d_scale_pcm(a[1]);
        dstr[(17 + i) * nch] = mp3d_scale_pcm(b[1]);
        dstl[(15 - i) * nch] = mp3d_scale_pcm(a[0]);
        dstl[(17 + i) * nch] = mp3d_scale_pcm(b[0]);
        dstr[(47 - i) * nch] = mp3d_scale_pcm(a[3]);
        dstr[(49 + i) * nch] = mp3d_scale_pcm(b[3]);
        dstl[(47 - i) * nch] = mp3d_scale_pcm(a[2]);
        dstl[(49 + i) * nch] = mp3d_scale_pcm(b[2]);
#undef LOAD
#undef S0
#undef S1
#undef S2
    }
}

static void mp3d_synth_granule(float *qmf_state, float *grbuf, int nbands, int nch, short *pcm, float *lins)
{
    int i;
    for (i = 0; i < nch; i++)
        mp3d_DCT_II(grbuf + 576 * i, nbands);

    mp3_copy(lins, qmf_state, (int)sizeof(float) * 15 * 64);
    for (i = 0; i < nbands; i += 2)
        mp3d_synth(grbuf + i, pcm + 32 * nch * i, nch, lins + i * 64);

    if (nch == 1) /* mono keeps only every second history value (the left channel's slots) */
    {
        for (i = 0; i < 15 * 64; i += 2)
            qmf_state[i] = lins[nbands * 64 + i];
    }
    else
        mp3_copy(qmf_state, lins + nbands * 64, (int)sizeof(float) * 15 * 64);
}

/* ---- finding frames ---- */

/* hdr is followed by frames of the same stream, as far as the data reaches */
static int mp3d_match_frame(const unsigned char *hdr, int mp3_bytes)
{
    int i, nmatch;
    for (i = 0, nmatch = 0; nmatch < MAX_FRAME_SYNC_MATCHES; nmatch++)
    {
        i += hdr_frame_bytes(hdr + i) + hdr_padding(hdr + i);
        if (i + HDR_SIZE > mp3_bytes)
            return nmatch > 0;
        if (!hdr_compare(hdr, hdr + i))
            return 0;
    }
    return 1;
}

/* The offset of the first real frame (a header whose successors also look right), its length in *ptr_frame_bytes. */
static int mp3d_find_frame(const unsigned char *mp3, int mp3_bytes, int *ptr_frame_bytes)
{
    for (int i = 0; i < mp3_bytes - HDR_SIZE; i++, mp3++)
    {
        if (!hdr_valid(mp3))
            continue;
        int frame_bytes = hdr_frame_bytes(mp3);
        int frame_and_padding = frame_bytes + hdr_padding(mp3);
        if ((frame_bytes && i + frame_and_padding <= mp3_bytes && mp3d_match_frame(mp3, mp3_bytes - i)) || (!i && frame_and_padding == mp3_bytes))
        {
            *ptr_frame_bytes = frame_and_padding;
            return i;
        }
    }
    *ptr_frame_bytes = 0;
    return mp3_bytes;
}

void mp3_init(Mp3Dec *d)
{
    d->header[0] = 0;
    d->reserv = 0;
}

int mp3_decode_frame(Mp3Dec *dec, const unsigned char *mp3, int mp3_bytes, short *pcm, Mp3Info *info)
{
    int i = 0, igr, frame_size = 0;
    const unsigned char *hdr;
    Mp3Bits bs_frame;
    Mp3Scratch *scratch = &dec->s;

    if (mp3_bytes > 4 && dec->header[0] == 0xff && hdr_compare(dec->header, mp3)) /* the frame we expect, right here? */
    {
        frame_size = hdr_frame_bytes(mp3) + hdr_padding(mp3);
        if (frame_size != mp3_bytes && (frame_size + HDR_SIZE > mp3_bytes || !hdr_compare(mp3, mp3 + frame_size)))
            frame_size = 0;
    }
    if (!frame_size)
    {
        dec->header[0] = 0;
        dec->reserv = 0;
        mp3_zero(dec->mdct_overlap, (int)sizeof dec->mdct_overlap);
        mp3_zero(dec->qmf_state, (int)sizeof dec->qmf_state);
        i = mp3d_find_frame(mp3, mp3_bytes, &frame_size);
        if (!frame_size || i + frame_size > mp3_bytes)
        {
            info->frame_bytes = i;
            info->layer = 0;
            return 0;
        }
    }

    hdr = mp3 + i;
    mp3_copy(dec->header, hdr, HDR_SIZE);
    info->frame_bytes = i + frame_size;
    info->frame_offset = i;
    info->channels = HDR_IS_MONO(hdr) ? 1 : 2;
    info->hz = (int)hdr_sample_rate_hz(hdr);
    info->layer = 4 - HDR_GET_LAYER(hdr);
    info->bitrate_kbps = (int)hdr_bitrate_kbps(hdr);
    if (info->layer != 3 || frame_size - HDR_SIZE > MP3_MAX_FRAME)
    {
        dec->header[0] = 0;
        return 0; /* Layer I / II: skipped as a whole frame */
    }

    bs_init(&bs_frame, hdr + HDR_SIZE, frame_size - HDR_SIZE);
    if (HDR_IS_CRC(hdr))
        get_bits(&bs_frame, 16);

    int main_data_begin = L3_read_side_info(&bs_frame, scratch->gr, hdr);
    if (main_data_begin < 0 || bs_frame.pos > bs_frame.limit)
    {
        mp3_init(dec);
        return 0;
    }
    int success = L3_restore_reservoir(dec, &bs_frame, scratch, main_data_begin);
    if (success)
        for (igr = 0; igr < (HDR_TEST_MPEG1(hdr) ? 2 : 1); igr++, pcm += 576 * info->channels)
        {
            mp3_zero(scratch->grbuf[0], 576 * 2 * (int)sizeof(float));
            L3_decode(dec, scratch, scratch->gr + igr * info->channels, info->channels);
            mp3d_synth_granule(dec->qmf_state, scratch->grbuf[0], 18, info->channels, pcm, scratch->syn[0]);
        }
    L3_save_reservoir(dec, scratch);
    return success * (int)hdr_frame_samples(dec->header);
}
