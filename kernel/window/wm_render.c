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
        ui_window_button_state(wm_titlebtn_hover(w), wm_titlebtn_pressed(w));
        UiRect body = w->dialog      ? ui_dialog(w->r, w->title, i == top)
                      : w->maximized ? ui_window_flat(w->r, w->title, i == top)
                                     : ui_window(w->r, w->title, i == top);
        wm_layout_internal(w);
        gfx_clip_intersect(body.x, body.y, body.w, body.h); /* nothing can spill out of the window */
        if (w->on_draw)
            w->on_draw(w, body);
        ui_buttons_draw(&w->set);
        gfx_restore_clip(saved);
        if (!w->dialog && !w->maximized && !w->fixed_size)
        {
            /* size grip: six small dots in the bottom-right corner show that the window can be resized */
            UiRgb c = ui_theme()->text_dim;
            gfx_clip_intersect(w->r.x, w->r.y, w->r.w, w->r.h);
            for (int gy = 0; gy < 3; gy++)
                for (int gx = 0; gx + gy < 3; gx++)
                    gfx_fill_rect_alpha(w->r.x + w->r.w - 7 - 4 * gx, w->r.y + w->r.h - 7 - 4 * gy, 2, 2, c.r, c.g, c.b,
                                        i == top ? 150 : 90);
            gfx_restore_clip(saved);
        }
    }
}

