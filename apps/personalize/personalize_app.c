/* Personalize app: the theme (dark / light), the wallpaper (a ready-made one, with a preview of each, or a .bmp
 * picture from a drive) and the accent colour (8 ready-made ones, the theme's own, or any colour from three R / G / B
 * sliders). A pick is shown at once and remembered in THEME.CFG (look.h does both). Esc closes the window.
 * An app uses only public headers (ui.h, wm.h, look.h, ...) - never desktop_internal.h. */
#include "app.h"
#include "desktop.h"
#include "gfx.h"
#include "look.h"
#include "taskbar.h"
#include "ui.h"
#include "vfs.h"
#include "wm.h"

#define M 24       /* margin inside the window */
#define G 12       /* gap between tiles */
#define PIC_MAX 6  /* pictures listed */
#define PIC_COLS 3
#define DOTS 8
#define DOT_R 14

static const UiRgb ACCENTS[DOTS] = {
    {54, 120, 246}, {136, 84, 240}, {226, 72, 160}, {226, 60, 70},
    {240, 128, 40}, {40, 170, 90}, {20, 160, 170}, {100, 110, 130},
};

static Window *PS_WIN;
static int PICS[PIC_MAX], NPICS;
static UiRgb CUSTOM; /* the colour of the sliders */
static int DRAG = -1; /* the slider being dragged (0 = red, 1 = green, 2 = blue), -1 = none */

typedef struct
{
    int y_theme, y_wall, y_pics, y_accent, y_custom, y_msg;
    UiRect theme[2], wall[UI_WALL_PRESETS], pics[PIC_MAX], own, dots[DOTS], slider[3], swatch;
} Layout;

static void layout(UiRect body, Layout *L)
{
    int x = body.x + M, w = body.w - 2 * M, y = body.y + 16;
    L->y_theme = y;
    for (int i = 0; i < 2; i++)
        L->theme[i] = ui_rect(x + i * (150 + G), y + 32, 150, 56);
    y += 32 + 56 + 24;

    L->y_wall = y;
    int tw = (w - 3 * G) / 4, th = tw * (int)gfx_height() / ((int)gfx_width() > 0 ? (int)gfx_width() : 1);
    for (int i = 0; i < UI_WALL_PRESETS; i++)
        L->wall[i] = ui_rect(x + (i % 4) * (tw + G), y + 32 + (i / 4) * (th + 26), tw, th);
    y += 32 + 2 * (th + 26) + 4;

    L->y_pics = y;
    int cw = (w - (PIC_COLS - 1) * 10) / PIC_COLS;
    for (int i = 0; i < PIC_MAX; i++)
        L->pics[i] = ui_rect(x + (i % PIC_COLS) * (cw + 10), y + 24 + (i / PIC_COLS) * 42, cw, 34);
    int rows = NPICS > PIC_COLS ? 2 : 1;
    y += 24 + rows * 42 + 14;

    L->y_accent = y;
    L->own = ui_rect(x, y + 32, 84, 32);
    for (int i = 0; i < DOTS; i++)
        L->dots[i] = ui_rect(x + 84 + 18 + i * 42, y + 32, 2 * DOT_R + 4, 2 * DOT_R + 4);
    y += 32 + 32 + 18;

    L->y_custom = y;
    for (int i = 0; i < 3; i++)
        L->slider[i] = ui_rect(x + 26, y + 26 + i * 30, w - 26 - 96, 20);
    L->swatch = ui_rect(x + w - 80, y + 24, 80, 84);
    y += 26 + 90 + 10;
    L->y_msg = y;
}

