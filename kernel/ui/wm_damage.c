/* Damage tracking and invalidation for the window manager. */
#include "wm_internal.h"

/* Damage: the screen area that must be redrawn (union of everything that changed). */
int WM_HAS_DMG, WM_DMG_X0, WM_DMG_Y0, WM_DMG_X1, WM_DMG_Y1;

void wm_damage_internal(int x0, int y0, int x1, int y1)
{
    if (!WM_HAS_DMG)
    {
        WM_DMG_X0 = x0;
        WM_DMG_Y0 = y0;
        WM_DMG_X1 = x1;
        WM_DMG_Y1 = y1;
        WM_HAS_DMG = 1;
        return;
    }
    if (x0 < WM_DMG_X0)
        WM_DMG_X0 = x0;
    if (y0 < WM_DMG_Y0)
        WM_DMG_Y0 = y0;
    if (x1 > WM_DMG_X1)
        WM_DMG_X1 = x1;
    if (y1 > WM_DMG_Y1)
        WM_DMG_Y1 = y1;
}

/* a window plus its shadow */
void wm_damage_window_internal(const Window *w)
{
    wm_damage_internal(w->r.x - WM_SHADOW_MARGIN, w->r.y - WM_SHADOW_MARGIN,
           w->r.x + w->r.w + WM_SHADOW_MARGIN, w->r.y + w->r.h + WM_SHADOW_MARGIN);
}

void wm_invalidate(void)
{
    wm_damage_internal(0, 0, (int)gfx_width(), (int)gfx_height());
}

int wm_take_damage(UiRect *out)
{
    if (!WM_HAS_DMG)
        return 0;
    int W = (int)gfx_width(), H = (int)gfx_height();
    int x0 = WM_DMG_X0 < 0 ? 0 : WM_DMG_X0, y0 = WM_DMG_Y0 < 0 ? 0 : WM_DMG_Y0;
    int x1 = WM_DMG_X1 > W ? W : WM_DMG_X1, y1 = WM_DMG_Y1 > H ? H : WM_DMG_Y1;
    WM_HAS_DMG = 0;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    *out = ui_rect(x0, y0, x1 - x0, y1 - y0);
    return 1;
}


void wm_add_damage(UiRect r)
{
    wm_damage_internal(r.x, r.y, r.x + r.w, r.y + r.h);
}
