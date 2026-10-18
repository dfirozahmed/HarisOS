/* HarisOS anti-aliased rounded rectangles and circles. */
#include "gfx_internal.h"

#define ROW(y) (DRAW + (long long)(y) * DPITCH)

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
static int rr_sd(int px, int py, int x, int y, int w, int h, int r)
{
    int cx = px * 16 + 8, cy = py * 16 + 8;
    int ix0 = x + r, ix1 = x + w - r, iy0 = y + r, iy1 = y + h - r;
    int qx = ix0 - cx > cx - ix1 ? ix0 - cx : cx - ix1; /* <0: inside the straight part */
    int qy = iy0 - cy > cy - iy1 ? iy0 - cy : cy - iy1;
    if (qx > 0 && qy > 0)
        return (int)isqrt64((unsigned long long)((long long)qx * qx + (long long)qy * qy)) - r;
    return (qx > qy ? qx : qy) - r;
}

/* Coverage of pixel (px,py) for a rounded rectangle given in Q4. */
static int rr_cov(int px, int py, int x, int y, int w, int h, int r)
{
    return cov_from_sd(rr_sd(px, py, x, y, w, h, r));
}

static int clamp_radius(int w, int h, int r)
{
    int m = (w < h ? w : h) / 2;
    if (r > m)
        r = m;
    return r < 0 ? 0 : r;
}

