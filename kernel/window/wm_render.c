/* Window surface rendering, separated from input/state management. */
#include "wm_internal.h"

/* Drag layer: while a window is moved it is drawn once into a layer (window + shadow, with
 * transparency) and each frame only pastes that layer at the new place. Redrawing the window
 * itself every frame (shadow, gradients, text) is what made dragging so slow. The window's content
 * is frozen while it is dragged. Needs two buffers from the kernel (wm_set_drag_memory); without
 * them, or for a window too big for them, the window is simply drawn normally. */
static void *LAYER_A, *LAYER_B; /* A holds the finished layer, B is scratch for the white pass */
static unsigned long long LAYER_BYTES;
static Window *LAYER_WIN; /* whose picture is in LAYER_A, or 0 */
static int LAYER_W, LAYER_H;

void wm_set_drag_memory(void *a, void *b, unsigned long long bytes)
{
    LAYER_A = a;
    LAYER_B = b;
    LAYER_BYTES = (a && b) ? bytes : 0;
    LAYER_WIN = 0;
}

/* ---- drawing ------------------------------------------------- */

/* One window: frame, content, buttons and size grip. i is its place in WM_Z. */
static void draw_window(Window *w, int i, int top)
{
    GfxClip saved = gfx_get_clip();
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

/* Draw the window twice, over black and over white, at (WM_SHADOW_MARGIN, WM_SHADOW_MARGIN) of a
 * private canvas, and pack the pair into LAYER_A. Returns 1 when the layer is ready. */
static int layer_capture(Window *w, int i, int top)
{
    int lw = w->r.w + 2 * WM_SHADOW_MARGIN, lh = w->r.h + 2 * WM_SHADOW_MARGIN;
    if (!LAYER_A || (unsigned long long)lw * (unsigned long long)lh * 4ULL > LAYER_BYTES)
        return 0;
    UiRect real = w->r;
    void *buf[2] = {LAYER_A, LAYER_B};
    for (int pass = 0; pass < 2; pass++)
    {
        if (gfx_layer_begin(buf[pass], lw, lh, pass ? 0x00FFFFFFu : 0) != 0)
        {
            w->r = real;
            wm_layout_internal(w);
            return 0;
        }
        w->r = ui_rect(WM_SHADOW_MARGIN, WM_SHADOW_MARGIN, real.w, real.h);
        draw_window(w, i, top);
        gfx_layer_end();
    }
    w->r = real;
    wm_layout_internal(w);
    gfx_layer_pack(LAYER_A, LAYER_B, lw, lh);
    LAYER_WIN = w;
    LAYER_W = lw;
    LAYER_H = lh;
    return 1;
}

void wm_draw(void)
{
    int top = wm_top_index_internal();
    Window *moving = wm_moving_window_internal();
    if (LAYER_WIN && LAYER_WIN != moving)
        LAYER_WIN = 0; /* the drag is over: its picture is stale */
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
        if (w == moving)
        {
            int ready = LAYER_WIN == w && LAYER_W == w->r.w + 2 * WM_SHADOW_MARGIN &&
                        LAYER_H == w->r.h + 2 * WM_SHADOW_MARGIN;
            if (!ready)
                ready = layer_capture(w, i, top);
            if (ready)
            {
                gfx_layer_blit(LAYER_A, LAYER_W, LAYER_H, w->r.x - WM_SHADOW_MARGIN, w->r.y - WM_SHADOW_MARGIN);
                continue;
            }
        }
        draw_window(w, i, top);
    }
}
