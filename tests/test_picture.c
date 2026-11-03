/* Host test of kernel/graphics/picture.c, png.c and jpeg.c: PNGs of every colour type, bit depth and interlace (made
 * with Python zlib, tests/picture_fixtures.h) read back pixel for pixel, JPEGs (made with .NET System.Drawing, 4:2:0)
 * read back within a small error, file names, scaling to another shape, and broken or unsupported files refused
 * (never a crash). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "picture.h"
#include "picture_fixtures.h"

void *kmalloc(unsigned long long n) { return malloc(n ? n : 1); }
void kfree(void *p) { free(p); }

static int ok = 1;
static void check(int cond, const char *name)
{
    printf("%s: %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond)
        ok = 0;
}

static unsigned rgb(int r, int g, int b) { return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b; }

static unsigned pattern(int x, int y, int w, int h) { return rgb(x * 255 / (w - 1), y * 255 / (h - 1), (x + y) * 255 / (w + h - 2)); }

/* how far (worst channel) the picture read at its own size is from expected(x, y); -1 when it was refused */
static int worst_error(const unsigned char *f, unsigned n, int w, int h, unsigned (*expected)(int, int))
{
    unsigned *out = malloc((size_t)w * h * 4);
    int worst = 0;
    if (picture_cover(f, n, out, w, h) != 0)
    {
        free(out);
        return -1;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            unsigned a = out[y * w + x], b = expected(x, y);
            for (int s = 0; s < 24; s += 8)
            {
                int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
                if (d < 0)
                    d = -d;
                if (d > worst)
                    worst = d;
            }
        }
    free(out);
    return worst;
}

#define PW 19
#define PH 13
static unsigned e_rgb(int x, int y) { return pattern(x, y, PW, PH); }
static unsigned e_rgba(int x, int y)
{
    unsigned p = pattern(x, y, PW, PH);
    int a = x % 2 ? 128 : 255;
    return rgb((int)(p >> 16 & 255) * a / 255, (int)(p >> 8 & 255) * a / 255, (int)(p & 255) * a / 255);
}
static unsigned e_gray(int x, int y)
{
    (void)y;
    int v = x * 255 / (PW - 1);
    return rgb(v, v, v);
}
static unsigned e_pal(int x, int y)
{
    int i = (x + y) % 16;
    return rgb(i * 17, 255 - i * 17, i * 8);
}
static unsigned e_one(int x, int y) { return (x + y) % 2 ? 0xFFFFFFu : 0u; }
static unsigned e_j1(int x, int y) { return pattern(x, y, 40, 24); }
static unsigned e_j2(int x, int y) { return pattern(x, y, 37, 23); }

