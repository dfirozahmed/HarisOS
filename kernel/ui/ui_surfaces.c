/* HarisUI: Wallpaper, cards, windows and separators. */
#include "ui_internal.h"

/* ---- surfaces ------------------------------------------------ */
void ui_background(void)
{
    const UiTheme *t = THEME;
    int W = (int)gfx_width(), H = (int)gfx_height();
    gfx_gradient_v(0, 0, W, H, t->bg_top.r, t->bg_top.g, t->bg_top.b,
                   t->bg_bottom.r, t->bg_bottom.g, t->bg_bottom.b);
    /* two big soft lights give the wallpaper depth */
    int big = (W > H ? W : H) * 6 / 10;
    gfx_glow(W * 78 / 100, H * 20 / 100, big, ui_color(t->glow1), 90);
    gfx_glow(W * 18 / 100, H * 88 / 100, big * 8 / 10, ui_color(t->glow2), 70);

    /* soft abstract wallpaper waves */
    gfx_fill_rect(W * 56 / 100, H * 16 / 100, W * 34 / 100, H * 48 / 100, gfx_rgb(29, 104, 169));
    gfx_fill_rect(W * 62 / 100, H * 22 / 100, W * 25 / 100, H * 36 / 100, gfx_rgb(34, 126, 185));
    gfx_fill_rect(W * 68 / 100, H * 28 / 100, W * 15 / 100, H * 24 / 100, gfx_rgb(42, 148, 199));
}

void ui_background1(void)
{
    // const UiTheme *t = THEME;
    int W = (int)gfx_width(), H = (int)gfx_height();
    gfx_gradient_v(0, 0, W, H, 12, 45, 86, 40, 103, 162);

    /* soft abstract wallpaper waves */
    gfx_fill_rect(W * 56 / 100, H * 16 / 100, W * 34 / 100, H * 48 / 100, gfx_rgb(29, 104, 169));
    gfx_fill_rect(W * 62 / 100, H * 22 / 100, W * 25 / 100, H * 36 / 100, gfx_rgb(34, 126, 185));
    gfx_fill_rect(W * 68 / 100, H * 28 / 100, W * 15 / 100, H * 24 / 100, gfx_rgb(42, 148, 199));
}

void ui_card(UiRect r)
{
    const UiTheme *t = THEME;
    gfx_shadow(r.x, r.y, r.w, r.h, t->radius_m, 14, 0, 4, (unsigned char)(t->shadow / 2));
    gfx_fill_round_rect(r.x, r.y, r.w, r.h, t->radius_m, ui_color(t->surface));
    gfx_round_rect(r.x, r.y, r.w, r.h, t->radius_m, 1, ui_color(t->border));
}

/* A small glyph inside a window button, drawn with smooth lines. (cxq, cyq) = center in
 * Q4 (1/16 pixel). kind: 0 = close (x), 1 = minimize (-), 2 = maximize (square),
 * 3 = restore (two squares, shown while the window is maximized). */
static void wb_glyph(int kind, int cxq, int cyq, UiRgb color)
{
    GfxColor c = ui_color(color);
    const int w = 24; /* line width, 1.5 px */
#define L(x0, y0, x1, y1) gfx_line_q(cxq + (x0), cyq + (y0), cxq + (x1), cyq + (y1), w, c, 255)
    switch (kind)
    {
    case 0: /* x */
        L(-36, -36, 36, 36);
        L(-36, 36, 36, -36);
        break;
    case 1: /* - */
        L(-40, 0, 40, 0);
        break;
    case 2: /* square */
        L(-36, -36, 36, -36);
        L(36, -36, 36, 36);
        L(36, 36, -36, 36);
        L(-36, 36, -36, -36);
        break;
    default:               /* restore: a small square in front of a second one (kept apart so it stays readable) */
        L(-44, -2, 2, -2); /* front square */
        L(2, -2, 2, 44);
        L(2, 44, -44, 44);
        L(-44, 44, -44, -2);
        L(-14, -44, 44, -44); /* the square behind: only its top and right edges show */
        L(44, -44, 44, 14);
        break;
    }
#undef L
}

/* flat = 1: square corners and no shadow (a maximized window that touches the screen edges) */
static UiRect window_impl(UiRect r, const char *title, int active, int flat)
{
    const UiTheme *t = THEME;
    int tb = t->titlebar_h;
    int dialog = (flat == 2); /* 2 = dialog frame: only the close button */
    if (dialog)
        flat = 0;
    int rad = flat ? 0 : t->radius_l;
    if (!flat)
        gfx_shadow(r.x, r.y, r.w, r.h, rad, active ? 28 : 18, 0, active ? 12 : 6,
                   (unsigned char)(active ? t->shadow : t->shadow * 2 / 3));
    gfx_fill_round_rect(r.x, r.y, r.w, r.h, rad, ui_color(t->surface));

    /* title bar: draw a taller rounded rectangle but clip it, so only the top corners are round */
    GfxClip saved = gfx_get_clip();
    gfx_clip_intersect(r.x, r.y, r.w, tb);
    gfx_fill_round_rect(r.x, r.y, r.w, tb + rad * 2 + (flat ? 1 : 0), rad, ui_color(t->titlebar));
    gfx_restore_clip(saved);
    gfx_hline(r.x, r.y + tb, r.w, ui_color(t->border));
    gfx_round_rect(r.x, r.y, r.w, r.h, rad, 1, ui_color(t->border));

    /* window buttons (close / minimize / maximize); the symbols show on the focused window */
    int cy = r.y + tb / 2;
    int cyq = cy * 16 + 8;
    UiRgb dots[3] = {t->danger, t->warning, t->success};
    for (int i = 0; i < (dialog ? 1 : 3); i++)
    {
        int cxq = (r.x + 22 + 20 * i) * 16 + 8;
        UiRgb col = dots[i];
        if (!active)
            col = ui_rgb_mix(t->titlebar, t->text_dim, 90);
        gfx_fill_circle_q(cxq, cyq, 8 * 16, ui_color(col), 255);
        if (active)
            wb_glyph(i == 0 ? 0 : i == 1 ? 1
                                         : (flat ? 3 : 2),
                     cxq, cyq, ui_rgb_mix(col, ui_rgb(0, 0, 0), 175));
    }

    UiRect tr = ui_rect(r.x + 80, r.y, r.w - 160 > 0 ? r.w - 160 : 0, tb);
    ui_text_in_color(tr, title, UI_TEXT_LABEL, UI_ALIGN_CENTER, active ? t->text : t->text_dim);

    return ui_rect(r.x + 1, r.y + tb + 1, r.w - 2, r.h - tb - 2);
}

UiRect ui_window(UiRect r, const char *title, int active) { return window_impl(r, title, active, 0); }
UiRect ui_window_flat(UiRect r, const char *title, int active) { return window_impl(r, title, active, 1); }
UiRect ui_dialog(UiRect r, const char *title, int active) { return window_impl(r, title, active, 2); }

void ui_separator(int x, int y, int w)
{
    gfx_hline(x, y, w, ui_color(THEME->border));
}

