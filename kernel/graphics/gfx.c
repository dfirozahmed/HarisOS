/* HarisOS graphics layer: see include/gfx.h.
 *
 * No libc is available, so row writes go through volatile pointers; this
 * stops the compiler from turning fill loops into calls to memset(). */
#include "gfx.h"

static volatile unsigned int *FB;   /* the real screen */
static volatile unsigned int *DRAW; /* where primitives draw: FB, or the back buffer */
static int W, H, PITCH, DPITCH;     /* PITCH: screen scanline, DPITCH: draw target scanline */
static int BACK;                    /* 1 while double buffering is on */
static int DX0, DY0, DX1, DY1;      /* dirty rectangle (back buffer areas not yet shown) */
static int RED_FIRST;               /* 1: 0x00RRGGBB, 0: 0x00BBGGRR */
static int CX0, CY0, CX1, CY1;      /* active clip rectangle, CX1/CY1 exclusive */

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
    DX0 = DY0 = DX1 = DY1 = 0;
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

/* ---- clipping ---------------------------------------------- */
void gfx_reset_clip(void)
{
    CX0 = 0;
    CY0 = 0;
    CX1 = W;
    CY1 = H;
}

void gfx_set_clip(int x, int y, int w, int h)
{
    CX0 = x < 0 ? 0 : x;
    CY0 = y < 0 ? 0 : y;
    CX1 = (w <= 0 || x + w > W) ? (w <= 0 ? CX0 : W) : x + w;
    CY1 = (h <= 0 || y + h > H) ? (h <= 0 ? CY0 : H) : y + h;
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
    for (int y = DY0; y < DY1; y++)
    {
        volatile unsigned int *src = DRAW + (long long)y * DPITCH;
        volatile unsigned int *dst = FB + (long long)y * PITCH;
        for (int x = DX0; x < DX1; x++)
            dst[x] = src[x];
    }
    DX0 = DY0 = DX1 = DY1 = 0;
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
}

int gfx_double_buffered(void) { return BACK; }

/* ---- primitives -------------------------------------------- */
void gfx_pixel(int x, int y, GfxColor c)
{
    if (DRAW && x >= CX0 && x < CX1 && y >= CY0 && y < CY1)
    {
        DRAW[(long long)y * DPITCH + x] = c;
        mark_dirty(x, y, x + 1, y + 1);
    }
}

void gfx_fill_rect(int x, int y, int w, int h, GfxColor c)
{
    if (!DRAW || w <= 0 || h <= 0)
        return;
    int x0 = x < CX0 ? CX0 : x;
    int y0 = y < CY0 ? CY0 : y;
    int x1 = x + w > CX1 ? CX1 : x + w;
    int y1 = y + h > CY1 ? CY1 : y + h;
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = DRAW + (long long)yy * DPITCH;
        for (int xx = x0; xx < x1; xx++)
            row[xx] = c;
    }
    mark_dirty(x0, y0, x1, y1);
}

void gfx_clear(GfxColor c)
{
    int sx0 = CX0, sy0 = CY0, sx1 = CX1, sy1 = CY1;
    gfx_reset_clip();
    gfx_fill_rect(0, 0, W, H, c);
    CX0 = sx0;
    CY0 = sy0;
    CX1 = sx1;
    CY1 = sy1;
}

void gfx_hline(int x, int y, int len, GfxColor c) { gfx_fill_rect(x, y, len, 1, c); }
void gfx_vline(int x, int y, int len, GfxColor c) { gfx_fill_rect(x, y, 1, len, c); }

