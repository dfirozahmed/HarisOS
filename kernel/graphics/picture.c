/* HarisOS pictures: see include/picture.h. The type is found from the first bytes of the file: "BM" is a BMP, the
 * 8-byte PNG signature a PNG, 0xFF 0xD8 a JPEG. BMP is read and scaled straight from the file (bmp.c); PNG and JPEG are
 * decoded into a pixel block first and then scaled the same way ("cover": filled, centred, the overhang cut off,
 * bilinear - the same arithmetic as bmp_cover, so a picture looks the same whatever file type it came in). */
#include "bmp.h"
#include "heap.h"
#include "picture.h"
#include "picture_internal.h"

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int ends_with(const char *s, const char *end)
{
    int n = 0, m = 0;
    while (s[n])
        n++;
    while (end[m])
        m++;
    if (n < m)
        return 0;
    for (int i = 0; i < m; i++)
        if (lower(s[n - m + i]) != end[i])
            return 0;
    return 1;
}

int picture_is_name(const char *name)
{
    return ends_with(name, ".bmp") || ends_with(name, ".png") || ends_with(name, ".jpg") || ends_with(name, ".jpeg");
}

/* where output pixel i (of n) samples the visible span [first, first + len) of a side of `side` pixels, in 1/65536
 * pixel: the middle of the output pixel, minus a half, kept inside 0 .. side - 1 */
static long long sample_at(int i, int n, long long first, long long len, int side)
{
    long long u = first * 65536 + ((2LL * i + 1) * len * 65536) / (2LL * n) - 32768;
    long long hi = (long long)(side - 1) * 65536;
    return u < 0 ? 0 : (u > hi ? hi : u);
}

/* one colour channel (shift 0, 8 or 16) of the four neighbours mixed by the fractions fx, fy (1/65536) */
static unsigned mix(unsigned a, unsigned b, unsigned c, unsigned d, int fx, int fy, int shift)
{
    long long A = (a >> shift) & 255, B = (b >> shift) & 255, C = (c >> shift) & 255, D = (d >> shift) & 255;
    long long top = A * (65536 - fx) + B * fx, bot = C * (65536 - fx) + D * fx;
    return (unsigned)((top * (65536 - fy) + bot * fy + (1LL << 31)) >> 32);
}

static void cover_pixels(const unsigned int *src, int sw, int sh, unsigned int *out, int ow, int oh)
{
    long long cx0 = 0, cy0 = 0, cw = sw, ch = sh;
    if ((long long)sw * oh > (long long)sh * ow)
    {
        cw = (long long)sh * ow / oh;
        if (cw < 1)
            cw = 1;
        cx0 = (sw - cw) / 2;
    }
    else
    {
        ch = (long long)sw * oh / ow;
        if (ch < 1)
            ch = 1;
        cy0 = (sh - ch) / 2;
    }
    for (int y = 0; y < oh; y++)
    {
        long long v = sample_at(y, oh, cy0, ch, sh);
        int y0 = (int)(v >> 16), y1 = y0 + 1 < sh ? y0 + 1 : y0, fy = (int)(v & 0xFFFF);
        const unsigned int *r0 = src + (long long)y0 * sw, *r1 = src + (long long)y1 * sw;
        unsigned int *row = out + (long long)y * ow;
        for (int x = 0; x < ow; x++)
        {
            long long u = sample_at(x, ow, cx0, cw, sw);
            int x0 = (int)(u >> 16), x1 = x0 + 1 < sw ? x0 + 1 : x0, fx = (int)(u & 0xFFFF);
            unsigned a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
            row[x] = (mix(a, b, c, d, fx, fy, 16) << 16) | (mix(a, b, c, d, fx, fy, 8) << 8) | mix(a, b, c, d, fx, fy, 0);
        }
    }
}

int picture_cover(const unsigned char *file, unsigned size, unsigned int *out, int ow, int oh)
{
    if (!file || !out || ow <= 0 || oh <= 0 || ow > BMP_MAX_SIDE || oh > BMP_MAX_SIDE || size < 4)
        return -1;
    if (file[0] == 'B' && file[1] == 'M')
        return bmp_cover(file, size, out, ow, oh);
    int w = 0, h = 0;
    unsigned int *px = 0;
    if (file[0] == 0x89 && file[1] == 'P')
        px = png_decode(file, size, &w, &h);
    else if (file[0] == 0xFF && file[1] == 0xD8)
        px = jpeg_decode(file, size, &w, &h);
    if (!px)
        return -1;
    cover_pixels(px, w, h, out, ow, oh);
    kfree(px);
    return 0;
}

unsigned int *picture_decode(const unsigned char *file, unsigned size, int *w, int *h)
{
    if (!file || !w || !h || size < 4)
        return 0;
    int pw = 0, ph = 0;
    unsigned int *px = 0;
    if (file[0] == 'B' && file[1] == 'M')
    {
        /* bmp_cover to the picture's own size samples exactly at the pixel centres, so it is a plain copy */
        if (bmp_info(file, size, &pw, &ph) != 0 || (unsigned long long)pw * (unsigned)ph > PICTURE_MAX_PIXELS)
            return 0;
        px = kmalloc((unsigned long long)pw * (unsigned)ph * 4);
        if (!px)
            return 0;
        if (bmp_cover(file, size, px, pw, ph) != 0)
        {
            kfree(px);
            return 0;
        }
    }
    else
    {
        if (file[0] == 0x89 && file[1] == 'P')
            px = png_decode(file, size, &pw, &ph);
        else if (file[0] == 0xFF && file[1] == 0xD8)
            px = jpeg_decode(file, size, &pw, &ph);
        if (!px)
            return 0;
        if (pw <= 0 || ph <= 0 || (unsigned long long)pw * (unsigned)ph > PICTURE_MAX_PIXELS)
        {
            kfree(px);
            return 0;
        }
    }
    *w = pw;
    *h = ph;
    return px;
}