static int in(UiRect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static int same_rgb(UiRgb a, UiRgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static unsigned char rgb_part(UiRgb c, int i) { return i == 0 ? c.r : i == 1 ? c.g : c.b; }

static void set_part(UiRgb *c, int i, int v)
{
    unsigned char b = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    if (i == 0)
        c->r = b;
    else if (i == 1)
        c->g = b;
    else
        c->b = b;
}

/* a picked tile: a ring in the accent colour around it */
static void picked(UiRect r, int radius)
{
    gfx_round_rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6, radius + 3, 3, ui_color(ui_theme()->accent));
}

static void theme_tile(UiRect r, int dark, int on)
{
    const UiTheme *t = dark ? &ui_theme_dark : &ui_theme_light;
    gfx_fill_round_rect(r.x, r.y, r.w, r.h, 10, ui_color(t->surface));
    gfx_fill_rect(r.x + 10, r.y + 10, 40, 6, ui_color(t->text_dim));
    gfx_fill_round_rect(r.x + 10, r.y + 22, 24, 24, 6, ui_color(ui_theme()->accent));
    ui_text_in_color(ui_rect(r.x + 44, r.y + 20, r.w - 52, 26), dark ? "Dark" : "Light", UI_TEXT_LABEL, UI_ALIGN_LEFT, t->text);
    gfx_round_rect(r.x, r.y, r.w, r.h, 10, 1, ui_color(ui_theme()->border));
    if (on)
        picked(r, 10);
}

static const char *file_name(int node)
{
    const VfsNode *n = vfs_node(node);
    return n ? n->name : "?";
}

static void ps_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    Layout L;
    layout(body, &L);
    int x = body.x + M;

    ui_text(x, L.y_theme, "Theme", UI_TEXT_TITLE);
    theme_tile(L.theme[0], 1, look_dark());
    theme_tile(L.theme[1], 0, !look_dark());

    ui_text(x, L.y_wall, "Wallpaper", UI_TEXT_TITLE);
    for (int i = 0; i < UI_WALL_PRESETS; i++)
    {
        UiRect r = L.wall[i];
        GfxWallpaper d;
        ui_wallpaper_preset(i, r.w, r.h, &d);
        gfx_wallpaper_draw_at(&d, r.x, r.y);
        gfx_rect(r.x, r.y, r.w, r.h, ui_color(t->border)); /* square, like the picture: a round edge left its corners out */
        if (look_wallpaper() == i)
            picked(r, 0);
        ui_text_in_color(ui_rect(r.x, r.y + r.h + 4, r.w, 18), ui_wallpaper_preset_name(i), UI_TEXT_SMALL, UI_ALIGN_CENTER,
                         t->text_dim);
    }

    ui_text_color(x, L.y_pics, "Pictures (.bmp in a drive's top folder, or its Pictures / Wallpapers folder)", UI_TEXT_SMALL,
                  t->text_dim);
    if (NPICS == 0)
        ui_text_in_color(ui_rect(x, L.y_pics + 24, body.w - 2 * M, 34), "No .bmp pictures found", UI_TEXT_BODY, UI_ALIGN_LEFT,
                         t->text_dim);
    for (int i = 0; i < NPICS; i++)
    {
        UiRect r = L.pics[i];
        gfx_fill_round_rect(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->surface_alt));
        ui_text_in(ui_inset(r, 10), file_name(PICS[i]), UI_TEXT_BODY, UI_ALIGN_LEFT);
        if (look_picture() == PICS[i])
            picked(r, t->radius_s);
    }

    ui_text(x, L.y_accent, "Accent colour", UI_TEXT_TITLE);
    {
        UiRect r = L.own;
        gfx_fill_round_rect(r.x, r.y, r.w, r.h, r.h / 2, ui_color(t->surface_alt));
        ui_text_in(r, "Theme", UI_TEXT_LABEL, UI_ALIGN_CENTER);
        if (!look_accent_own())
            picked(r, r.h / 2);
    }
    for (int i = 0; i < DOTS; i++)
    {
        UiRect r = L.dots[i];
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        gfx_fill_circle_q(cx * 16, cy * 16, DOT_R * 16, ui_color(ACCENTS[i]), 255);
        if (look_accent_own() && same_rgb(look_accent(), ACCENTS[i]))
            gfx_ring_q(cx * 16, cy * 16, (DOT_R + 5) * 16, 3 * 16, ui_color(t->text), 255);
    }

    ui_text_color(x, L.y_custom, "Your own colour (applied when the slider is let go)", UI_TEXT_SMALL, t->text_dim);
    static const char *const RGB_NAME[3] = {"R", "G", "B"};
    for (int i = 0; i < 3; i++)
    {
        UiRect s = L.slider[i];
        ui_text_in(ui_rect(x, s.y - 2, 20, s.h + 4), RGB_NAME[i], UI_TEXT_LABEL, UI_ALIGN_LEFT);
        ui_slider(s, rgb_part(CUSTOM, i) * 100 / 255);
    }
    gfx_fill_round_rect(L.swatch.x, L.swatch.y, L.swatch.w, L.swatch.h, t->radius_m, ui_color(CUSTOM));
    gfx_round_rect(L.swatch.x, L.swatch.y, L.swatch.w, L.swatch.h, t->radius_m, 1, ui_color(t->border));

    const char *msg = look_message();
    if (msg[0])
        ui_text_in_color(ui_rect(x, L.y_msg, body.w - 2 * M, 22), msg, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->warning);
}

