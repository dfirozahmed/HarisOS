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

int wm_drag_memory(void **a, void **b, unsigned long long *bytes)
{
    *a = LAYER_A;
    *b = LAYER_B;
    *bytes = LAYER_BYTES;
    LAYER_WIN = 0; /* whoever borrows them draws over the drag picture */
    return LAYER_BYTES != 0;
}

/* ---- drawing ------------------------------------------------- */

/* One window: frame, content, buttons and size grip. i is its place in WM_Z. */
void wm_draw_window_internal(Window *w, int i, int top)
{
    GfxClip saved = gfx_get_clip();
    ui_window_button_state(wm_titlebtn_hover(w), wm_titlebtn_pressed(w));
    UiRect body = w->dialog      ? ui_dialog(w->r, w->title, i == top)
                  : w->maximized ? ui_window_flat(w->r, w->title, i == top)
                                 : ui_window(w->r, w->title, i == top);
    wm_layout_internal(w);
    gfx_clip_intersect(body.x, body.y, body.w, body.h); /* nothing can spill out of the window */
    if (w->on_draw)
    {
        unsigned long long t0 = wm_now_us();
        w->on_draw(w, body);
        wm_charge(w, t0);
    }
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
        wm_draw_window_internal(w, i, top);
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


/* Every visible window except `skip`, back to front, that reaches the current clip. */
static void draw_windows(const Window *skip, int top)
{
    for (int i = 0; i < WM_ZN; i++)
    {
        Window *w = WM_Z[i];
        if (w->minimized || w == skip)
            continue;
        GfxClip saved = gfx_get_clip();
        /* partial redraw: skip windows (and shadows) that are outside the clip */
        if (w->r.x - WM_SHADOW_MARGIN >= saved.x1 || w->r.x + w->r.w + WM_SHADOW_MARGIN <= saved.x0 ||
            w->r.y - WM_SHADOW_MARGIN >= saved.y1 || w->r.y + w->r.h + WM_SHADOW_MARGIN <= saved.y0)
            continue;
        wm_draw_window_internal(w, i, top);
    }
}

void wm_draw(void)
{
    int top = wm_top_index_internal();
    Window *moving = wm_moving_window_internal();
    if (LAYER_WIN && LAYER_WIN != moving)
        LAYER_WIN = 0; /* the drag is over: its picture is stale */

    /* The dragged window is on top (it was raised when grabbed), so its opaque inside hides whatever is behind
     * it. Draw the other windows only around that inside (a File Explorer behind it is expensive), then paste
     * the layer over everything. */
    if (moving && wm_z_index_internal(moving) == top)
    {
        int ready = LAYER_WIN == moving && LAYER_W == moving->r.w + 2 * WM_SHADOW_MARGIN &&
                    LAYER_H == moving->r.h + 2 * WM_SHADOW_MARGIN;
        int fresh = 0;
        if (!ready)
            ready = fresh = layer_capture(moving, top, top);
        if (ready)
        {
            GfxClip base = gfx_get_clip();
            int m = ui_theme()->radius_l + 2; /* the rounded corners are not opaque */
            /* With a GPU driver the layer is not blended here: gfx_present() has the GPU paste it onto the screen */
            int gpu = gfx_overlay_show(LAYER_A, LAYER_W, LAYER_H, moving->r.x - WM_SHADOW_MARGIN,
                                       moving->r.y - WM_SHADOW_MARGIN, fresh, WM_SHADOW_MARGIN + m);
            int cx0 = moving->r.x + m, cy0 = moving->r.y + m;
            int cx1 = moving->r.x + moving->r.w - m, cy1 = moving->r.y + moving->r.h - m;
            if (cx1 > cx0 && cy1 > cy0)
            {
                /* the clip minus the opaque inside: a band above, a band below, and two beside it */
                int bx0 = base.x0, by0 = base.y0, bx1 = base.x1, by1 = base.y1;
                int mid0 = cy0 > by0 ? cy0 : by0, mid1 = cy1 < by1 ? cy1 : by1;
                if (cy0 > by0)
                {
                    gfx_set_clip(bx0, by0, bx1 - bx0, (cy0 < by1 ? cy0 : by1) - by0);
                    draw_windows(moving, top);
                }
                if (cy1 < by1)
                {
                    int ys = cy1 > by0 ? cy1 : by0;
                    gfx_set_clip(bx0, ys, bx1 - bx0, by1 - ys);
                    draw_windows(moving, top);
                }
                if (mid1 > mid0)
                {
                    if (cx0 > bx0)
                    {
                        gfx_set_clip(bx0, mid0, (cx0 < bx1 ? cx0 : bx1) - bx0, mid1 - mid0);
                        draw_windows(moving, top);
                    }
                    if (cx1 < bx1)
                    {
                        int xs = cx1 > bx0 ? cx1 : bx0;
                        gfx_set_clip(xs, mid0, bx1 - xs, mid1 - mid0);
                        draw_windows(moving, top);
                    }
                }
                gfx_restore_clip(base);
            }
            else
                draw_windows(moving, top);
            if (!gpu)
                gfx_layer_blit(LAYER_A, LAYER_W, LAYER_H, moving->r.x - WM_SHADOW_MARGIN, moving->r.y - WM_SHADOW_MARGIN);
            return;
        }
    }
    gfx_overlay_hide(); /* no drag (any more): the window is drawn normally again */
    draw_windows(0, top);
}
