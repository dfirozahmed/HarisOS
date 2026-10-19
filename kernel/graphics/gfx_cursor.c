/* HarisOS software cursor rendering.
 * Cursor overlay remains on the physical framebuffer.
 * Shapes: the arrow (bitmap in cursor_data.c) and four resize double-arrows made at first use. */
#include "gfx_internal.h"

extern const int gfx_cursor_w, gfx_cursor_h, gfx_cursor_hot;
extern const unsigned char gfx_cursor_pixels[]; /* per pixel: shade, alpha */

#define CUR_MAX_W 64
#define CUR_MAX_H 64
#define BIG 21 /* resize cursors are BIG x BIG with the hot spot in the middle */

static int CUR_ON, CUR_X;
static int CUR_SHAPE; /* GFX_CURSOR_* */
static unsigned int CUR_SAVE[CUR_MAX_W * CUR_MAX_H];
static unsigned char SHAPE_PIX[4][BIG * BIG * 2];
static int SHAPE_BUILT;

/* ---- the resize double-arrows ---- */
static int iabs(int v) { return v < 0 ? -v : v; }

/* 1 if pixel (x, y) of a BIG x BIG cursor is part of the white arrow */
static int fill_at(int shape, int x, int y)
{
    if (x < 0 || y < 0 || x >= BIG || y >= BIG)
        return 0;
    if (shape == GFX_CURSOR_SIZE_H || shape == GFX_CURSOR_SIZE_V)
    {
        int a = shape == GFX_CURSOR_SIZE_H ? x : y; /* position along the arrow */
        int d = iabs((shape == GFX_CURSOR_SIZE_H ? y : x) - BIG / 2);
        int e = a <= BIG / 2 ? a : BIG - 1 - a; /* distance from the nearest tip */
        if (e < 1)
            return 0;
        return e <= 5 ? d <= e - 1 : d <= 1; /* arrow head, then a 3 pixel shaft */
    }
    if (shape == GFX_CURSOR_SIZE_NESW)
        x = BIG - 1 - x;
    int s = x + y;
    if (x >= 1 && y >= 1 && s <= 8)
        return 1; /* head at the top-left */
    if (x <= BIG - 2 && y <= BIG - 2 && s >= 2 * (BIG - 1) - 8)
        return 1; /* head at the bottom-right */
    return s > 8 && s < 2 * (BIG - 1) - 8 && iabs(x - y) <= 1;
}

static void build_shapes(void)
{
    for (int sh = 1; sh <= 4; sh++)
        for (int y = 0; y < BIG; y++)
            for (int x = 0; x < BIG; x++)
            {
                unsigned char *p = &SHAPE_PIX[sh - 1][2 * (y * BIG + x)];
                if (fill_at(sh, x, y))
                {
                    p[0] = 255;
                    p[1] = 255;
                    continue;
                }
                int edge = 0; /* a dark outline around the white shape */
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if (fill_at(sh, x + dx, y + dy))
                            edge = 1;
                p[0] = 0;
                p[1] = edge ? 215 : 0;
            }
    SHAPE_BUILT = 1;
}

/* current cursor: size, hot spot and pixels */
static int cur_w(void) { return CUR_SHAPE ? BIG : gfx_cursor_w; }
static int cur_h(void) { return CUR_SHAPE ? BIG : gfx_cursor_h; }
static int cur_hot(void) { return CUR_SHAPE ? BIG / 2 : gfx_cursor_hot; }
static const unsigned char *cur_pix(void) { return CUR_SHAPE ? SHAPE_PIX[CUR_SHAPE - 1] : gfx_cursor_pixels; }

void gfx_internal_cursor_reset_state(void)
{
    CUR_ON = 0;
    CUR_X = 0;
    CUR_SHAPE = 0;
    CUR_DRAWN = 0;
    CUR_Y_SHARED = 0;
}

void gfx_internal_cursor_y_range(int *y0, int *y1)
{
    *y0 = CUR_Y_SHARED - cur_hot();
    *y1 = *y0 + cur_h();
}

void gfx_internal_cursor_restore(void)
{
    if (!CUR_DRAWN || !FB)
        return;
    int cw = cur_w(), ch = cur_h();
    int ox = CUR_X - cur_hot(), oy = CUR_Y_SHARED - cur_hot();
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
        {
            int sx = ox + x, sy = oy + y;
            if (sx >= 0 && sx < W && sy >= 0 && sy < H)
                FB[(long long)sy * PITCH + sx] = CUR_SAVE[y * cw + x];
        }
    CUR_DRAWN = 0;
}

void gfx_internal_cursor_paint(void)
{
    if (!CUR_ON || CUR_DRAWN || !FB)
        return;
    int cw = cur_w(), ch = cur_h();
    const unsigned char *pix = cur_pix();
    int ox = CUR_X - cur_hot(), oy = CUR_Y_SHARED - cur_hot();
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
        {
            int sx = ox + x, sy = oy + y;
            if (sx < 0 || sx >= W || sy < 0 || sy >= H)
                continue;
            volatile unsigned int *p = FB + (long long)sy * PITCH + sx;
            CUR_SAVE[y * cw + x] = *p;
            int shade = pix[2 * (y * cw + x)];
            int alpha = pix[2 * (y * cw + x) + 1];
            gfx_internal_blend_ptr(p, shade, shade, shade, alpha);
        }
    CUR_DRAWN = 1;
}

void gfx_cursor_set_shape(int shape)
{
    if (shape < 0 || shape > 4)
        shape = 0;
    if (shape == CUR_SHAPE)
        return;
    if (shape && !SHAPE_BUILT)
        build_shapes();
    gfx_internal_cursor_restore(); /* take the old one off the screen with its own size */
    CUR_SHAPE = shape;
    gfx_internal_cursor_paint();
}

void gfx_cursor_show(int on)
{
    if (on)
    {
        CUR_ON = 1;
        gfx_internal_cursor_paint();
    }
    else
    {
        gfx_internal_cursor_restore();
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
    if (x == CUR_X && y == CUR_Y_SHARED && CUR_DRAWN)
        return;
    gfx_internal_cursor_restore();
    CUR_X = x;
    CUR_Y_SHARED = y;
    gfx_internal_cursor_paint();
}
