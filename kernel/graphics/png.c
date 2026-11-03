/* HarisOS PNG decoder: see picture_internal.h. Format from the PNG specification (RFC 2083 / ISO 15948): an 8-byte
 * signature, then chunks (4-byte big-endian length, 4-byte type, data, 4-byte CRC). IHDR gives the size, bit depth,
 * colour type and interlace; PLTE the palette; the IDAT chunks together hold one zlib stream (RFC 1950) of DEFLATE
 * data (RFC 1951) which unpacks to the scanlines, each starting with a filter-type byte (None, Sub, Up, Average, Paeth).
 * The CRCs and the zlib checksum are not checked: a damaged file shows damaged pixels, never a crash. */
#include "heap.h"
#include "picture.h"
#include "picture_internal.h"

/* ---- DEFLATE ---- */
typedef struct
{
    const unsigned char *p, *end;
    unsigned buf;
    int cnt;
    int err; /* read past the end */
} Bits;

static unsigned getbits(Bits *b, int n) /* n <= 16 */
{
    while (b->cnt < n)
    {
        unsigned byte = 0;
        if (b->p < b->end)
            byte = *b->p++;
        else
            b->err = 1;
        b->buf |= byte << b->cnt;
        b->cnt += 8;
    }
    unsigned v = b->buf & ((1u << n) - 1u);
    b->buf >>= n;
    b->cnt -= n;
    return v;
}

typedef struct
{
    unsigned short count[16]; /* how many codes have each length */
    unsigned short sym[288];  /* the symbols, in order of code */
} Huff;

/* canonical Huffman code from the lengths of its symbols; -1 if the lengths over-subscribe the code */
static int huff_build(Huff *h, const unsigned char *len, int n)
{
    unsigned short offs[16];
    for (int i = 0; i < 16; i++)
        h->count[i] = 0;
    for (int s = 0; s < n; s++)
        h->count[len[s]]++;
    int left = 1;
    for (int l = 1; l < 16; l++)
    {
        left = (left << 1) - h->count[l];
        if (left < 0)
            return -1;
    }
    offs[1] = 0;
    for (int l = 1; l < 15; l++)
        offs[l + 1] = (unsigned short)(offs[l] + h->count[l]);
    for (int s = 0; s < n; s++)
        if (len[s])
            h->sym[offs[len[s]]++] = (unsigned short)s;
    return 0;
}

