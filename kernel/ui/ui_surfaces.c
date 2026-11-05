/* HarisUI: Wallpaper, cards, windows and separators. */
#include "ui_internal.h"

/* ---- surfaces ------------------------------------------------ */
static void wall_rgb(unsigned char *out, UiRgb c)
{
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
}

static void wall_rect(GfxWallpaper *w, int x, int y, int rw, int rh, unsigned char r, unsigned char g, unsigned char b)
{
    int i = w->nrect++;
    w->rect[i].x = x;
    w->rect[i].y = y;
    w->rect[i].w = rw;
    w->rect[i].h = rh;
    w->rect[i].rgb[0] = r;
    w->rect[i].rgb[1] = g;
    w->rect[i].rgb[2] = b;
}

/* The ready-made wallpapers. The first follows the theme (its colours, and the soft blue "waves"); the others look the
 * same in the dark and the light theme. */
typedef struct
{
    const char *name;
    UiRgb top, bottom, glow1, glow2;
    unsigned char a1, a2;
} WallPreset;

#define RGB(r, g, b) {r, g, b}
static const WallPreset PRESETS[UI_WALL_PRESETS] = {
    {"HarisOS", RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), 90, 70}, /* colours from the theme */
    {"Midnight", RGB(6, 8, 20), RGB(26, 30, 64), RGB(90, 110, 255), RGB(40, 190, 220), 80, 50},
    {"Aurora", RGB(4, 22, 30), RGB(16, 70, 82), RGB(60, 220, 160), RGB(120, 90, 255), 95, 75},
    {"Sunset", RGB(44, 18, 64), RGB(236, 120, 92), RGB(255, 196, 120), RGB(200, 60, 140), 110, 80},
    {"Forest", RGB(8, 28, 18), RGB(40, 92, 60), RGB(150, 220, 120), RGB(30, 120, 90), 70, 60},
    {"Ocean", RGB(6, 36, 76), RGB(40, 150, 200), RGB(120, 220, 255), RGB(20, 80, 200), 90, 70},
    {"Rose", RGB(62, 20, 42), RGB(228, 140, 170), RGB(255, 210, 230), RGB(180, 80, 200), 100, 60},
    {"Graphite", RGB(16, 16, 20), RGB(58, 60, 70), RGB(150, 150, 170), RGB(90, 90, 116), 50, 40},
};
#undef RGB

static int WALL = 0;                    /* the preset in use */
static const unsigned int *WALL_IMAGE;  /* or a picture: screen size, 0x00RRGGBB, one screen width per row */
static unsigned WALL_IMAGE_GEN;

int ui_wallpaper_preset_count(void) { return UI_WALL_PRESETS; }
const char *ui_wallpaper_preset_name(int i) { return i >= 0 && i < UI_WALL_PRESETS ? PRESETS[i].name : ""; }

void ui_wallpaper_preset(int i, int W, int H, GfxWallpaper *w)
{
    const UiTheme *t = THEME;
    const WallPreset *p = &PRESETS[i >= 0 && i < UI_WALL_PRESETS ? i : 0];
    int theme = p == &PRESETS[0];
    w->w = W;
    w->h = H;
    wall_rgb(w->top, theme ? t->bg_top : p->top);
    wall_rgb(w->bottom, theme ? t->bg_bottom : p->bottom);
    /* two big soft lights give the wallpaper depth */
    int big = (W > H ? W : H) * 6 / 10;
    w->nglow = 2;
    w->glow[0].cx = W * 78 / 100;
    w->glow[0].cy = H * 20 / 100;
    w->glow[0].r = big;
    wall_rgb(w->glow[0].rgb, theme ? t->glow1 : p->glow1);
    w->glow[0].alpha = p->a1;
    w->glow[1].cx = W * 18 / 100;
    w->glow[1].cy = H * 88 / 100;
    w->glow[1].r = big * 8 / 10;
    wall_rgb(w->glow[1].rgb, theme ? t->glow2 : p->glow2);
    w->glow[1].alpha = p->a2;

    /* soft abstract wallpaper waves (only the theme's own wallpaper has them) */
    w->nrect = 0;
    if (!theme)
        return;
    wall_rect(w, W * 56 / 100, H * 16 / 100, W * 34 / 100, H * 48 / 100, 29, 104, 169);
    wall_rect(w, W * 62 / 100, H * 22 / 100, W * 25 / 100, H * 36 / 100, 34, 126, 185);
    wall_rect(w, W * 68 / 100, H * 28 / 100, W * 15 / 100, H * 24 / 100, 42, 148, 199);
}

void ui_wallpaper(GfxWallpaper *w) { ui_wallpaper_preset(WALL, (int)gfx_width(), (int)gfx_height(), w); }

void ui_set_wallpaper(int preset)
{
    WALL = preset >= 0 && preset < UI_WALL_PRESETS ? preset : 0;
    WALL_IMAGE = 0;
}

int ui_wallpaper_current(void) { return WALL_IMAGE ? -1 : WALL; }

void ui_set_wallpaper_image(const unsigned int *px)
{
    WALL_IMAGE = px;
    WALL_IMAGE_GEN++;
}

const unsigned int *ui_wallpaper_image(unsigned *gen)
{
    if (gen)
        *gen = WALL_IMAGE_GEN;
    return WALL_IMAGE;
}

