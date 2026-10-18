/* HarisOS line and convex-polygon rasterization.
 * Q4 fixed-point coordinates and integer-only anti-aliasing.
 */
#include "gfx_internal.h"

#define ROW(y) (DRAW + (long long)(y) * DPITCH)

/* ---- anti-aliasing helpers ---------------------------------- */
static unsigned int isqrt64(unsigned long long n)
{
    unsigned long long r = 0, bit = 1ULL << 62;
    while (bit > n)
        bit >>= 2;
    while (bit)
    {
        if (n >= r + bit)
        {
            n -= r + bit;
            r = (r >> 1) + bit;
        }
        else
            r >>= 1;
        bit >>= 2;
    }
    return (unsigned int)r;
}

/* Coverage (0..255) of a pixel whose center is `sd` away from an edge (Q4,
 * negative = inside). The edge is blurred over exactly one pixel. */
static inline int cov_from_sd(int sd)
{
    int v = 8 - sd; /* 0..16 across the 1 px transition */
    if (v <= 0)
        return 0;
    if (v >= 16)
        return 255;
    return v * 255 / 16;
}


/* ---- lines -------------------------------------------------- */
void gfx_line_q(int ax, int ay, int bx, int by, int width_q, GfxColor c, unsigned char alpha)
{
    if (width_q <= 0 || alpha == 0)
        return;
    int hw = width_q / 2;
    int minx = (ax < bx ? ax : bx) - hw, maxx = (ax > bx ? ax : bx) + hw;
    int miny = (ay < by ? ay : by) - hw, maxy = (ay > by ? ay : by) + hw;
    int x0 = minx / 16 - 1, y0 = miny / 16 - 1, x1 = maxx / 16 + 2, y1 = maxy / 16 + 2;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    unsigned char r, g, b;
    gfx_unpack(c, &r, &g, &b);
    long long dx = bx - ax, dy = by - ay;
    long long len2 = dx * dx + dy * dy;
    long long out_lim = (long long)(hw + 8) * (hw + 8);
    long long in_lim = hw > 8 ? (long long)(hw - 8) * (hw - 8) : -1;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        long long py = yy * 16 + 8;
        for (int xx = x0; xx < x1; xx++)
        {
            long long px = xx * 16 + 8;
            long long cxp = ax, cyp = ay; /* closest point on the segment */
            if (len2 > 0)
            {
                long long dot = (px - ax) * dx + (py - ay) * dy;
                if (dot >= len2)
                {
                    cxp = bx;
                    cyp = by;
                }
                else if (dot > 0)
                {
                    cxp = ax + dx * dot / len2;
                    cyp = ay + dy * dot / len2;
                }
            }
            long long ex = px - cxp, ey = py - cyp;
            long long d2 = ex * ex + ey * ey;
            if (d2 >= out_lim)
                continue;
            int cov = d2 <= in_lim ? 255 : cov_from_sd((int)isqrt64((unsigned long long)d2) - hw);
            gfx_internal_blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

void gfx_line(int x0, int y0, int x1, int y1, GfxColor c)
{
    gfx_line_q(x0 * 16 + 8, y0 * 16 + 8, x1 * 16 + 8, y1 * 16 + 8, 16, c, 255);
}


/* ---- convex polygons ---------------------------------------- */
void gfx_fill_poly_q(const int *pts, int n, GfxColor c, unsigned char alpha)
{
    if (!pts || n < 3 || n > 8 || alpha == 0)
        return;
    long long ex[8], ey[8], len[8];
    int minx = pts[0], maxx = pts[0], miny = pts[1], maxy = pts[1];
    long long area2 = 0;
    for (int i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        ex[i] = pts[2 * j] - pts[2 * i];
        ey[i] = pts[2 * j + 1] - pts[2 * i + 1];
        len[i] = (long long)isqrt64((unsigned long long)(ex[i] * ex[i] + ey[i] * ey[i]));
        if (len[i] == 0)
            len[i] = 1;
        area2 += (long long)pts[2 * i] * pts[2 * j + 1] - (long long)pts[2 * j] * pts[2 * i + 1];
        if (pts[2 * i] < minx)
            minx = pts[2 * i];
        if (pts[2 * i] > maxx)
            maxx = pts[2 * i];
        if (pts[2 * i + 1] < miny)
            miny = pts[2 * i + 1];
        if (pts[2 * i + 1] > maxy)
            maxy = pts[2 * i + 1];
    }
    long long sign = area2 >= 0 ? 1 : -1; /* make "inside" the positive side of every edge */
    int x0 = minx / 16 - 1, y0 = miny / 16 - 1, x1 = maxx / 16 + 2, y1 = maxy / 16 + 2;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    unsigned char r, g, b;
    gfx_unpack(c, &r, &g, &b);
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        long long py = yy * 16 + 8;
        for (int xx = x0; xx < x1; xx++)
        {
            long long px = xx * 16 + 8;
            long long best = 1LL << 40; /* smallest inside-distance over all edges (Q4) */
            for (int i = 0; i < n; i++)
            {
                long long cross = (px - pts[2 * i]) * ey[i] - (py - pts[2 * i + 1]) * ex[i];
                long long d = -sign * cross / len[i];
                if (d < best)
                    best = d;
            }
            int cov = cov_from_sd((int)-best);
            gfx_internal_blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}
