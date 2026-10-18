/* Window surface rendering, separated from input/state management. */
#include "wm_internal.h"

/* ---- drawing ------------------------------------------------- */
void wm_draw(void)
{
    int top = wm_top_index_internal();
    for (int i = 0; i < WM_ZN; i++)
    {
        Window *w = WM_Z[i];
        if (w->minimized)
            continue;
        GfxClip saved = gfx_get_clip();
        /* partial redraw: skip windows (and shadows) that are outside the clip */
        if (w->r.x - WM_SHADOW_MARGIN >= saved.x1 || w->r.x + w->r.w + WM_SHADOW_MARGIN <= saved.x0 ||
            w->r.y - WM_SHADOW_MARGIN >= saved.y1 || w->r.y + w->r.h + WM_SHADOW_MARGIN <= saved.y0)
            continue;
        UiRect body = w->dialog      ? ui_dialog(w->r, w->title, i == top)
                      : w->maximized ? ui_window_flat(w->r, w->title, i == top)
                                     : ui_window(w->r, w->title, i == top);
        wm_layout_internal(w);
        gfx_clip_intersect(body.x, body.y, body.w, body.h); /* nothing can spill out of the window */
        if (w->on_draw)
            w->on_draw(w, body);
        ui_buttons_draw(&w->set);
        gfx_restore_clip(saved);
    }
}