void ui_background(void)
{
    if (WALL_IMAGE)
    {
        int W = (int)gfx_width(), H = (int)gfx_height();
        for (int y = 0; y < H; y++)
            gfx_put_pixels(0, y, WALL_IMAGE + (long long)y * W, W);
        return;
    }
    GfxWallpaper w;
    ui_wallpaper(&w);
    gfx_wallpaper_draw(&w);
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

static int BTN_HOVER = -1, BTN_PRESS = -1; /* for the next window frame, see ui_window_button_state() */

void ui_window_button_state(int hover, int pressed)
{
    BTN_HOVER = hover;
    BTN_PRESS = pressed;
}

static int SHADOWS = 1; /* 0: a compositor draws the windows' shadows (ui_window_shadows) */

void ui_window_shadows(int on) { SHADOWS = on; }

void ui_window_shadow(int active, int *blur, int *dy, int *strength)
{
    /* A wide, soft shadow, lifted a little (dy) like a light from above; the focused window floats higher than the others.
     * The reach below the window is blur / 2 + dy: it must stay inside WM_SHADOW_MARGIN (48). */
    *blur = active ? 40 : 26;
    *dy = active ? 14 : 8;
    *strength = active ? THEME->shadow : THEME->shadow * 11 / 20;
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
    if (!flat && SHADOWS)
    {
        int blur, dy, strength;
        ui_window_shadow(active, &blur, &dy, &strength);
        gfx_shadow(r.x, r.y, r.w, r.h, rad, blur, 0, dy, (unsigned char)strength);
    }
    gfx_fill_round_rect(r.x, r.y, r.w, r.h, rad, ui_color(t->surface));

    /* title bar: draw a taller rounded rectangle but clip it, so only the top corners are round */
    GfxClip saved = gfx_get_clip();
    gfx_clip_intersect(r.x, r.y, r.w, tb);
    gfx_fill_round_rect(r.x, r.y, r.w, tb + rad * 2 + (flat ? 1 : 0), rad, ui_color(t->titlebar));
    gfx_restore_clip(saved);
    gfx_hline(r.x, r.y + tb, r.w, ui_color(t->border));
    if (!flat) /* a maximized window fills the screen: no outline */
        gfx_round_rect(r.x, r.y, r.w, r.h, rad, 1, ui_color(t->border));

    /* window buttons (close / minimize / maximize); the symbols show on the focused window */
    int cy = r.y + tb / 2;
    int cyq = cy * 16 + 8;
    /* bright fixed colors (not theme-dependent) so the buttons stand out; index order: close, minimize, maximize */
    // UiRgb dots[3] = {ui_rgb(255, 82, 82), ui_rgb(255, 196, 0), ui_rgb(0, 214, 100)};
    UiRgb dots[3] = {t->danger, t->warning, t->success};
    /* right-aligned like Windows: [minimize][maximize][close]; distance of each center from the right edge */
    const int from_right[3] = {22, 62, 42};
    for (int i = 0; i < (dialog ? 1 : 3); i++)
    {
        int cxq = (r.x + r.w - from_right[i]) * 16 + 8;
        int hot = (i == BTN_HOVER);
        int down = hot && (i == BTN_PRESS); /* pressed and still on the button */
        UiRgb col = dots[i];
        if (!active && !hot)
            col = ui_rgb_mix(t->titlebar, t->text_dim, 90); /* an unfocused window's buttons are paler, but keep their color */
        if (hot)
            col = ui_rgb_mix(col, ui_rgb(255, 255, 255), 60); /* lighter while the pointer is on them */
        if (down)
            col = ui_rgb_mix(dots[i], ui_rgb(0, 0, 0), 105); /* pressed: darker */
        gfx_fill_circle_q(cxq, cyq, down ? 7 * 16 + 6 : 8 * 16, ui_color(col), 255);
        if (active || hot)
            wb_glyph(i == 0 ? 0 : i == 1 ? 1
                                         : (flat ? 3 : 2),
                     cxq, cyq, ui_rgb_mix(col, ui_rgb(0, 0, 0), down ? 215 : 175));
    }
    BTN_HOVER = BTN_PRESS = -1; /* one-shot: the next window starts plain */

    UiRect tr = ui_rect(r.x + 80, r.y, r.w - 160 > 0 ? r.w - 160 : 0, tb);
    ui_text_in_color(tr, title, UI_TEXT_LABEL, UI_ALIGN_CENTER, active ? t->text : t->text_dim);

    if (flat) /* no border to leave room for */
        return ui_rect(r.x, r.y + tb + 1, r.w, r.h - tb - 1);
    return ui_rect(r.x + 1, r.y + tb + 1, r.w - 2, r.h - tb - 2);
}

UiRect ui_window(UiRect r, const char *title, int active) { return window_impl(r, title, active, 0); }
UiRect ui_window_flat(UiRect r, const char *title, int active) { return window_impl(r, title, active, 1); }
UiRect ui_dialog(UiRect r, const char *title, int active) { return window_impl(r, title, active, 2); }

void ui_separator(int x, int y, int w)
{
    gfx_hline(x, y, w, ui_color(THEME->border));
}

void ui_fill_body(UiRect body, int round_bottom, UiRgb color)
{
    if (!round_bottom)
    {
        gfx_fill_rect(body.x, body.y, body.w, body.h, ui_color(color));
        return;
    }
    /* The body inside the 1px outline has radius_l - 1. Draw a taller rounded rect that reaches above the body and clip it to the
     * body, so only the bottom corners are round (the same trick as the title bar in window_impl). */
    int rad = THEME->radius_l - 1;
    GfxClip saved = gfx_get_clip();
    gfx_clip_intersect(body.x, body.y, body.w, body.h);
    gfx_fill_round_rect(body.x, body.y - rad * 2, body.w, body.h + rad * 2, rad, ui_color(color));
    gfx_restore_clip(saved);
}
