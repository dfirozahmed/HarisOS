/* HarisOS graphics layer: see include/gfx.h.
 *
 * No libc and no floating point are used: all anti-aliasing is done with
 * integer "signed distance" math. For every edge pixel we measure how far its
 * center is from the shape's border (in 1/16 pixel units, "Q4") and turn that
 * into a 0..255 coverage value; the pixel is then blended with the background.
 */
#include "gfx.h"
#include "gfx_internal.h"

static volatile unsigned int *FB;    /* the real screen */
volatile unsigned int *DRAW;         /* internal shared draw target */
int W, H;                            /* logical screen dimensions, shared with clipping module */
static int PITCH, DPITCH;            /* PITCH: screen scanline, DPITCH: draw target scanline */
static int BACK;                     /* 1 while double buffering is on */
static volatile unsigned int *CACHE; /* optional copy of the finished wallpaper (see gfx_cache_*) */
static int CACHE_OK;                 /* 1 once gfx_cache_save() filled it */
static int DX0, DY0, DX1, DY1;       /* dirty rectangle (back buffer areas not yet shown) */
static int RED_FIRST;                /* 1: 0x00RRGGBB, 0: 0x00BBGGRR */
int CX0, CY0, CX1, CY1;              /* active clip rectangle, CX1/CY1 exclusive */

/* mouse pointer state (see the pointer section below) */
static int CUR_ON, CUR_DRAWN, CUR_X, CUR_Y;
static void cursor_restore(void);
static void cursor_paint(void);

void gfx_init(void *framebuffer, unsigned int width, unsigned int height,
              unsigned int pitch, unsigned int red_mask_mode)
{
    FB = (volatile unsigned int *)framebuffer;
    W = (int)width;
    H = (int)height;
    PITCH = (int)pitch;
    RED_FIRST = red_mask_mode ? 1 : 0;
    DRAW = FB;
    DPITCH = PITCH;
    BACK = 0;
    CACHE = 0;
    CACHE_OK = 0;
    DX0 = DY0 = DX1 = DY1 = 0;
    CUR_ON = CUR_DRAWN = 0;
    gfx_reset_clip();
}

int gfx_ready(void) { return FB != 0 && W > 0 && H > 0; }
unsigned int gfx_width(void) { return (unsigned int)W; }
unsigned int gfx_height(void) { return (unsigned int)H; }

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

/* 4x4 ordered dither: hides the banding of 8-bit gradients and glows. */
static const unsigned char BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
static inline int dither(int x, int y) { return BAYER[((y & 3) << 2) | (x & 3)] * 4096; }

/* Blend with a 16-bit alpha (0..65535) plus dithering. */
static inline void blend_ptr16(volatile unsigned int *p, int x, int y, int r, int g, int b, int a16)
{
    if (a16 <= 0)
        return;
    unsigned char dr, dg, db;
    gfx_unpack(*p, &dr, &dg, &db);
    int d = dither(x, y), ia = 65536 - a16;
    *p = pack((r * a16 + dr * ia + d) >> 16, (g * a16 + dg * ia + d) >> 16, (b * a16 + db * ia + d) >> 16);
}

/* ---- double buffering --------------------------------------- */
/* Remember which part of the back buffer changed, so gfx_present() only copies that. */
static void mark_dirty(int x0, int y0, int x1, int y1)
{
    if (!BACK || x1 <= x0 || y1 <= y0)
        return;
    if (DX1 <= DX0 || DY1 <= DY0)
    {
        DX0 = x0;
        DY0 = y0;
        DX1 = x1;
        DY1 = y1;
        return;
    }
    if (x0 < DX0)
        DX0 = x0;
    if (y0 < DY0)
        DY0 = y0;
    if (x1 > DX1)
        DX1 = x1;
    if (y1 > DY1)
        DY1 = y1;
}

unsigned long long gfx_backbuffer_size(void)
{
    return (unsigned long long)W * (unsigned long long)H * 4ULL;
}

int gfx_enable_backbuffer(void *memory)
{
    if (!FB || !memory || BACK)
        return -1;
    volatile unsigned int *mem = (volatile unsigned int *)memory;
    /* Start from what is on screen now, so nothing visible jumps. */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            mem[(long long)y * W + x] = FB[(long long)y * PITCH + x];
    DRAW = mem;
    DPITCH = W;
    BACK = 1;
    DX0 = DY0 = DX1 = DY1 = 0;
    return 0;
}

