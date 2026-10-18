/* HarisOS clipping module.
 * Public clip API stays declared in include/gfx.h.
 */
#include "gfx_internal.h"

static int clamp_coord(long long value, int limit)
{
    if (value < 0) return 0;
    if (value > limit) return limit;
    return (int)value;
}

void gfx_reset_clip(void)
{
    CX0 = 0;
    CY0 = 0;
    CX1 = W > 0 ? W : 0;
    CY1 = H > 0 ? H : 0;
}

void gfx_set_clip(int x, int y, int w, int h)
{
    long long right = (long long)x + (long long)w;
    long long bottom = (long long)y + (long long)h;

    if (w <= 0 || h <= 0 || W <= 0 || H <= 0)
    {
        CX0 = clamp_coord(x, W > 0 ? W : 0);
        CY0 = clamp_coord(y, H > 0 ? H : 0);
        CX1 = CX0;
        CY1 = CY0;
        return;
    }

    CX0 = clamp_coord(x, W);
    CY0 = clamp_coord(y, H);
    CX1 = clamp_coord(right, W);
    CY1 = clamp_coord(bottom, H);
    if (CX1 < CX0) CX1 = CX0;
    if (CY1 < CY0) CY1 = CY0;
}

void gfx_clip_intersect(int x, int y, int w, int h)
{
    long long right = (long long)x + (long long)w;
    long long bottom = (long long)y + (long long)h;
    int x0 = clamp_coord(x, W > 0 ? W : 0);
    int y0 = clamp_coord(y, H > 0 ? H : 0);
    int x1 = clamp_coord(right, W > 0 ? W : 0);
    int y1 = clamp_coord(bottom, H > 0 ? H : 0);

    if (w <= 0 || h <= 0)
    {
        x1 = x0;
        y1 = y0;
    }
    if (x0 > CX0) CX0 = x0;
    if (y0 > CY0) CY0 = y0;
    if (x1 < CX1) CX1 = x1;
    if (y1 < CY1) CY1 = y1;
    if (CX1 < CX0) CX1 = CX0;
    if (CY1 < CY0) CY1 = CY0;
}

GfxClip gfx_get_clip(void)
{
    GfxClip clip = {CX0, CY0, CX1, CY1};
    return clip;
}

void gfx_restore_clip(GfxClip clip)
{
    /* Clamp restored coordinates so an invalid external struct cannot
       expand drawing beyond the framebuffer. Normal saved clips are unchanged. */
    CX0 = clamp_coord(clip.x0, W > 0 ? W : 0);
    CY0 = clamp_coord(clip.y0, H > 0 ? H : 0);
    CX1 = clamp_coord(clip.x1, W > 0 ? W : 0);
    CY1 = clamp_coord(clip.y1, H > 0 ? H : 0);
    if (CX1 < CX0) CX1 = CX0;
    if (CY1 < CY0) CY1 = CY0;
}

int gfx_internal_clip_box(int *x0, int *y0, int *x1, int *y1)
{
    if (!x0 || !y0 || !x1 || !y1 || !DRAW) return 0;
    if (*x0 < CX0) *x0 = CX0;
    if (*y0 < CY0) *y0 = CY0;
    if (*x1 > CX1) *x1 = CX1;
    if (*y1 > CY1) *y1 = CY1;
    return *x1 > *x0 && *y1 > *y0;
}
