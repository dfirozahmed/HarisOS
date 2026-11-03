/* HarisOS JPEG decoder: see picture_internal.h. Format from ITU-T T.81 (JPEG) and the JFIF file layout: markers
 * (0xFF, code) split the file into segments: DQT quantization tables, SOF0 / SOF1 the frame (size, components and
 * their sampling), DHT Huffman tables, DRI restart interval, SOS the start of the entropy-coded data.
 * The data is cut into MCUs (minimum coded units) of 8x8 blocks; each block is a DC difference plus run-length coded AC
 * coefficients in zig-zag order, multiplied by the quantization table, turned back into pixels by the inverse DCT.
 * Y, Cb, Cr planes are then put together into RGB (the chroma planes may be smaller: they are repeated).
 * Only integers are used (no floating point in the kernel). */
#include "heap.h"
#include "picture.h"
#include "picture_internal.h"

#define MAX_COMP 3

typedef struct
{
    unsigned char bits[17]; /* codes per length 1 .. 16 */
    unsigned char val[256]; /* the symbols in code order */
    int mincode[17], maxcode[18], valptr[17];
    int set;
} Jhuff;

typedef struct
{
    int id, h, v, tq;
    int dc_t, ac_t, pred;
    int pw, ph; /* plane size in pixels (a whole number of blocks) */
    unsigned char *plane;
} Comp;

typedef struct
{
    const unsigned char *p, *end;
    unsigned buf;
    int cnt;
    int hit; /* a marker was met in the data: zeros are fed from then on */
} JBits;

static const unsigned char ZZ[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
                                     12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                                     35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                     58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* cos table of the 8-point inverse DCT in 1/4096: M[x][u] = C(u) / 2 * cos((2x + 1) u pi / 16), C(0) = 1 / sqrt 2 */
static const int IDCT_M[8][8] = {
    {1448, 2009, 1892, 1703, 1448, 1138, 784, 400},
    {1448, 1703, 784, -400, -1448, -2009, -1892, -1138},
    {1448, 1138, -784, -2009, -1448, 400, 1892, 1703},
    {1448, 400, -1892, -1138, 1448, 1703, -784, -2009},
    {1448, -400, -1892, 1138, 1448, -1703, -784, 2009},
    {1448, -1138, -784, 2009, -1448, -400, 1892, -1703},
    {1448, -1703, 784, 400, -1448, 2009, -1892, 1138},
    {1448, -2009, 1892, -1703, 1448, -1138, 784, -400},
};

typedef struct
{
    int w, h, ncomp, hmax, vmax, mcux, mcuy, ri, adobe_transform, got_scan;
    unsigned short q[4][64]; /* natural order */
    int q_set[4];
    Jhuff dc[4], ac[4];
    Comp c[MAX_COMP];
    int have_frame;
} Jpeg;

static int next_bit(JBits *b)
{
    if (b->cnt == 0)
    {
        unsigned byte = 0;
        if (!b->hit && b->p < b->end)
        {
            byte = *b->p;
            if (byte == 0xFF)
            {
                if (b->p + 1 < b->end && b->p[1] == 0)
                    b->p += 2; /* stuffed zero after a data byte of 0xFF */
                else
                {
                    b->hit = 1; /* a marker: p stays on it */
                    byte = 0;
                }
            }
            else
                b->p++;
        }
        b->buf = byte;
        b->cnt = 8;
    }
    b->cnt--;
    return (int)((b->buf >> b->cnt) & 1u);
}

static int get_bits(JBits *b, int n)
{
    int v = 0;
    while (n--)
        v = (v << 1) | next_bit(b);
    return v;
}

static int jh_decode(JBits *b, const Jhuff *h)
{
    int code = 0;
    for (int l = 1; l <= 16; l++)
    {
        code = (code << 1) | next_bit(b);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l])
            return h->val[h->valptr[l] + code - h->mincode[l]];
    }
    return -1;
}