void gfx_present(void)
{
    if (!BACK || DX1 <= DX0 || DY1 <= DY0)
        return;

    extern const int gfx_cursor_h, gfx_cursor_hot;
    int cy0 = 0, cy1 = 0; /* screen rows covered by the pointer */
    if (CUR_DRAWN)
    {
        cy0 = CUR_Y - gfx_cursor_hot;
        cy1 = cy0 + gfx_cursor_h;
    }
    for (int y = DY0; y < DY1; y++)
    {
        if (y >= cy0 && y < cy1)
            continue;
        volatile unsigned int *src = DRAW + (long long)y * DPITCH;
        volatile unsigned int *dst = FB + (long long)y * PITCH;
        for (int x = DX0; x < DX1; x++)
            dst[x] = src[x];
    }
    cursor_restore();
    for (int y = DY0 > cy0 ? DY0 : cy0; y < (DY1 < cy1 ? DY1 : cy1); y++)
    {
        volatile unsigned int *src = DRAW + (long long)y * DPITCH;
        volatile unsigned int *dst = FB + (long long)y * PITCH;
        for (int x = DX0; x < DX1; x++)
            dst[x] = src[x];
    }
    DX0 = DY0 = DX1 = DY1 = 0;
    cursor_paint();
}

void gfx_disable_backbuffer(void)
{
    if (!BACK)
        return;
    /* Show everything pending, then draw straight to the screen again. */
    DX0 = 0;
    DY0 = 0;
    DX1 = W;
    DY1 = H;
    gfx_present();
    DRAW = FB;
    DPITCH = PITCH;
    BACK = 0;
    CACHE = 0;
    CACHE_OK = 0;
}

int gfx_double_buffered(void) { return BACK; }

/* ---- background cache ---------------------------------------- */
int gfx_cache_enable(void *memory)
{
    if (!BACK || !memory)
        return -1;
    CACHE = (volatile unsigned int *)memory;
    CACHE_OK = 0;
    return 0;
}

int gfx_cache_valid(void) { return CACHE != 0 && CACHE_OK; }
void gfx_cache_invalidate(void) { CACHE_OK = 0; }

void gfx_cache_save(void)
{
    if (!CACHE || !BACK)
        return;
    for (int y = 0; y < H; y++)
    {
        volatile unsigned int *src = DRAW + (long long)y * DPITCH;
        volatile unsigned int *dst = CACHE + (long long)y * W;
        for (int x = 0; x < W; x++)
            dst[x] = src[x];
    }
    CACHE_OK = 1;
}

void gfx_cache_restore(int x, int y, int w, int h)
{
    if (!gfx_cache_valid())
        return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *src = CACHE + (long long)yy * W;
        volatile unsigned int *dst = DRAW + (long long)yy * DPITCH;
        for (int xx = x0; xx < x1; xx++)
            dst[xx] = src[xx];
    }
    mark_dirty(x0, y0, x1, y1);
}

/* ---- plain primitives -------------------------------------- */
#define ROW(y) (DRAW + (long long)(y) * DPITCH)

void gfx_pixel(int x, int y, GfxColor c)
{
    if (DRAW && x >= CX0 && x < CX1 && y >= CY0 && y < CY1)
    {
        DRAW[(long long)y * DPITCH + x] = c;
        mark_dirty(x, y, x + 1, y + 1);
    }
}

void gfx_blend_pixel(int x, int y, GfxColor c, unsigned char alpha)
{
    if (!DRAW || x < CX0 || x >= CX1 || y < CY0 || y >= CY1)
        return;
    unsigned char r, g, b;
    gfx_unpack(c, &r, &g, &b);
    blend_ptr(DRAW + (long long)y * DPITCH + x, r, g, b, alpha);
    mark_dirty(x, y, x + 1, y + 1);
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
    mark_dirty(x0, y0, x1, y1);
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
    mark_dirty(x0, y0, x1, y1);
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
                blend_ptr(row + xx, cr, cg, cb, rr_cov(xx, yy, x * 16, y * 16, w * 16, h * 16, r * 16) * alpha / 255);
            for (int xx = (sx1 > x0 ? sx1 : x0); xx < x1; xx++)
                blend_ptr(row + xx, cr, cg, cb, rr_cov(xx, yy, x * 16, y * 16, w * 16, h * 16, r * 16) * alpha / 255);
            if (sx0 < x0)
                sx0 = x0;
            if (sx1 > x1)
                sx1 = x1;
        }
        for (int xx = sx0; xx < sx1; xx++)
            blend_ptr(row + xx, cr, cg, cb, alpha);
    }
    mark_dirty(x0, y0, x1, y1);
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
            blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    mark_dirty(x0, y0, x1, y1);
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
            blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    mark_dirty(x0, y0, x1, y1);
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
            blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    mark_dirty(x0, y0, x1, y1);
}

