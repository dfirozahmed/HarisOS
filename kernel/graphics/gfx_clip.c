/* HarisOS clipping module.
 * Public clip API stays declared in include/gfx.h.
 */
#include "gfx_internal.h"

/* The hard bounds of drawing: the screen, or the buffer of a layer (which may reach past the screen's left and top edges, see
 * gfx_layer_begin_at). No clip ever leaves them. */
int BX0, BY0, BX1, BY1;

static int clamp_coord(long long value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return (int)value;
}
#define CLAMP_X(v) clamp_coord((v), BX0, BX1 > BX0 ? BX1 : BX0)
#define CLAMP_Y(v) clamp_coord((v), BY0, BY1 > BY0 ? BY1 : BY0)

void gfx_reset_clip(void)
{
    CX0 = BX0;
    CY0 = BY0;
    CX1 = BX1 > BX0 ? BX1 : BX0;
    CY1 = BY1 > BY0 ? BY1 : BY0;
}

void gfx_set_clip(int x, int y, int w, int h)
{
    long long right = (long long)x + (long long)w;
    long long bottom = (long long)y + (long long)h;

    if (w <= 0 || h <= 0 || BX1 <= BX0 || BY1 <= BY0)
    {
        CX0 = CLAMP_X(x);
        CY0 = CLAMP_Y(y);
        CX1 = CX0;
        CY1 = CY0;
        return;
    }

    CX0 = CLAMP_X(x);
    CY0 = CLAMP_Y(y);
    CX1 = CLAMP_X(right);
    CY1 = CLAMP_Y(bottom);
    if (CX1 < CX0) CX1 = CX0;
    if (CY1 < CY0) CY1 = CY0;
}

void gfx_clip_intersect(int x, int y, int w, int h)
{
    long long right = (long long)x + (long long)w;
    long long bottom = (long long)y + (long long)h;
    int x0 = CLAMP_X(x);
    int y0 = CLAMP_Y(y);
    int x1 = CLAMP_X(right);
    int y1 = CLAMP_Y(bottom);

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
    CX0 = CLAMP_X(clip.x0);
    CY0 = CLAMP_Y(clip.y0);
    CX1 = CLAMP_X(clip.x1);
    CY1 = CLAMP_Y(clip.y1);
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