/* ---- rounded rectangles ------------------------------------- */
/* Fill with a vertical color ramp top -> bottom (equal colors = flat). */
static void rrect_fill(int x, int y, int w, int h, int r, GfxColor top, GfxColor bottom, int alpha)
{
    if (w <= 0 || h <= 0 || alpha <= 0)
        return;
    r = clamp_radius(w, h, r);
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    unsigned char tr, tg, tb, br, bg, bb;
    gfx_unpack(top, &tr, &tg, &tb);
    gfx_unpack(bottom, &br, &bg, &bb);
    int span = h > 1 ? h - 1 : 1;
    for (int yy = y0; yy < y1; yy++)
    {
        int t = (yy - y) * 255 / span;
        int cr = tr + ((int)br - (int)tr) * t / 255;
        int cg = tg + ((int)bg - (int)tg) * t / 255;
        int cb = tb + ((int)bb - (int)tb) * t / 255;
        volatile unsigned int *row = ROW(yy);
        int corner = yy < y + r || yy >= y + h - r;
        int sx0 = x0, sx1 = x1; /* solid span, always fully covered */
        if (corner)
        {
            sx0 = x + r;
            sx1 = x + w - r;
            for (int xx = x0; xx < x1 && xx < sx0; xx++)
                gfx_internal_blend_ptr(row + xx, cr, cg, cb, rr_cov(xx, yy, x * 16, y * 16, w * 16, h * 16, r * 16) * alpha / 255);
            for (int xx = (sx1 > x0 ? sx1 : x0); xx < x1; xx++)
                gfx_internal_blend_ptr(row + xx, cr, cg, cb, rr_cov(xx, yy, x * 16, y * 16, w * 16, h * 16, r * 16) * alpha / 255);
            if (sx0 < x0)
                sx0 = x0;
            if (sx1 > x1)
                sx1 = x1;
        }
        for (int xx = sx0; xx < sx1; xx++)
            gfx_internal_blend_ptr(row + xx, cr, cg, cb, alpha);
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

void gfx_fill_round_rect_alpha(int x, int y, int w, int h, int radius, GfxColor c, unsigned char alpha)
{
    rrect_fill(x, y, w, h, radius, c, c, alpha);
}

void gfx_fill_round_rect(int x, int y, int w, int h, int radius, GfxColor c)
{
    rrect_fill(x, y, w, h, radius, c, c, 255);
}

void gfx_fill_round_rect_gradient(int x, int y, int w, int h, int radius, GfxColor top, GfxColor bottom)
{
    rrect_fill(x, y, w, h, radius, top, bottom, 255);
}

void gfx_round_rect_alpha(int x, int y, int w, int h, int radius, int thick, GfxColor c, unsigned char alpha)
{
    if (w <= 0 || h <= 0 || thick <= 0 || alpha == 0)
        return;
    radius = clamp_radius(w, h, radius);
    if (thick > (w < h ? w : h) / 2)
        thick = (w < h ? w : h) / 2 ? (w < h ? w : h) / 2 : 1;
    int ir = radius > thick ? radius - thick : 0;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    unsigned char r, g, b;
    gfx_unpack(c, &r, &g, &b);
    int m = (radius > thick ? radius : thick) + 1; /* border band width */
    int iw = w - 2 * thick, ih = h - 2 * thick;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        int middle = yy >= y + m && yy < y + h - m;
        for (int xx = x0; xx < x1; xx++)
        {
            if (middle && xx >= x + m && xx < x + w - m)
            {
                xx = x + w - m - 1; /* skip the hollow center */
                continue;
            }
            int outer = rr_cov(xx, yy, x * 16, y * 16, w * 16, h * 16, radius * 16);
            int inner = (iw > 0 && ih > 0)
                            ? rr_cov(xx, yy, (x + thick) * 16, (y + thick) * 16, iw * 16, ih * 16, ir * 16)
                            : 0;
            int cov = outer * (255 - inner) / 255;
            gfx_internal_blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

void gfx_round_rect(int x, int y, int w, int h, int radius, int thick, GfxColor c)
{
    gfx_round_rect_alpha(x, y, w, h, radius, thick, c, 255);
}

/* ---- circles ------------------------------------------------ */
void gfx_fill_circle_q(int cx_q, int cy_q, int r_q, GfxColor c, unsigned char alpha)
{
    if (r_q <= 0 || alpha == 0)
        return;
    int x0 = (cx_q - r_q) / 16 - 1, y0 = (cy_q - r_q) / 16 - 1;
    int x1 = (cx_q + r_q) / 16 + 2, y1 = (cy_q + r_q) / 16 + 2;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    unsigned char r, g, b;
    gfx_unpack(c, &r, &g, &b);
    long long inner = r_q > 8 ? (long long)(r_q - 8) * (r_q - 8) : -1;
    long long outer = (long long)(r_q + 8) * (r_q + 8);
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        long long dy = yy * 16 + 8 - cy_q;
        for (int xx = x0; xx < x1; xx++)
        {
            long long dx = xx * 16 + 8 - cx_q;
            long long d2 = dx * dx + dy * dy;
            if (d2 >= outer)
                continue;
            int cov = d2 <= inner ? 255 : cov_from_sd((int)isqrt64((unsigned long long)d2) - r_q);
            gfx_internal_blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

void gfx_ring_q(int cx_q, int cy_q, int r_q, int thick_q, GfxColor c, unsigned char alpha)
{
    if (r_q <= 0 || thick_q <= 0 || alpha == 0)
        return;
    int ro = r_q + thick_q / 2, ri = r_q - thick_q / 2;
    int x0 = (cx_q - ro) / 16 - 1, y0 = (cy_q - ro) / 16 - 1;
    int x1 = (cx_q + ro) / 16 + 2, y1 = (cy_q + ro) / 16 + 2;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    unsigned char r, g, b;
    gfx_unpack(c, &r, &g, &b);
    long long out_lim = (long long)(ro + 8) * (ro + 8);
    long long in_lim = ri > 8 ? (long long)(ri - 8) * (ri - 8) : -1;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        long long dy = yy * 16 + 8 - cy_q;
        for (int xx = x0; xx < x1; xx++)
        {
            long long dx = xx * 16 + 8 - cx_q;
            long long d2 = dx * dx + dy * dy;
            if (d2 >= out_lim || d2 < in_lim)
                continue;
            int d = (int)isqrt64((unsigned long long)d2);
            int cov = cov_from_sd(d - ro) - (ri > 0 ? cov_from_sd(d - ri) : 0);
            gfx_internal_blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

void gfx_fill_circle(int cx, int cy, int radius, GfxColor c)
{
    gfx_fill_circle_q(cx * 16 + 8, cy * 16 + 8, radius * 16 + 8, c, 255);
}

void gfx_circle(int cx, int cy, int radius, GfxColor c)
{
    gfx_ring_q(cx * 16 + 8, cy * 16 + 8, radius * 16 + 8, 16, c, 255);
}