void gfx_rect(int x, int y, int w, int h, GfxColor c)
{
    if (w <= 0 || h <= 0)
        return;
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

void gfx_line(int x0, int y0, int x1, int y1, GfxColor c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0; /* negative */
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;)
    {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_circle(int cx, int cy, int r, GfxColor c)
{
    if (r < 0)
        return;
    int x = r, y = 0, err = 1 - r; /* midpoint circle algorithm */
    while (x >= y)
    {
        gfx_pixel(cx + x, cy + y, c);
        gfx_pixel(cx + y, cy + x, c);
        gfx_pixel(cx - y, cy + x, c);
        gfx_pixel(cx - x, cy + y, c);
        gfx_pixel(cx - x, cy - y, c);
        gfx_pixel(cx - y, cy - x, c);
        gfx_pixel(cx + y, cy - x, c);
        gfx_pixel(cx + x, cy - y, c);
        y++;
        if (err < 0)
            err += 2 * y + 1;
        else
        {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

/* Half-width of a circle of radius r at vertical distance dy from its center. */
static int circle_half_width(int r, int dy)
{
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r)
        dx++;
    return dx;
}

void gfx_fill_circle(int cx, int cy, int radius, GfxColor c)
{
    if (radius <= 0)
        return;
    for (int dy = -radius; dy <= radius; dy++)
    {
        int hw = circle_half_width(radius, dy < 0 ? -dy : dy);
        gfx_hline(cx - hw, cy + dy, 2 * hw + 1, c);
    }
}

void gfx_fill_round_rect(int x, int y, int w, int h, int radius, GfxColor c)
{
    if (w <= 0 || h <= 0)
        return;
    int maxr = (w < h ? w : h) / 2;
    if (radius > maxr)
        radius = maxr;
    if (radius <= 0)
    {
        gfx_fill_rect(x, y, w, h, c);
        return;
    }
    gfx_fill_rect(x, y + radius, w, h - 2 * radius, c);
    for (int i = 0; i < radius; i++)
    {
        int inset = radius - circle_half_width(radius, radius - i);
        gfx_hline(x + inset, y + i, w - 2 * inset, c);
        gfx_hline(x + inset, y + h - 1 - i, w - 2 * inset, c);
    }
}

void gfx_fill_rect_alpha(int x, int y, int w, int h,
                         unsigned char r, unsigned char g, unsigned char b,
                         unsigned char alpha)
{
    if (!DRAW || w <= 0 || h <= 0)
        return;
    int x0 = x < CX0 ? CX0 : x;
    int y0 = y < CY0 ? CY0 : y;
    int x1 = x + w > CX1 ? CX1 : x + w;
    int y1 = y + h > CY1 ? CY1 : y + h;
    unsigned int a = alpha, ia = 255u - alpha;
    mark_dirty(x0, y0, x1, y1);
    for (int yy = y0; yy < y1; yy++)
    {
        volatile unsigned int *row = DRAW + (long long)yy * DPITCH;
        for (int xx = x0; xx < x1; xx++)
        {
            unsigned char dr, dg, db;
            gfx_unpack(row[xx], &dr, &dg, &db);
            row[xx] = gfx_rgb((unsigned char)((r * a + dr * ia) / 255u),
                              (unsigned char)((g * a + dg * ia) / 255u),
                              (unsigned char)((b * a + db * ia) / 255u));
        }
    }
}

void gfx_gradient_v(int x, int y, int w, int h,
                    unsigned char r1, unsigned char g1, unsigned char b1,
                    unsigned char r2, unsigned char g2, unsigned char b2)
{
    if (w <= 0 || h <= 0)
        return;
    for (int i = 0; i < h; i++)
    {
        int d = h > 1 ? h - 1 : 1;
        unsigned char r = (unsigned char)(r1 + ((int)r2 - (int)r1) * i / d);
        unsigned char g = (unsigned char)(g1 + ((int)g2 - (int)g1) * i / d);
        unsigned char b = (unsigned char)(b1 + ((int)b2 - (int)b1) * i / d);
        gfx_fill_rect(x, y + i, w, 1, gfx_rgb(r, g, b));
    }
}

/* ---- text -------------------------------------------------- */
/* Compact 5x7 font. Each glyph is 7 rows; bit 4 is the leftmost pixel. */
static const unsigned char *glyph(char c)
{
    static const unsigned char blank[7] = {0, 0, 0, 0, 0, 0, 0};
    static const unsigned char font[][7] = {
        {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14}, {7, 2, 2, 2, 18, 18, 12}, {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31}, {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4}, {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17}, {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31}, {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30}, {2, 6, 10, 18, 31, 2, 2}, {31, 16, 16, 30, 1, 1, 30}, {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8}, {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14}};
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return font[c - 'A'];
    if (c >= '0' && c <= '9')
        return font[26 + c - '0'];
    static const struct
    {
        char ch;
        unsigned char rows[7];
    } extra[] = {
        {'.', {0, 0, 0, 0, 0, 12, 12}},
        {':', {0, 12, 12, 0, 12, 12, 0}},
        {'-', {0, 0, 0, 31, 0, 0, 0}},
        {'/', {1, 1, 2, 4, 8, 16, 16}},
        {'_', {0, 0, 0, 0, 0, 0, 31}},
        {'!', {4, 4, 4, 4, 4, 0, 4}},
        {'+', {0, 4, 4, 31, 4, 4, 0}},
    };
    for (unsigned int i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
        if (extra[i].ch == c)
            return extra[i].rows;
    return blank;
}

void gfx_draw_char(int x, int y, char ch, GfxColor c, int scale)
{
    if (scale < 1)
        scale = 1;
    const unsigned char *g = glyph(ch);
    for (int yy = 0; yy < GFX_FONT_H; yy++)
        for (int xx = 0; xx < GFX_FONT_W; xx++)
            if (g[yy] & (1 << (GFX_FONT_W - 1 - xx)))
                gfx_fill_rect(x + xx * scale, y + yy * scale, scale, scale, c);
}

void gfx_draw_text(int x, int y, const char *s, GfxColor c, int scale)
{
    if (!s)
        return;
    if (scale < 1)
        scale = 1;
    int cx = x;
    for (; *s; s++)
    {
        if (*s == '\n')
        {
            cx = x;
            y += (GFX_FONT_H + 2) * scale;
            continue;
        }
        gfx_draw_char(cx, y, *s, c, scale);
        cx += (GFX_FONT_W + 1) * scale;
    }
}

int gfx_text_width(const char *s, int scale)
{
    int best = 0, cur = 0;
    if (!s)
        return 0;
    if (scale < 1)
        scale = 1;
    for (; *s; s++)
    {
        if (*s == '\n')
            cur = 0;
        else
            cur++;
        if (cur > best)
            best = cur;
    }
    return best ? best * (GFX_FONT_W + 1) * scale - scale : 0;
}

int gfx_text_height(const char *s, int scale)
{
    int lines = 1;
    if (!s || !*s)
        return 0;
    if (scale < 1)
        scale = 1;
    for (; *s; s++)
        if (*s == '\n')
            lines++;
    return lines * (GFX_FONT_H + 2) * scale - 2 * scale;
}