static int huff_decode(Bits *b, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++)
    {
        code |= (int)getbits(b, 1);
        int count = h->count[l];
        if (code - count < first)
            return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const unsigned short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const unsigned char LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const unsigned char DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* the symbols of one compressed block: literals are copied, (length, distance) pairs copy from what was written */
static int inflate_codes(Bits *b, unsigned char *out, unsigned cap, unsigned *pos, const Huff *lit, const Huff *dist)
{
    for (;;)
    {
        int s = huff_decode(b, lit);
        if (s < 0 || b->err)
            return -1;
        if (s < 256)
        {
            if (*pos >= cap)
                return -1;
            out[(*pos)++] = (unsigned char)s;
        }
        else if (s == 256)
            return 0;
        else
        {
            s -= 257;
            if (s >= 29)
                return -1;
            unsigned len = LBASE[s] + getbits(b, LEXT[s]);
            int d = huff_decode(b, dist);
            if (d < 0 || d >= 30)
                return -1;
            unsigned back = DBASE[d] + getbits(b, DEXT[d]);
            if (back > *pos || len > cap - *pos || b->err)
                return -1;
            for (unsigned i = 0; i < len; i++, (*pos)++)
                out[*pos] = out[*pos - back];
        }
    }
}

/* the whole stream (after the 2-byte zlib header) into out[0 .. cap); 0 when it ended with the final block */
static int inflate(Bits *b, unsigned char *out, unsigned cap, unsigned *pos)
{
    static const unsigned char ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int last;
    *pos = 0;
    do
    {
        last = (int)getbits(b, 1);
        unsigned type = getbits(b, 2);
        if (b->err)
            return -1;
        if (type == 0) /* stored: byte-aligned length, its complement, then the bytes */
        {
            getbits(b, b->cnt & 7);
            unsigned len = getbits(b, 16), nlen = getbits(b, 16);
            if (b->err || (len ^ 0xFFFFu) != nlen || len > cap - *pos)
                return -1;
            for (unsigned i = 0; i < len; i++)
                out[(*pos)++] = (unsigned char)getbits(b, 8);
            if (b->err)
                return -1;
        }
        else if (type == 1 || type == 2)
        {
            static Huff lit, dist;
            unsigned char len[320];
            if (type == 1) /* the fixed code of the specification */
            {
                int i = 0;
                for (; i < 144; i++)
                    len[i] = 8;
                for (; i < 256; i++)
                    len[i] = 9;
                for (; i < 280; i++)
                    len[i] = 7;
                for (; i < 288; i++)
                    len[i] = 8;
                huff_build(&lit, len, 288);
                for (i = 0; i < 30; i++)
                    len[i] = 5;
                huff_build(&dist, len, 30);
            }
            else /* the code is sent first, itself Huffman coded */
            {
                int nlen = (int)getbits(b, 5) + 257, ndist = (int)getbits(b, 5) + 1, ncode = (int)getbits(b, 4) + 4;
                if (nlen > 286 || ndist > 30 || b->err)
                    return -1;
                int i = 0;
                for (; i < ncode; i++)
                    len[ORDER[i]] = (unsigned char)getbits(b, 3);
                for (; i < 19; i++)
                    len[ORDER[i]] = 0;
                Huff lencode;
                if (huff_build(&lencode, len, 19) != 0)
                    return -1;
                int idx = 0;
                unsigned char all[320];
                while (idx < nlen + ndist)
                {
                    int sym = huff_decode(b, &lencode);
                    if (sym < 0 || b->err)
                        return -1;
                    if (sym < 16)
                        all[idx++] = (unsigned char)sym;
                    else
                    {
                        int rep, val = 0;
                        if (sym == 16)
                        {
                            if (idx == 0)
                                return -1;
                            val = all[idx - 1];
                            rep = 3 + (int)getbits(b, 2);
                        }
                        else if (sym == 17)
                            rep = 3 + (int)getbits(b, 3);
                        else
                            rep = 11 + (int)getbits(b, 7);
                        if (idx + rep > nlen + ndist)
                            return -1;
                        while (rep--)
                            all[idx++] = (unsigned char)val;
                    }
                }
                if (all[256] == 0) /* no end-of-block code */
                    return -1;
                if (huff_build(&lit, all, nlen) != 0 || huff_build(&dist, all + nlen, ndist) != 0)
                    return -1;
            }
            if (inflate_codes(b, out, cap, pos, &lit, &dist) != 0)
                return -1;
        }
        else
            return -1;
    } while (!last);
    return 0;
}

/* ---- PNG ---- */
static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | (unsigned)p[3];
}

typedef struct
{
    int w, h, depth, ctype, interlace;
    int channels, bits;      /* samples per pixel, bits per pixel */
    unsigned char pal[256][3];
    int npal;
} Png;

static const unsigned char XSTART[7] = {0, 4, 0, 2, 0, 1, 0}, YSTART[7] = {0, 0, 4, 0, 2, 0, 1};
static const unsigned char XSTEP[7] = {8, 8, 4, 4, 2, 2, 1}, YSTEP[7] = {8, 8, 8, 4, 4, 2, 2};

/* the size of pass `pass` (a non-interlaced picture is the one pass 0 with steps of 1) */
static void pass_size(const Png *P, int pass, int *x0, int *y0, int *dx, int *dy, int *pw, int *ph)
{
    if (!P->interlace)
    {
        *x0 = *y0 = 0;
        *dx = *dy = 1;
        *pw = P->w;
        *ph = P->h;
        return;
    }
    *x0 = XSTART[pass];
    *y0 = YSTART[pass];
    *dx = XSTEP[pass];
    *dy = YSTEP[pass];
    *pw = P->w > *x0 ? (P->w - *x0 + *dx - 1) / *dx : 0;
    *ph = P->h > *y0 ? (P->h - *y0 + *dy - 1) / *dy : 0;
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
}

