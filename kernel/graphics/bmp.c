/* HarisOS BMP pictures: see include/bmp.h. Layout from the Windows GDI documentation (BITMAPFILEHEADER, then
 * BITMAPINFOHEADER or a longer V4 / V5 header): 'B' 'M', the pixel data's offset at byte 10, the header's size at 14,
 * width 18, height 22, planes 26, bits per pixel 28, compression 30; with BI_BITFIELDS the red / green / blue masks
 * follow at 54 (header of 40 bytes) or are inside the longer header at the same place. */
#include "bmp.h"

#define BI_RGB 0u
#define BI_BITFIELDS 3u

typedef struct
{
    int w, h, top_down, bpp;
    unsigned offset, stride;
    int shift[3]; /* red, green, blue: where their 8 bits are in a 32-bit pixel */
} BmpLayout;

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }

/* the shift of an 8-bit mask (0xFF << s), -1 for any other mask */
static int mask_shift(unsigned m)
{
    for (int s = 0; s <= 24; s += 8)
        if (m == 0xFFu << s)
            return s;
    return -1;
}

static int layout(const unsigned char *f, unsigned size, BmpLayout *L)
{
    if (!f || size < 54 || f[0] != 'B' || f[1] != 'M')
        return -1;
    unsigned hdr = rd32(f + 14), comp = rd32(f + 30);
    int w = (int)rd32(f + 18), h = (int)rd32(f + 22);
    if (hdr < 40 || hdr > size - 14 || rd16(f + 26) != 1)
        return -1;
    L->bpp = (int)rd16(f + 28);
    L->top_down = h < 0;
    if (h < 0)
        h = -h;
    if (w <= 0 || h <= 0 || w > BMP_MAX_SIDE || h > BMP_MAX_SIDE)
        return -1;
    L->w = w;
    L->h = h;
    L->shift[0] = 16; /* B, G, R in memory: red is the third byte */
    L->shift[1] = 8;
    L->shift[2] = 0;
    if (L->bpp == 24 && comp == BI_RGB)
        ;
    else if (L->bpp == 32 && comp == BI_RGB)
        ;
    else if (L->bpp == 32 && comp == BI_BITFIELDS)
    {
        if (size < 14u + 40u + 12u)
            return -1;
        for (int c = 0; c < 3; c++)
            if ((L->shift[c] = mask_shift(rd32(f + 54 + 4 * c))) < 0)
                return -1;
    }
    else
        return -1;
    L->stride = (unsigned)(((unsigned long long)w * (unsigned)L->bpp + 31u) / 32u * 4u);
    L->offset = rd32(f + 10);
    unsigned head_end = 14u + hdr + (comp == BI_BITFIELDS && hdr == 40 ? 12u : 0u); /* the masks after a short header */
    if (L->offset < head_end ||(unsigned long long)L->offset + (unsigned long long)L->stride * (unsigned)h > size)
        return -1;
    return 0;
}

int bmp_info(const unsigned char *file, unsigned size, int *w, int *h)
{
    BmpLayout L;
    if (layout(file, size, &L) != 0)
        return -1;
    *w = L.w;
    *h = L.h;
    return 0;
}

/* source pixel (x, y), y counted from the top, as r, g, b */
static void src_px(const unsigned char *f, const BmpLayout *L, int x, int y, int *rgb)
{
    int row = L->top_down ? y : L->h - 1 - y;
    const unsigned char *p = f + L->offset + (unsigned long long)L->stride * (unsigned)row + (unsigned)x * (unsigned)(L->bpp / 8);
    if (L->bpp == 24)
    {
        rgb[0] = p[2];
        rgb[1] = p[1];
        rgb[2] = p[0];
        return;
    }
    unsigned v = rd32(p);
    for (int c = 0; c < 3; c++)
        rgb[c] = (int)((v >> L->shift[c]) & 0xFFu);
}

/* where output pixel i (of n) samples the visible span [first, first + len) of a side of `side` pixels, in 1/65536
 * pixel: the middle of the output pixel, minus a half, kept inside 0 .. side - 1 */
static long long sample_at(int i, int n, long long first, long long len, int side)
{
    long long u = first * 65536 + ((2LL * i + 1) * len * 65536) / (2LL * n) - 32768;
    long long hi = (long long)(side - 1) * 65536;
    return u < 0 ? 0 : (u > hi ? hi : u);
}

int bmp_cover(const unsigned char *file, unsigned size, unsigned int *out, int ow, int oh)
{
    BmpLayout L;
    if (!out || ow <= 0 || oh <= 0 || ow > BMP_MAX_SIDE || oh > BMP_MAX_SIDE || layout(file, size, &L) != 0)
        return -1;
    /* the part of the picture that is shown: as wide as the output's shape allows, centered */
    long long cx0 = 0, cy0 = 0, cw = L.w, ch = L.h;
    if ((long long)L.w * oh > (long long)L.h * ow)
    {
        cw = (long long)L.h * ow / oh;
        if (cw < 1)
            cw = 1;
        cx0 = (L.w - cw) / 2;
    }
    else
    {
        ch = (long long)L.w * oh / ow;
        if (ch < 1)
            ch = 1;
        cy0 = (L.h - ch) / 2;
    }
    for (int y = 0; y < oh; y++)
    {
        long long v = sample_at(y, oh, cy0, ch, L.h);
        int y0 = (int)(v >> 16), y1 = y0 + 1 < L.h ? y0 + 1 : y0, fy = (int)(v & 0xFFFF);
        unsigned int *row = out + (long long)y * ow;
        for (int x = 0; x < ow; x++)
        {
            long long u = sample_at(x, ow, cx0, cw, L.w);
            int x0 = (int)(u >> 16), x1 = x0 + 1 < L.w ? x0 + 1 : x0, fx = (int)(u & 0xFFFF);
            int a[3], b[3], c[3], d[3], o[3];
            src_px(file, &L, x0, y0, a);
            src_px(file, &L, x1, y0, b);
            src_px(file, &L, x0, y1, c);
            src_px(file, &L, x1, y1, d);
            for (int k = 0; k < 3; k++)
            {
                long long top = (long long)a[k] * (65536 - fx) + (long long)b[k] * fx;
                long long bot = (long long)c[k] * (65536 - fx) + (long long)d[k] * fx;
                o[k] = (int)((top * (65536 - fy) + bot * fy + (1LL << 31)) >> 32);
            }
            row[x] = ((unsigned)o[0] << 16) | ((unsigned)o[1] << 8) | (unsigned)o[2];
        }
    }
    return 0;
}
