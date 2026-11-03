/* Host test of kernel/graphics/bmp.c: small pictures built here byte by byte, read back at their own size (every pixel
 * exact), scaled to cover a wider and a taller area, and broken headers refused. */
#include <stdio.h>
#include <string.h>
#include "bmp.h"

static int ok = 1;
static void check(int cond, const char *name)
{
    printf("%s: %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond)
        ok = 0;
}

static void w16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void w32(unsigned char *p, unsigned v) { w16(p, v & 0xFFFF); w16(p + 2, v >> 16); }

/* A w x h picture whose pixel (x, y) (y from the top) is px[y * w + x] = 0x00RRGGBB. hdr: 40 or 124; bitfields: 32-bit
 * with masks (red in the low byte, to see the masks are used). Returns its size. */
static unsigned make(unsigned char *f, int w, int h, int bpp, int top_down, int hdr, int bitfields, const unsigned *px)
{
    unsigned stride = (unsigned)((w * bpp + 31) / 32 * 4), extra = bitfields && hdr == 40 ? 12u : 0u;
    unsigned off = 14u + (unsigned)hdr + extra, size = off + stride * (unsigned)h;
    memset(f, 0, size);
    f[0] = 'B'; f[1] = 'M';
    w32(f + 2, size);
    w32(f + 10, off);
    w32(f + 14, (unsigned)hdr);
    w32(f + 18, (unsigned)w);
    w32(f + 22, (unsigned)(top_down ? -h : h));
    w16(f + 26, 1);
    w16(f + 28, (unsigned)bpp);
    w32(f + 30, bitfields ? 3u : 0u);
    if (bitfields)
    {
        w32(f + 54, 0x000000FFu); /* red */
        w32(f + 58, 0x0000FF00u); /* green */
        w32(f + 62, 0x00FF0000u); /* blue */
    }
    for (int y = 0; y < h; y++)
    {
        unsigned char *row = f + off + stride * (unsigned)(top_down ? y : h - 1 - y);
        for (int x = 0; x < w; x++)
        {
            unsigned v = px[y * w + x], r = v >> 16 & 0xFF, g = v >> 8 & 0xFF, b = v & 0xFF;
            unsigned char *p = row + x * (bpp / 8);
            if (bitfields)
                w32(p, r | g << 8 | b << 16 | 0xFF000000u);
            else if (bpp == 32)
                w32(p, b | g << 8 | r << 16);
            else
            {
                p[0] = (unsigned char)b; p[1] = (unsigned char)g; p[2] = (unsigned char)r;
            }
        }
    }
    return size;
}

int main(void)
{
    static unsigned char f[65536];
    static unsigned px[64 * 64], out[64 * 64];
    int w, h;
    for (int i = 0; i < 64 * 64; i++)
        px[i] = (unsigned)(i * 2654435761u) & 0x00FFFFFFu;

    /* every kind, read at its own size: exact */
    {
        const int kinds[5][4] = {{24, 0, 40, 0}, {24, 1, 40, 0}, {32, 0, 40, 0}, {32, 1, 124, 1}, {32, 0, 40, 1}};
        int all = 1;
        for (int k = 0; k < 5; k++)
        {
            unsigned n = make(f, 7, 5, kinds[k][0], kinds[k][1], kinds[k][2], kinds[k][3], px);
            if (bmp_info(f, n, &w, &h) != 0 || w != 7 || h != 5 || bmp_cover(f, n, out, 7, 5) != 0)
                all = 0;
            else
                for (int i = 0; i < 35; i++)
                    if (out[i] != px[i])
                        all = 0;
        }
        check(all, "24 / 32-bit, bottom-up / top-down, 40 / 124-byte header, bit fields: 7 x 5 (padded rows) read back exactly");
    }

    /* cover: a 4 x 2 picture into 2 x 2 keeps its middle 2 x 2; a 2 x 4 one into 2 x 2 its middle rows */
    {
        unsigned p42[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
        unsigned n = make(f, 4, 2, 24, 0, 40, 0, p42);
        check(bmp_cover(f, n, out, 2, 2) == 0 && out[0] == 0x20 && out[1] == 0x30 && out[2] == 0x60 && out[3] == 0x70,
              "cover: a wide picture is cut equally left and right");
        unsigned p24[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
        n = make(f, 2, 4, 24, 0, 40, 0, p24);
        check(bmp_cover(f, n, out, 2, 2) == 0 && out[0] == 0x30 && out[1] == 0x40 && out[2] == 0x50 && out[3] == 0x60,
              "cover: a tall picture is cut equally above and below");
    }

    /* scaling up mixes neighbours: a 2 x 1 black / white picture across 4 x 2 */
    {
        unsigned p[2] = {0x000000, 0xFFFFFF};
        unsigned n = make(f, 2, 1, 24, 0, 40, 0, p);
        int r = bmp_cover(f, n, out, 8, 4);
        int mono = 1, edges = out[0] == 0 && out[7] == 0xFFFFFF;
        for (int x = 1; x < 8; x++)
            if ((out[x] & 0xFF) < (out[x - 1] & 0xFF))
                mono = 0;
        check(r == 0 && edges && mono && (out[3] & 0xFF) > 0 && (out[3] & 0xFF) < 255 && out[8 * 3 + 3] == out[3],
              "scaling up: black to white rises smoothly, the ends stay black / white, every row the same");
    }

    /* refused: not BM, 16-bit, compressed, a size that does not hold the rows, a zero / huge side */
    {
        unsigned n = make(f, 4, 4, 24, 0, 40, 0, px);
        int refused = 0;
        f[0] = 'X'; refused += bmp_info(f, n, &w, &h) != 0; f[0] = 'B';
        w16(f + 28, 16); refused += bmp_info(f, n, &w, &h) != 0; w16(f + 28, 24);
        w32(f + 30, 1); refused += bmp_info(f, n, &w, &h) != 0; w32(f + 30, 0);
        refused += bmp_info(f, n - 1, &w, &h) != 0;
        w32(f + 18, 0); refused += bmp_info(f, n, &w, &h) != 0;
        w32(f + 18, BMP_MAX_SIDE + 1); refused += bmp_info(f, n, &w, &h) != 0;
        w32(f + 18, 0x7FFFFFFF); refused += bmp_info(f, n, &w, &h) != 0;
        w32(f + 18, 4);
        refused += bmp_cover(f, n, out, 0, 4) != 0;
        check(refused == 8 && bmp_info(f, n, &w, &h) == 0, "refused: no 'BM', 16-bit, RLE, a row missing, width 0 / too big, output 0");
    }

    printf("\nBMP TEST %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