/* undo the filter of one scanline in place; prev is the finished line above (NULL for the first) */
static int unfilter(unsigned char *cur, const unsigned char *prev, unsigned n, unsigned bpp, int type)
{
    for (unsigned x = 0; x < n; x++)
    {
        int a = x >= bpp ? cur[x - bpp] : 0, b = prev ? prev[x] : 0, c = prev && x >= bpp ? prev[x - bpp] : 0;
        switch (type)
        {
        case 0:
            break;
        case 1:
            cur[x] = (unsigned char)(cur[x] + a);
            break;
        case 2:
            cur[x] = (unsigned char)(cur[x] + b);
            break;
        case 3:
            cur[x] = (unsigned char)(cur[x] + ((a + b) >> 1));
            break;
        case 4:
            cur[x] = (unsigned char)(cur[x] + paeth(a, b, c));
            break;
        default:
            return -1;
        }
    }
    return 0;
}

/* sample i of a line as 0 .. 2^depth - 1 (16-bit samples: the high byte) */
static int sample(const unsigned char *row, int i, int depth)
{
    if (depth == 8)
        return row[i];
    if (depth == 16)
        return row[2 * i];
    int per = 8 / depth, shift = 8 - depth * (i % per + 1);
    return (row[i / per] >> shift) & ((1 << depth) - 1);
}

