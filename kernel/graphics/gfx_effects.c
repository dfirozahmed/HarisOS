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
            f = f * f / 256 * (768 - 2 * f) / 256;     /* twice: a long, gentle tail like a gaussian, no visible edge */
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

void gfx_wallpaper_draw_at(const GfxWallpaper *w, int ox, int oy)
{
    GfxClip saved = gfx_get_clip();
    gfx_clip_intersect(ox, oy, w->w, w->h); /* the lights reach past the picture */
    gfx_gradient_v(ox, oy, w->w, w->h, w->top[0], w->top[1], w->top[2], w->bottom[0], w->bottom[1], w->bottom[2]);
    for (int i = 0; i < w->nglow && i < GFX_WALL_GLOWS; i++)
        gfx_glow(ox + w->glow[i].cx, oy + w->glow[i].cy, w->glow[i].r,
                 gfx_rgb(w->glow[i].rgb[0], w->glow[i].rgb[1], w->glow[i].rgb[2]), w->glow[i].alpha);
    for (int i = 0; i < w->nrect && i < GFX_WALL_RECTS; i++)
        gfx_fill_rect(ox + w->rect[i].x, oy + w->rect[i].y, w->rect[i].w, w->rect[i].h,
                      gfx_rgb(w->rect[i].rgb[0], w->rect[i].rgb[1], w->rect[i].rgb[2]));
    gfx_restore_clip(saved);
}

void gfx_wallpaper_draw(const GfxWallpaper *w) { gfx_wallpaper_draw_at(w, 0, 0); }

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

/* ---- backdrop blur: frosted glass (see gfx.h) ---- */
#define BLUR_MAX 2048
#define BLUR_PASSES 3
static unsigned int BLUR_LINE[BLUR_MAX]; /* one row or column being blurred */

int gfx_backdrop_possible(void)
{
    return BACK && !gfx_backbuffer_detached();
}

/* One box blur along a line of n pixels: every pixel becomes the mean of the 2r+1 around it, the end pixels repeated beyond the line.
 * The three colour bytes are blurred each by itself, so the order of red, green and blue does not matter. inv = 65536 / (2r+1). */
static void box_line(const unsigned int *src, int n, int r, unsigned int *dst, int stride, int inv)
{
    int s0 = 0, s1 = 0, s2 = 0;
    for (int k = -r; k <= r; k++)
    {
        unsigned int p = src[k < 0 ? 0 : (k >= n ? n - 1 : k)];
        s0 += (int)(p & 255u);
        s1 += (int)((p >> 8) & 255u);
        s2 += (int)((p >> 16) & 255u);
    }
    for (int i = 0; i < n; i++)
    {
        dst[(long long)i * stride] = (unsigned int)((s0 * inv + 32768) >> 16) | ((unsigned int)((s1 * inv + 32768) >> 16) << 8) |
                                     ((unsigned int)((s2 * inv + 32768) >> 16) << 16);
        unsigned int pa = src[i + r + 1 >= n ? n - 1 : i + r + 1], pd = src[i - r < 0 ? 0 : i - r];
        s0 += (int)(pa & 255u) - (int)(pd & 255u);
        s1 += (int)((pa >> 8) & 255u) - (int)((pd >> 8) & 255u);
        s2 += (int)((pa >> 16) & 255u) - (int)((pd >> 16) & 255u);
    }
}

int gfx_backdrop_capture(int x, int y, int w, int h, int radius, unsigned int *out)
{
    if (!out || w <= 0 || h <= 0 || w > BLUR_MAX || h > BLUR_MAX || !gfx_backdrop_possible())
        return -1;
    if (x < 0 || y < 0 || x + w > W || y + h > H)
        return -1;
    if (radius < 1)
        radius = 1;
    if (radius > 64)
        radius = 64;
    int win = 2 * radius + 1;
    int inv = (65536 + win / 2) / win; /* a uniform area stays exactly as it is: 255 * (2r+1) * inv + 32768 >> 16 == 255 */
    for (int yy = 0; yy < h; yy++)
    {
        const volatile unsigned int *row = DRAW + (long long)(y + yy) * DPITCH + x;
        unsigned int *o = out + (long long)yy * w;
        for (int xx = 0; xx < w; xx++)
            o[xx] = row[xx] & 0x00FFFFFFu;
    }
    for (int pass = 0; pass < BLUR_PASSES; pass++)
    {
        for (int yy = 0; yy < h; yy++) /* along the rows */
        {
            unsigned int *o = out + (long long)yy * w;
            for (int xx = 0; xx < w; xx++)
                BLUR_LINE[xx] = o[xx];
            box_line(BLUR_LINE, w, radius, o, 1, inv);
        }
        for (int xx = 0; xx < w; xx++) /* down the columns */
        {
            for (int yy = 0; yy < h; yy++)
                BLUR_LINE[yy] = out[(long long)yy * w + xx];
            box_line(BLUR_LINE, h, radius, out + xx, w, inv);
        }
    }
    return 0;
}

void gfx_backdrop_draw(const unsigned int *px, int x, int y, int w, int h, int corner)
{
    if (!px || w <= 0 || h <= 0 || !gfx_backdrop_possible())
        return;
    corner = clamp_radius(w, h, corner);
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (!gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = ROW(yy);
        const unsigned int *src = px + (long long)(yy - y) * w - x; /* indexed by the screen's x */
        int corner_row = yy < y + corner || yy >= y + h - corner;
        for (int xx = x0; xx < x1; xx++)
        {
            unsigned int v = src[xx] & 0x00FFFFFFu;
            if (corner_row && (xx < x + corner || xx >= x + w - corner))
            {
                int cov = 8 - rr_sd(xx, yy, x * 16, y * 16, w * 16, h * 16, corner * 16); /* 16 = fully inside, 0 = outside */
                if (cov <= 0)
                    continue;
                if (cov < 16)
                {
                    unsigned char r, g, b;
                    gfx_unpack(v, &r, &g, &b);
                    gfx_internal_blend_ptr(row + xx, r, g, b, cov * 255 / 16);
                    continue;
                }
            }
            row[xx] = v;
        }
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}