/* slider i's value at pointer x (like the Display Settings brightness slider: the knob's centre spans the track) */
static int slider_value(UiRect s, int x)
{
    int span = s.w - s.h;
    return (x - (s.x + s.h / 2)) * 255 / (span > 0 ? span : 1);
}

static int ps_press(Window *w, int x, int y)
{
    Layout L;
    layout(wm_content(w), &L);
    for (int i = 0; i < 2; i++)
        if (in(L.theme[i], x, y))
        {
            if (look_dark() != (i == 0))
                look_set_dark(i == 0);
            return 1;
        }
    for (int i = 0; i < UI_WALL_PRESETS; i++)
        if (in(L.wall[i], x, y))
        {
            if (look_wallpaper() != i)
                look_set_wallpaper(i);
            return 1;
        }
    for (int i = 0; i < NPICS; i++)
        if (in(L.pics[i], x, y))
        {
            if (look_picture() != PICS[i])
                look_set_picture(PICS[i]);
            return 1;
        }
    if (in(L.own, x, y))
    {
        look_set_accent(0);
        CUSTOM = look_accent();
        return 1;
    }
    for (int i = 0; i < DOTS; i++)
        if (in(L.dots[i], x, y))
        {
            look_set_accent(&ACCENTS[i]);
            CUSTOM = ACCENTS[i];
            return 1;
        }
    for (int i = 0; i < 3; i++)
    {
        UiRect s = L.slider[i];
        if (x >= s.x && x < s.x + s.w && y >= s.y - 8 && y < s.y + s.h + 8)
        {
            DRAG = i;
            set_part(&CUSTOM, i, slider_value(s, x));
            return 1;
        }
    }
    return 0;
}

static int ps_drag(Window *w, int x, int y, unsigned buttons)
{
    (void)y;
    if (DRAG < 0)
        return 0;
    Layout L;
    layout(wm_content(w), &L);
    set_part(&CUSTOM, DRAG, slider_value(L.slider[DRAG], x));
    if (!(buttons & 1)) /* let go: the colour becomes the accent (everything is drawn again once, not on every step) */
    {
        DRAG = -1;
        look_set_accent(&CUSTOM);
    }
    return 1;
}

static void ps_key(Window *w, const KeyEvent *k)
{
    if (k->pressed && k->key == KEY_ESC)
        wm_close(w);
}

static void ps_close(Window *w)
{
    (void)w;
    PS_WIN = 0;
    DRAG = -1;
}

static void personalize_open(void)
{
    if (PS_WIN)
    {
        wm_activate(PS_WIN);
        return;
    }
    NPICS = look_find_pictures(PICS, PIC_MAX);
    CUSTOM = look_accent();
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 620, h = 724;
    if (w > W - 20)
        w = W - 20;
    if (h > H - TASKBAR_HEIGHT - 8)
        h = H - TASKBAR_HEIGHT - 8;
    int x = (W - w) / 2, y = (H - TASKBAR_HEIGHT - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    Window *win = wm_create("Personalize", x, y, w, h, ps_draw, 0);
    if (!win)
    {
        desktop_set_status("Window limit reached - close one first.");
        wm_invalidate();
        return;
    }
    win->on_key = ps_key;
    win->on_press = ps_press;
    win->on_drag = ps_drag;
    win->on_close = ps_close;
    win->icon = TASKBAR_ICON_SETTINGS;
    wm_set_resizable(win, 0);
    PS_WIN = win;
    desktop_set_status(0);
}

const AppInfo APP_PERSONALIZE = {
    .name = "Personalize",
    .desc = "Theme, wallpaper and colours",
    .icon = UI_ICON_SETTINGS,
    .open = personalize_open,
};
