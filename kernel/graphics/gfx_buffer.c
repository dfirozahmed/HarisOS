/* HarisOS graphics buffer management.
 * Owns backbuffer state, dirty tracking, presentation and background cache.
 * Public functions remain declared by include/gfx.h.
 */
#include "gfx_internal.h"

volatile unsigned int *FB;
volatile unsigned int *DRAW;
int W, H;
int PITCH, DPITCH;
int BACK;
volatile unsigned int *CACHE;
int CACHE_OK;
int CUR_DRAWN, CUR_Y_SHARED;

void gfx_internal_buffer_init(void *framebuffer, int width, int height, int pitch)
{
    FB = (volatile unsigned int *)framebuffer;
    W = width;
    H = height;
    PITCH = pitch;
    DRAW = FB;
    DPITCH = PITCH;
    BACK = 0;
    CACHE = 0;
    CACHE_OK = 0;
    gfx_internal_dirty_reset();
    CUR_DRAWN = 0;
    CUR_Y_SHARED = 0;
}

/* ---- double buffering --------------------------------------- */
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

    int cy0 = 0, cy1 = 0; /* screen rows covered by the pointer */
    if (CUR_DRAWN)
        gfx_internal_cursor_y_range(&cy0, &cy1);
    for (int y = DY0; y < DY1; y++)
    {
        if (y >= cy0 && y < cy1)
            continue;
        volatile unsigned int *src = DRAW + (long long)y * DPITCH;
        volatile unsigned int *dst = FB + (long long)y * PITCH;
        for (int x = DX0; x < DX1; x++)
            dst[x] = src[x];
    }
    gfx_internal_cursor_restore();
    for (int y = DY0 > cy0 ? DY0 : cy0; y < (DY1 < cy1 ? DY1 : cy1); y++)
    {
        volatile unsigned int *src = DRAW + (long long)y * DPITCH;
        volatile unsigned int *dst = FB + (long long)y * PITCH;
        for (int x = DX0; x < DX1; x++)
            dst[x] = src[x];
    }
    DX0 = DY0 = DX1 = DY1 = 0;
    gfx_internal_cursor_paint();
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
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}