void gfx_fill_circle(int cx, int cy, int radius, GfxColor c)
{
    gfx_fill_circle_q(cx * 16 + 8, cy * 16 + 8, radius * 16 + 8, c, 255);
}

void gfx_circle(int cx, int cy, int radius, GfxColor c)
{
    gfx_ring_q(cx * 16 + 8, cy * 16 + 8, radius * 16 + 8, 16, c, 255);
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
            blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    mark_dirty(x0, y0, x1, y1);
}

void gfx_line(int x0, int y0, int x1, int y1, GfxColor c)
{
    gfx_line_q(x0 * 16 + 8, y0 * 16 + 8, x1 * 16 + 8, y1 * 16 + 8, 16, c, 255);
}

/* ---- shadow and glow ---------------------------------------- */
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
            int a16 = f * strength * 256 / 255;        /* 0..65536 */
            blend_ptr16(row + xx, xx, yy, 0, 0, 0, a16 > 65535 ? 65535 : a16);
        }
    }
    mark_dirty(x0, y0, x1, y1);
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
            int t = (int)((r2 - d2) * 256 / r2);              /* 256 at the center, 0 at the edge */
            int a16 = (int)(((long long)t * t * alpha) >> 8); /* smooth falloff, 16-bit alpha */
            blend_ptr16(row + xx, xx, yy, r, g, b, a16);
        }
    }
    mark_dirty(x0, y0, x1, y1);
}

/* ---- gradients ---------------------------------------------- */
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
            row[xx] = pack(pr > 255 ? 255 : pr, pg > 255 ? 255 : pg, pb > 255 ? 255 : pb);
        }
    }
    mark_dirty(x0, y0, x1, y1);
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
            row[xx] = pack(pr > 255 ? 255 : pr, pg > 255 ? 255 : pg, pb > 255 ? 255 : pb);
        }
    }
    mark_dirty(x0, y0, x1, y1);
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
            blend_ptr(row + xx, r, g, b, cov * alpha / 255);
        }
    }
    mark_dirty(x0, y0, x1, y1);
}

/* ---- mouse pointer ------------------------------------------ */
extern const int gfx_cursor_w, gfx_cursor_h, gfx_cursor_hot;
extern const unsigned char gfx_cursor_pixels[]; /* per pixel: shade, alpha */

#define CUR_MAX_W 64
#define CUR_MAX_H 64
static unsigned int CUR_SAVE[CUR_MAX_W * CUR_MAX_H]; /* screen pixels under the pointer */

static void cursor_restore(void)
{
    if (!CUR_DRAWN || !FB)
        return;
    int ox = CUR_X - gfx_cursor_hot, oy = CUR_Y - gfx_cursor_hot;
    for (int y = 0; y < gfx_cursor_h; y++)
        for (int x = 0; x < gfx_cursor_w; x++)
        {
            int sx = ox + x, sy = oy + y;
            if (sx >= 0 && sx < W && sy >= 0 && sy < H)
                FB[(long long)sy * PITCH + sx] = CUR_SAVE[y * gfx_cursor_w + x];
        }
    CUR_DRAWN = 0;
}

static void cursor_paint(void)
{
    if (!CUR_ON || CUR_DRAWN || !FB)
        return;
    int ox = CUR_X - gfx_cursor_hot, oy = CUR_Y - gfx_cursor_hot;
    for (int y = 0; y < gfx_cursor_h; y++)
        for (int x = 0; x < gfx_cursor_w; x++)
        {
            int sx = ox + x, sy = oy + y;
            if (sx < 0 || sx >= W || sy < 0 || sy >= H)
                continue;
            volatile unsigned int *p = FB + (long long)sy * PITCH + sx;
            CUR_SAVE[y * gfx_cursor_w + x] = *p;
            int shade = gfx_cursor_pixels[2 * (y * gfx_cursor_w + x)];
            int alpha = gfx_cursor_pixels[2 * (y * gfx_cursor_w + x) + 1];
            blend_ptr(p, shade, shade, shade, alpha);
        }
    CUR_DRAWN = 1;
}

void gfx_cursor_show(int on)
{
    if (on)
    {
        CUR_ON = 1;
        cursor_paint();
    }
    else
    {
        cursor_restore();
        CUR_ON = 0;
    }
}

void gfx_cursor_move(int x, int y)
{
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= W)
        x = W - 1;
    if (y >= H)
        y = H - 1;
    if (x == CUR_X && y == CUR_Y && CUR_DRAWN)
        return;
    cursor_restore();
    CUR_X = x;
    CUR_Y = y;
    cursor_paint();
}
