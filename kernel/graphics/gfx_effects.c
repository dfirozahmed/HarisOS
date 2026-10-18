/* HarisOS gradient, shadow and glow effects. */
#include "gfx_internal.h"

#define ROW(y) (DRAW + (long long)(y) * DPITCH)

/* Same 4x4 ordered dither pattern used by the original gfx.c implementation. */
static const unsigned char BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
static inline int dither(int x, int y) { return BAYER[((y & 3) << 2) | (x & 3)] * 4096; }

static inline void blend_ptr16(volatile unsigned int *p, int x, int y, int r, int g, int b, int a16)
{
    if (a16 <= 0) return;
    unsigned char dr, dg, db;
    gfx_unpack(*p, &dr, &dg, &db);
    int d = dither(x, y), ia = 65536 - a16;
    *p = gfx_rgb((unsigned char)((r * a16 + dr * ia + d) >> 16),
                 (unsigned char)((g * a16 + dg * ia + d) >> 16),
                 (unsigned char)((b * a16 + db * ia + d) >> 16));
}

static unsigned int isqrt64(unsigned long long n)
{
    unsigned long long r = 0, bit = 1ULL << 62;
    while (bit > n) bit >>= 2;
    while (bit)
    {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (unsigned int)r;
}

static int clamp_radius(int w, int h, int r)
{
    int m = (w < h ? w : h) / 2;
    if (r > m) r = m;
    return r < 0 ? 0 : r;
}

static int rr_sd(int px, int py, int x, int y, int w, int h, int r)
{
    int cx = px * 16 + 8, cy = py * 16 + 8;
    int ix0 = x + r, ix1 = x + w - r, iy0 = y + r, iy1 = y + h - r;
    int qx = ix0 - cx > cx - ix1 ? ix0 - cx : cx - ix1;
    int qy = iy0 - cy > cy - iy1 ? iy0 - cy : cy - iy1;
    if (qx > 0 && qy > 0)
        return (int)isqrt64((unsigned long long)((long long)qx * qx + (long long)qy * qy)) - r;
    return (qx > qy ? qx : qy) - r;
}

void gfx_shadow(int x, int y, int w, int h, int radius, int blur, int dx, int dy, unsigned char strength)
{
    if (w <= 0 || h <= 0 || blur < 1 || strength == 0)
        return;
    radius = clamp_radius(w, h, radius);
    int sx = x + dx, sy = y + dy;
    int x0 = sx - blur, y0 = sy - blur, x1 = sx + w + blur, y1 = sy + h + blur;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    int B = blur * 16;
    /* the shape itself will cover this inner rectangle, so skip it */
    int hx0 = x + radius, hy0 = y + radius, hx1 = x + w - radius, hy1 = y + h - radius;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        for (int xx = x0; xx < x1; xx++)
        {
            if (xx >= hx0 && xx < hx1 && yy >= hy0 && yy < hy1)
            {
                xx = hx1 - 1; /* jump over the hidden part of this row */
                continue;
            }
            int sd = rr_sd(xx, yy, sx * 16, sy * 16, w * 16, h * 16, radius * 16);
            int u = (B / 2 - sd) * 256 / B; /* 256 inside, 0 beyond the blur, centered on the edge */
            if (u <= 0)
                continue;
            if (u > 256)
                u = 256;
            int f = u * u / 256 * (768 - 2 * u) / 256; /* smoothstep: u^2 (3 - 2u) */
            int a16 = f * strength * 256 / 255; /* 0..65536 */
            blend_ptr16(row + xx, xx, yy, 0, 0, 0, a16 > 65535 ? 65535 : a16);
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

void gfx_glow(int cx, int cy, int radius, GfxColor c, unsigned char alpha)
{
    if (radius <= 0 || alpha == 0)
        return;
    int x0 = cx - radius, y0 = cy - radius, x1 = cx + radius + 1, y1 = cy + radius + 1;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    unsigned char r, g, b;
    gfx_unpack(c, &r, &g, &b);
    long long r2 = (long long)radius * radius;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        long long dy = yy - cy;
        for (int xx = x0; xx < x1; xx++)
        {
            long long dx = xx - cx;
            long long d2 = dx * dx + dy * dy;
            if (d2 >= r2)
                continue;
            int t = (int)((r2 - d2) * 256 / r2);       /* 256 at the center, 0 at the edge */
            int a16 = (int)(((long long)t * t * alpha) >> 8); /* smooth falloff, 16-bit alpha */
            blend_ptr16(row + xx, xx, yy, r, g, b, a16);
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

void gfx_gradient_v(int x, int y, int w, int h,
                    unsigned char r1, unsigned char g1, unsigned char b1,
                    unsigned char r2, unsigned char g2, unsigned char b2)
{
    if (w <= 0 || h <= 0)
        return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    int span = h > 1 ? h - 1 : 1;
    for (int yy = y0; yy < y1; yy++)
    {
        int t = (int)((long long)(yy - y) * 65535 / span);
        int r = r1 * 65536 + ((int)r2 - (int)r1) * t;
        int g = g1 * 65536 + ((int)g2 - (int)g1) * t;
        int b = b1 * 65536 + ((int)b2 - (int)b1) * t;
        volatile unsigned int *row = ROW(yy);
        for (int xx = x0; xx < x1; xx++)
        {
            int d = dither(xx, yy);
            int pr = (r + d) >> 16, pg = (g + d) >> 16, pb = (b + d) >> 16;
            row[xx] = gfx_rgb((unsigned char)(pr > 255 ? 255 : pr), (unsigned char)(pg > 255 ? 255 : pg), (unsigned char)(pb > 255 ? 255 : pb));
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

void gfx_gradient_h(int x, int y, int w, int h,
                    unsigned char r1, unsigned char g1, unsigned char b1,
                    unsigned char r2, unsigned char g2, unsigned char b2)
{
    if (w <= 0 || h <= 0)
        return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    int span = w > 1 ? w - 1 : 1;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        for (int xx = x0; xx < x1; xx++)
        {
            int t = (int)((long long)(xx - x) * 65535 / span);
            int d = dither(xx, yy);
            int pr = (r1 * 65536 + ((int)r2 - (int)r1) * t + d) >> 16;
            int pg = (g1 * 65536 + ((int)g2 - (int)g1) * t + d) >> 16;
            int pb = (b1 * 65536 + ((int)b2 - (int)b1) * t + d) >> 16;
            row[xx] = gfx_rgb((unsigned char)(pr > 255 ? 255 : pr), (unsigned char)(pg > 255 ? 255 : pg), (unsigned char)(pb > 255 ? 255 : pb));
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}