static unsigned pixel(const Png *P, const unsigned char *row, int x)
{
    int r, g, b, a = 255, d = P->depth;
    switch (P->ctype)
    {
    case 0:
        r = g = b = sample(row, x, d);
        if (d < 8)
            r = g = b = r * 255 / ((1 << d) - 1);
        break;
    case 2:
        r = sample(row, 3 * x, d);
        g = sample(row, 3 * x + 1, d);
        b = sample(row, 3 * x + 2, d);
        break;
    case 3:
    {
        int i = sample(row, x, d);
        r = i < P->npal ? P->pal[i][0] : 0;
        g = i < P->npal ? P->pal[i][1] : 0;
        b = i < P->npal ? P->pal[i][2] : 0;
        break;
    }
    case 4:
        r = g = b = sample(row, 2 * x, d);
        a = sample(row, 2 * x + 1, d);
        break;
    default:
        r = sample(row, 4 * x, d);
        g = sample(row, 4 * x + 1, d);
        b = sample(row, 4 * x + 2, d);
        a = sample(row, 4 * x + 3, d);
        break;
    }
    if (a != 255) /* over black */
    {
        r = r * a / 255;
        g = g * a / 255;
        b = b * a / 255;
    }
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

unsigned int *png_decode(const unsigned char *f, unsigned size, int *w, int *h)
{
    static const unsigned char SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!f || size < 8 + 25)
        return 0;
    for (int i = 0; i < 8; i++)
        if (f[i] != SIG[i])
            return 0;

    Png P;
    int have_ihdr = 0, idat_seen = 0;
    P.npal = 0;
    unsigned char *z = kmalloc(size); /* the IDAT chunks joined: one zlib stream */
    if (!z)
        return 0;
    unsigned zn = 0;
    for (unsigned pos = 8; pos + 12 <= size;)
    {
        unsigned len = be32(f + pos);
        const unsigned char *type = f + pos + 4, *data = f + pos + 8;
        if (len > size - pos - 12)
            break; /* cut short: use what there is */
        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R' && len >= 13)
        {
            unsigned pw = be32(data), ph = be32(data + 4);
            P.depth = data[8];
            P.ctype = data[9];
            P.interlace = data[12];
            if (pw == 0 || ph == 0 || pw > 16384 || ph > 16384 || (unsigned long long)pw * ph > PICTURE_MAX_PIXELS ||
                data[10] != 0 || data[11] != 0 || P.interlace > 1)
                goto fail;
            P.w = (int)pw;
            P.h = (int)ph;
            P.channels = P.ctype == 0 ? 1 : P.ctype == 2 ? 3 : P.ctype == 3 ? 1 : P.ctype == 4 ? 2 : P.ctype == 6 ? 4 : 0;
            int d = P.depth, ok = P.channels != 0;
            if (ok)
                switch (P.ctype)
                {
                case 0:
                    ok = d == 1 || d == 2 || d == 4 || d == 8 || d == 16;
                    break;
                case 3:
                    ok = d == 1 || d == 2 || d == 4 || d == 8;
                    break;
                default:
                    ok = d == 8 || d == 16;
                    break;
                }
            if (!ok)
                goto fail;
            P.bits = P.channels * d;
            have_ihdr = 1;
        }
        else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E')
        {
            P.npal = (int)(len / 3 > 256 ? 256 : len / 3);
            for (int i = 0; i < P.npal; i++)
                for (int c = 0; c < 3; c++)
                    P.pal[i][c] = data[3 * i + c];
        }
        else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T')
        {
            for (unsigned i = 0; i < len && zn < size; i++)
                z[zn++] = data[i];
            idat_seen = 1;
        }
        else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D')
            break;
        pos += 12 + len;
    }
    if (!have_ihdr || !idat_seen || zn < 2 || (z[0] & 15) != 8 || ((z[0] << 8) | z[1]) % 31 != 0 || (z[1] & 0x20))
        goto fail;

    /* the unpacked size is known from the header: every pass's lines, each with its filter byte */
    unsigned long long raw = 0;
    for (int pass = 0; pass < (P.interlace ? 7 : 1); pass++)
    {
        int x0, y0, dx, dy, pw, ph;
        pass_size(&P, pass, &x0, &y0, &dx, &dy, &pw, &ph);
        if (pw > 0 && ph > 0)
            raw += (unsigned long long)ph * (1u + ((unsigned long long)pw * (unsigned)P.bits + 7) / 8);
    }
    if (raw > 0xF0000000ull)
        goto fail;
    unsigned char *data = kmalloc(raw);
    unsigned int *px = data ? kmalloc((unsigned long long)P.w * (unsigned)P.h * 4u) : 0;
    if (!data || !px)
    {
        kfree(data);
        kfree(px);
        goto fail;
    }
    Bits b = {z + 2, z + zn, 0, 0, 0};
    unsigned got;
    if (inflate(&b, data, (unsigned)raw, &got) != 0 || got != raw)
    {
        kfree(data);
        kfree(px);
        goto fail;
    }

    unsigned off = 0, bpp = (unsigned)(P.bits + 7) / 8;
    for (int pass = 0; pass < (P.interlace ? 7 : 1); pass++)
    {
        int x0, y0, dx, dy, pw, ph;
        pass_size(&P, pass, &x0, &y0, &dx, &dy, &pw, &ph);
        if (pw <= 0 || ph <= 0)
            continue;
        unsigned n = ((unsigned)pw * (unsigned)P.bits + 7) / 8;
        const unsigned char *prev = 0;
        for (int y = 0; y < ph; y++)
        {
            unsigned char *cur = data + off + 1;
            if (unfilter(cur, prev, n, bpp, data[off]) != 0)
            {
                kfree(data);
                kfree(px);
                goto fail;
            }
            for (int x = 0; x < pw; x++)
                px[(unsigned long long)(y0 + y * dy) * (unsigned)P.w + (unsigned)(x0 + x * dx)] = pixel(&P, cur, x);
            prev = cur;
            off += 1 + n;
        }
    }
    kfree(data);
    kfree(z);
    *w = P.w;
    *h = P.h;
    return px;

fail:
    kfree(z);
    return 0;
}
