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

void gfx_put_pixels(int x, int y, const unsigned int *src, int n)
{
    if (n <= 0)
        return;
    int x0 = x, y0 = y, x1 = x + n, y1 = y + 1;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    volatile unsigned int *row = ROW(y0);
    const unsigned int *s = src + (x0 - x);
    for (int xx = x0; xx < x1; xx++, s++)
    {
        unsigned int p = *s;
        row[xx] = RED_FIRST ? (p & 0xFFFFFFu) : ((p & 0xFF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16));
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
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

/* ---- lines and polygons are implemented in gfx_lines.c ---- */

/* ---- curved shapes, effects and gradients live in dedicated modules. ---- */
