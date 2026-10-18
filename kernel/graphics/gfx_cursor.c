/* HarisOS software cursor rendering.
 * Cursor overlay remains on the physical framebuffer; the public API is unchanged.
 */
#include "gfx_internal.h"

extern const int gfx_cursor_w, gfx_cursor_h, gfx_cursor_hot;
extern const unsigned char gfx_cursor_pixels[]; /* per pixel: shade, alpha */

#define CUR_MAX_W 64
#define CUR_MAX_H 64

static int CUR_ON, CUR_X;
static unsigned int CUR_SAVE[CUR_MAX_W * CUR_MAX_H];

void gfx_internal_cursor_reset_state(void)
{
    CUR_ON = 0;
    CUR_X = 0;
    CUR_DRAWN = 0;
    CUR_Y_SHARED = 0;
}

void gfx_internal_cursor_restore(void)
{
    if (!CUR_DRAWN || !FB)
        return;
    int ox = CUR_X - gfx_cursor_hot, oy = CUR_Y_SHARED - gfx_cursor_hot;
    for (int y = 0; y < gfx_cursor_h; y++)
        for (int x = 0; x < gfx_cursor_w; x++)
        {
            int sx = ox + x, sy = oy + y;
            if (sx >= 0 && sx < W && sy >= 0 && sy < H)
                FB[(long long)sy * PITCH + sx] = CUR_SAVE[y * gfx_cursor_w + x];
        }
    CUR_DRAWN = 0;
}

void gfx_internal_cursor_paint(void)
{
    if (!CUR_ON || CUR_DRAWN || !FB)
        return;
    int ox = CUR_X - gfx_cursor_hot, oy = CUR_Y_SHARED - gfx_cursor_hot;
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
            gfx_internal_blend_ptr(p, shade, shade, shade, alpha);
        }
    CUR_DRAWN = 1;
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