static int extend(int v, int s) { return s == 0 ? 0 : (v < (1 << (s - 1)) ? v - (1 << s) + 1 : v); }

static void jh_build(Jhuff *h)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++)
    {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        k += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->set = 1;
}

static unsigned char clamp8(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* one block of dequantized coefficients (natural order) into an 8x8 square of the plane */
static void idct_block(const int *in, unsigned char *dst, int stride)
{
    long long tmp[64];
    for (int y = 0; y < 8; y++) /* rows: tmp[y][x] = sum_u M[x][u] * in[y][u] */
        for (int x = 0; x < 8; x++)
        {
            long long s = 0;
            for (int u = 0; u < 8; u++)
                s += (long long)IDCT_M[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = (s + 2048) >> 12;
        }
    for (int x = 0; x < 8; x++) /* columns */
        for (int y = 0; y < 8; y++)
        {
            long long s = 0;
            for (int v = 0; v < 8; v++)
                s += (long long)IDCT_M[y][v] * tmp[v * 8 + x];
            dst[y * stride + x] = clamp8((int)((s + 2048) >> 12) + 128);
        }
}

/* decode one block of component c into the plane at block (bx, by); -1 on a broken code */
static int decode_block(JBits *b, Jpeg *J, Comp *c, int bx, int by)
{
    int blk[64];
    for (int i = 0; i < 64; i++)
        blk[i] = 0;
    const unsigned short *q = J->q[c->tq];
    int s = jh_decode(b, &J->dc[c->dc_t]);
    if (s < 0 || s > 16)
        return -1;
    c->pred += extend(get_bits(b, s), s);
    blk[0] = c->pred * q[0];
    for (int k = 1; k < 64;)
    {
        int rs = jh_decode(b, &J->ac[c->ac_t]);
        if (rs < 0)
            return -1;
        int r = rs >> 4;
        s = rs & 15;
        if (s == 0)
        {
            if (r != 15)
                break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return -1;
        blk[ZZ[k]] = extend(get_bits(b, s), s) * q[ZZ[k]];
        k++;
    }
    if (bx * 8 + 8 > c->pw || by * 8 + 8 > c->ph)
        return -1;
    idct_block(blk, c->plane + (long long)by * 8 * c->pw + bx * 8, c->pw);
    return 0;
}

/* restart marker: forget the bit buffer and the DC predictors, step over RSTn */
static void restart(JBits *b, Jpeg *J)
{
    b->cnt = 0;
    while (b->p + 1 < b->end && !(b->p[0] == 0xFF && b->p[1] >= 0xD0 && b->p[1] <= 0xD7))
        b->p++;
    if (b->p + 1 < b->end)
        b->p += 2;
    b->hit = 0;
    for (int i = 0; i < J->ncomp; i++)
        J->c[i].pred = 0;
}

/* the entropy-coded data of one scan; ns components (cs[]) listed in the scan. Returns 0, or -1 */
static int decode_scan(JBits *b, Jpeg *J, Comp **cs, int ns)
{
    int count = 0;
    for (int i = 0; i < J->ncomp; i++)
        J->c[i].pred = 0;
    if (ns > 1) /* interleaved: each MCU has h x v blocks of every component */
    {
        for (int my = 0; my < J->mcuy; my++)
            for (int mx = 0; mx < J->mcux; mx++)
            {
                if (J->ri && count && count % J->ri == 0)
                    restart(b, J);
                count++;
                for (int i = 0; i < ns; i++)
                    for (int v = 0; v < cs[i]->v; v++)
                        for (int h = 0; h < cs[i]->h; h++)
                            if (decode_block(b, J, cs[i], mx * cs[i]->h + h, my * cs[i]->v + v) != 0)
                                return -1;
            }
        return 0;
    }
    /* one component alone: the blocks that cover its own pixels, left to right, top to bottom */
    Comp *c = cs[0];
    int cw = (J->w * c->h + J->hmax - 1) / J->hmax, chh = (J->h * c->v + J->vmax - 1) / J->vmax;
    int bw = (cw + 7) / 8, bh = (chh + 7) / 8;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
        {
            if (J->ri && count && count % J->ri == 0)
                restart(b, J);
            count++;
            if (decode_block(b, J, c, bx, by) != 0)
                return -1;
        }
    return 0;
}

static unsigned rd16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

static void free_planes(Jpeg *J)
{
    for (int i = 0; i < MAX_COMP; i++)
    {
        kfree(J->c[i].plane);
        J->c[i].plane = 0;
    }
}

unsigned int *jpeg_decode(const unsigned char *f, unsigned size, int *w, int *h)
{
    if (!f || size < 4 || f[0] != 0xFF || f[1] != 0xD8)
        return 0;
    Jpeg *J = kmalloc(sizeof *J);
    if (!J)
        return 0;
    unsigned char *base = (unsigned char *)J;
    for (unsigned i = 0; i < sizeof *J; i++)
        base[i] = 0;
    J->adobe_transform = -1;

    unsigned pos = 2;
    unsigned int *px = 0;
    while (pos + 4 <= size)
    {
        if (f[pos] != 0xFF) /* not on a marker: look for the next one */
        {
            pos++;
            continue;
        }
        unsigned m = f[pos + 1];
        if (m == 0xFF || m == 0x00 || (m >= 0xD0 && m <= 0xD8) || m == 0x01) /* fill, stuffed, RSTn, SOI, TEM: no length */
        {
            pos += m == 0xFF ? 1 : 2;
            continue;
        }
        if (m == 0xD9) /* EOI */
            break;
        unsigned len = rd16(f + pos + 2);
        if (len < 2 || pos + 2 + len > size)
            break;
        const unsigned char *s = f + pos + 4;
        unsigned sl = len - 2;
        if (m == 0xDB) /* DQT */
        {
            while (sl >= 1)
            {
                int pq = s[0] >> 4, tq = s[0] & 15;
                unsigned need = 1 + (pq ? 128u : 64u);
                if (tq > 3 || pq > 1 || sl < need)
                    goto fail;
                for (int i = 0; i < 64; i++)
                    J->q[tq][ZZ[i]] = pq ? (unsigned short)rd16(s + 1 + 2 * i) : s[1 + i];
                J->q_set[tq] = 1;
                s += need;
                sl -= need;
            }
        }
        else if (m == 0xC4) /* DHT */
        {
            while (sl >= 17)
            {
                int tc = s[0] >> 4, th = s[0] & 15, total = 0;
                if (tc > 1 || th > 3)
                    goto fail;
                Jhuff *H = tc ? &J->ac[th] : &J->dc[th];
                H->bits[0] = 0;
                for (int i = 1; i <= 16; i++)
                    total += H->bits[i] = s[i];
                if (total > 256 || sl < 17u + (unsigned)total)
                    goto fail;
                for (int i = 0; i < total; i++)
                    H->val[i] = s[17 + i];
                jh_build(H);
                s += 17 + total;
                sl -= 17 + (unsigned)total;
            }
        }
        else if (m == 0xC0 || m == 0xC1) /* SOF0 baseline, SOF1 extended sequential (8-bit) */
        {
            if (J->have_frame || sl < 6 || s[0] != 8)
                goto fail;
            J->h = (int)rd16(s + 1);
            J->w = (int)rd16(s + 3);
            J->ncomp = s[5];
            if (J->w <= 0 || J->h <= 0 || (unsigned long long)J->w * (unsigned)J->h > PICTURE_MAX_PIXELS ||
                (J->ncomp != 1 && J->ncomp != 3) || sl < 6u + 3u * (unsigned)J->ncomp)
                goto fail;
            for (int i = 0; i < J->ncomp; i++)
            {
                Comp *c = &J->c[i];
                c->id = s[6 + 3 * i];
                c->h = s[7 + 3 * i] >> 4;
                c->v = s[7 + 3 * i] & 15;
                c->tq = s[8 + 3 * i];
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3)
                    goto fail;
                if (c->h > J->hmax)
                    J->hmax = c->h;
                if (c->v > J->vmax)
                    J->vmax = c->v;
            }
            J->mcux = (J->w + 8 * J->hmax - 1) / (8 * J->hmax);
            J->mcuy = (J->h + 8 * J->vmax - 1) / (8 * J->vmax);
            for (int i = 0; i < J->ncomp; i++)
            {
                Comp *c = &J->c[i];
                c->pw = J->mcux * c->h * 8;
                c->ph = J->mcuy * c->v * 8;
                c->plane = kmalloc((unsigned long long)c->pw * (unsigned)c->ph);
                if (!c->plane)
                    goto fail;
            }
            J->have_frame = 1;
        }
        else if ((m >= 0xC2 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) /* progressive, lossless, arithmetic ... */
            goto fail;
        else if (m == 0xDD) /* DRI */
        {
            if (sl < 2)
                goto fail;
            J->ri = (int)rd16(s);
        }
        else if (m == 0xEE && sl >= 12 && s[0] == 'A' && s[1] == 'd' && s[2] == 'o' && s[3] == 'b' && s[4] == 'e')
            J->adobe_transform = s[11];
        else if (m == 0xDA) /* SOS */
        {
            if (!J->have_frame || sl < 1)
                goto fail;
            int ns = s[0];
            if (ns < 1 || ns > J->ncomp || sl < 1u + 2u * (unsigned)ns + 3u)
                goto fail;
            Comp *cs[MAX_COMP];
            for (int i = 0; i < ns; i++)
            {
                int id = s[1 + 2 * i], found = -1;
                for (int k = 0; k < J->ncomp; k++)
                    if (J->c[k].id == id)
                        found = k;
                int dc = s[2 + 2 * i] >> 4, ac = s[2 + 2 * i] & 15;
                if (found < 0 || dc > 3 || ac > 3)
                    goto fail;
                cs[i] = &J->c[found];
                cs[i]->dc_t = dc;
                cs[i]->ac_t = ac;
                if (!J->dc[dc].set || !J->ac[ac].set || !J->q_set[cs[i]->tq])
                    goto fail;
            }
            JBits b = {f + pos + 2 + len, f + size, 0, 0, 0};
            if (decode_scan(&b, J, cs, ns) != 0)
                goto fail;
            J->got_scan = 1;
            pos = (unsigned)(b.p - f); /* continue after the data (at the next marker) */
            continue;
        }
        pos += 2 + len;
    }
    if (!J->have_frame || !J->got_scan)
        goto fail;

    px = kmalloc((unsigned long long)J->w * (unsigned)J->h * 4u);
    if (!px)
        goto fail;
    int ycc = J->ncomp == 3 && J->adobe_transform != 0;
    for (int y = 0; y < J->h; y++)
        for (int x = 0; x < J->w; x++)
        {
            int v[MAX_COMP];
            for (int i = 0; i < J->ncomp; i++)
            {
                const Comp *c = &J->c[i];
                v[i] = c->plane[(long long)(y * c->v / J->vmax) * c->pw + x * c->h / J->hmax];
            }
            int r, g, bl;
            if (J->ncomp == 1)
                r = g = bl = v[0];
            else if (ycc)
            {
                int cb = v[1] - 128, cr = v[2] - 128;
                r = v[0] + ((91881 * cr + 32768) >> 16);
                g = v[0] - ((22554 * cb + 46802 * cr + 32768) >> 16);
                bl = v[0] + ((116130 * cb + 32768) >> 16);
            }
            else
            {
                r = v[0];
                g = v[1];
                bl = v[2];
            }
            px[(unsigned long long)y * (unsigned)J->w + (unsigned)x] =
                ((unsigned)clamp8(r) << 16) | ((unsigned)clamp8(g) << 8) | clamp8(bl);
        }
    *w = J->w;
    *h = J->h;
    free_planes(J);
    kfree(J);
    return px;

fail:
    free_planes(J);
    kfree(J);
    return 0;
}