int main(void)
{
    check(picture_is_name("a.bmp") && picture_is_name("A.PNG") && picture_is_name("x.y.Jpg") && picture_is_name("q.jpeg"), "names: picture types");
    check(!picture_is_name("a.txt") && !picture_is_name("png") && !picture_is_name(".bm") && !picture_is_name(""), "names: others");

    check(worst_error(PNG_RGB, sizeof PNG_RGB, PW, PH, e_rgb) == 0, "png: 8-bit RGB, all five filters, exact");
    check(worst_error(PNG_RGB_ADAM7, sizeof PNG_RGB_ADAM7, PW, PH, e_rgb) == 0, "png: Adam7 interlaced, two IDAT chunks, exact");
    check(worst_error(PNG_RGBA, sizeof PNG_RGBA, PW, PH, e_rgba) == 0, "png: RGBA laid over black");
    check(worst_error(PNG_GRAY16, sizeof PNG_GRAY16, PW, PH, e_gray) == 0, "png: 16-bit grey");
    check(worst_error(PNG_PAL4, sizeof PNG_PAL4, PW, PH, e_pal) == 0, "png: 4-bit palette");
    check(worst_error(PNG_GRAY1, sizeof PNG_GRAY1, PW, PH, e_one) == 0, "png: 1-bit grey");

    int e1 = worst_error(JPEG_J1, sizeof JPEG_J1, 40, 24, e_j1), e2 = worst_error(JPEG_J2, sizeof JPEG_J2, 37, 23, e_j2);
    printf("  (jpeg worst errors: %d and %d)\n", e1, e2);
    check(e1 >= 0 && e1 <= 16, "jpeg: 40x24 4:2:0 close to the original");
    check(e2 >= 0 && e2 <= 16, "jpeg: 37x23 (not a whole number of MCUs) close to the original");

    /* scaling: a wide area from a taller picture keeps the middle, the corners of the original are cut off */
    unsigned out[64 * 16];
    check(picture_cover(PNG_RGB, sizeof PNG_RGB, out, 64, 16) == 0, "cover: PNG into a wide area");
    check(((out[8 * 64 + 32] >> 8) & 255) > 100 && ((out[8 * 64 + 32] >> 8) & 255) < 155, "cover: middle row stays in the middle");
    check(((out[0] >> 8) & 255) > 20, "cover: top row is not the top of the picture (cut off)");
    check(picture_cover(JPEG_J1, sizeof JPEG_J1, out, 64, 16) == 0, "cover: JPEG into a wide area");
    check(picture_cover(JPEG_J1, sizeof JPEG_J1, out, 8, 8) == 0, "cover: JPEG made smaller");

    /* broken or unsupported files */
    unsigned char bad[1024];
    unsigned *o = malloc(PW * PH * 4);
    memcpy(bad, PNG_RGB, sizeof PNG_RGB);
    bad[1] = 'X';
    check(picture_cover(bad, sizeof PNG_RGB, o, PW, PH) != 0, "bad: PNG signature");
    check(picture_cover(PNG_RGB, 60, o, PW, PH) != 0, "bad: PNG cut short before its data");
    check(picture_cover(PNG_RGB, sizeof PNG_RGB - 60, o, PW, PH) != 0, "bad: PNG data cut short");
    memcpy(bad, PNG_RGB, sizeof PNG_RGB);
    bad[16 + 3] = 0; /* width 0x00000013 -> 0 */
    bad[16 + 2] = 0;
    bad[16 + 3] = 0;
    check(picture_cover(bad, sizeof PNG_RGB, o, PW, PH) != 0, "bad: PNG of width 0");
    memcpy(bad, PNG_RGB, sizeof PNG_RGB);
    bad[24] = 7; /* bit depth 7 */
    check(picture_cover(bad, sizeof PNG_RGB, o, PW, PH) != 0, "bad: PNG bit depth");
    memcpy(bad, JPEG_J1, sizeof JPEG_J1);
    for (unsigned i = 0; i + 1 < sizeof JPEG_J1; i++)
        if (bad[i] == 0xFF && bad[i + 1] == 0xC0)
            bad[i + 1] = 0xC2; /* SOF2: progressive */
    check(picture_cover(bad, sizeof JPEG_J1, o, PW, PH) != 0, "bad: progressive JPEG refused");
    check(picture_cover(JPEG_J1, 300, o, PW, PH) != 0, "bad: JPEG cut off before its scan");
    check(picture_cover((const unsigned char *)"GIF89a....", 10, o, PW, PH) != 0, "bad: other file type");

    /* damaged data: whatever comes back, nothing crashes (run under many single-byte changes) */
    srand(7);
    for (int t = 0; t < 3000; t++)
    {
        const unsigned char *src = t % 3 == 0 ? JPEG_J1 : t % 3 == 1 ? PNG_RGB : PNG_RGB_ADAM7;
        unsigned n = t % 3 == 0 ? sizeof JPEG_J1 : t % 3 == 1 ? sizeof PNG_RGB : sizeof PNG_RGB_ADAM7;
        memcpy(bad, src, n);
        for (int k = 0; k < 1 + t % 4; k++)
            bad[rand() % n] = (unsigned char)rand();
        unsigned *big = malloc((size_t)16 * 16 * 4);
        picture_cover(bad, n, big, 16, 16);
        free(big);
    }
    check(1, "fuzz: 3000 damaged files, no crash");
    free(o);
    return ok ? 0 : 1;
}
