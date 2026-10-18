/* HarisOS graphics layer: see include/gfx.h.
 *
 * No libc and no floating point are used: all anti-aliasing is done with
 * integer "signed distance" math. For every edge pixel we measure how far its
 * center is from the shape's border (in 1/16 pixel units, "Q4") and turn that
 * into a 0..255 coverage value; the pixel is then blended with the background.
 */
#include "gfx.h"
#include "gfx_internal.h"

int CX0, CY0, CX1, CY1; /* active clip rectangle, CX1/CY1 exclusive */

/* ---- colors ------------------------------------------------- */
static inline unsigned int pack(int r, int g, int b)
{
    return RED_FIRST ? ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b
                     : ((unsigned int)b << 16) | ((unsigned int)g << 8) | (unsigned int)r;
}

GfxColor gfx_rgb(unsigned char r, unsigned char g, unsigned char b) { return pack(r, g, b); }

void gfx_unpack(GfxColor c, unsigned char *r, unsigned char *g, unsigned char *b)
{
    *g = (unsigned char)(c >> 8);
    if (RED_FIRST)
    {
        *r = (unsigned char)(c >> 16);
        *b = (unsigned char)c;
    }
    else
    {
        *b = (unsigned char)(c >> 16);
        *r = (unsigned char)c;
    }
}

static inline int div255(int v) { return ((v + 128) * 257) >> 16; }

GfxColor gfx_mix(GfxColor a, GfxColor b, unsigned char t)
{
    unsigned char ar, ag, ab, br, bg, bb;
    gfx_unpack(a, &ar, &ag, &ab);
    gfx_unpack(b, &br, &bg, &bb);
    int it = 255 - t;
    return pack(div255(ar * it + br * t), div255(ag * it + bg * t), div255(ab * it + bb * t));
}

/* Blend r,g,b over the pixel at p with alpha a (0..255). No clipping here. */
static inline void blend_ptr(volatile unsigned int *p, int r, int g, int b, int a)
{
    if (a <= 0)
        return;
    if (a >= 255)
    {
        *p = pack(r, g, b);
        return;
    }
    unsigned char dr, dg, db;
    gfx_unpack(*p, &dr, &dg, &db);
    int ia = 255 - a;
    *p = pack(div255(r * a + dr * ia), div255(g * a + dg * ia), div255(b * a + db * ia));
}

/* Internal shared primitive for anti-aliased shape modules. */
void gfx_internal_blend_ptr(volatile unsigned int *p, int r, int g, int b, int alpha)
{
    blend_ptr(p, r, g, b, alpha);
}

/* Blend with a 16-bit alpha (0..65535) plus dithering. */

/* ---- plain primitives -------------------------------------- */
#define ROW(y) (DRAW + (long long)(y) * DPITCH)

void gfx_pixel(int x, int y, GfxColor c)
{
    if (DRAW && x >= CX0 && x < CX1 && y >= CY0 && y < CY1)
    {
        DRAW[(long long)y * DPITCH + x] = c;
        gfx_internal_mark_dirty(x, y, x + 1, y + 1);
    }
}

void gfx_blend_pixel(int x, int y, GfxColor c, unsigned char alpha)
{
    if (!DRAW || x < CX0 || x >= CX1 || y < CY0 || y >= CY1)
        return;
    unsigned char r, g, b;
    gfx_unpack(c, &r, &g, &b);
    blend_ptr(DRAW + (long long)y * DPITCH + x, r, g, b, alpha);
    gfx_internal_mark_dirty(x, y, x + 1, y + 1);
}

void gfx_fill_rect(int x, int y, int w, int h, GfxColor c)
{
    if (w <= 0 || h <= 0)
        return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        for (int xx = x0; xx < x1; xx++)
            row[xx] = c;
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

void gfx_clear(GfxColor c)
{
    GfxClip saved = gfx_get_clip();
    gfx_reset_clip();
    gfx_fill_rect(0, 0, W, H, c);
    gfx_restore_clip(saved);
}

void gfx_hline(int x, int y, int len, GfxColor c) { gfx_fill_rect(x, y, len, 1, c); }
void gfx_vline(int x, int y, int len, GfxColor c) { gfx_fill_rect(x, y, 1, len, c); }

void gfx_rect(int x, int y, int w, int h, GfxColor c)
{
    if (w <= 0 || h <= 0)
        return;
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y + 1, h - 2, c);
    gfx_vline(x + w - 1, y + 1, h - 2, c);
}

void gfx_fill_rect_alpha(int x, int y, int w, int h,
                         unsigned char r, unsigned char g, unsigned char b,
                         unsigned char alpha)
{
    if (w <= 0 || h <= 0)
        return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        for (int xx = x0; xx < x1; xx++)
            blend_ptr(row + xx, r, g, b, alpha);
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

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

/* Signed distance (Q4, negative = inside) from the center of pixel (px,py) to a
 * rounded rectangle given in Q4. */

/* Coverage of pixel (px,py) for a rounded rectangle given in Q4. */

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
            blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

void gfx_line(int x0, int y0, int x1, int y1, GfxColor c)
{
    gfx_line_q(x0 * 16 + 8, y0 * 16 + 8, x1 * 16 + 8, y1 * 16 + 8, 16, c, 255);
}

/* ---- shadow and glow ---------------------------------------- */

/* ---- gradients ---------------------------------------------- */

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
            blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}
