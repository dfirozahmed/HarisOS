/* HarisUI: theme, layout, text and widgets. See include/ui.h. */
#include "ui.h"

/* ---- colors -------------------------------------------------- */
UiRgb ui_rgb(unsigned char r, unsigned char g, unsigned char b)
{
    UiRgb c = {r, g, b};
    return c;
}

UiRgb ui_rgb_mix(UiRgb a, UiRgb b, unsigned char t)
{
    int it = 255 - t;
    return ui_rgb((unsigned char)((a.r * it + b.r * t) / 255),
                  (unsigned char)((a.g * it + b.g * t) / 255),
                  (unsigned char)((a.b * it + b.b * t) / 255));
}

GfxColor ui_color(UiRgb c) { return gfx_rgb(c.r, c.g, c.b); }

/* ---- themes -------------------------------------------------- */
#define RGB(r, g, b) {r, g, b}

const UiTheme ui_theme_dark = {
    .bg_top = RGB(11, 16, 38),
    .bg_bottom = RGB(36, 64, 128),
    .glow1 = RGB(74, 128, 255),
    .glow2 = RGB(150, 84, 235),
    .surface = RGB(27, 33, 52),
    .surface_alt = RGB(40, 48, 72),
    .titlebar = RGB(34, 41, 63),
    .border = RGB(58, 68, 98),
    .text = RGB(236, 241, 252),
    .text_dim = RGB(148, 160, 190),
    .text_on_accent = RGB(255, 255, 255),
    .accent = RGB(54, 120, 246),
    .accent_hi = RGB(94, 156, 255),
    .success = RGB(70, 206, 140),
    .warning = RGB(255, 188, 66),
    .danger = RGB(238, 84, 92),
    .radius_s = 6,
    .radius_m = 10,
    .radius_l = 14,
    .pad = 20,
    .gap = 10,
    .titlebar_h = 40,
    .shadow = 150,
    .font_small = &font_ui_12,
    .font_body = &font_ui_14,
    .font_label = &font_med_14,
    .font_title = &font_med_16,
    .font_heading = &font_bold_20,
};

const UiTheme ui_theme_light = {
    .bg_top = RGB(150, 190, 246),
    .bg_bottom = RGB(226, 238, 252),
    .glow1 = RGB(255, 255, 255),
    .glow2 = RGB(190, 160, 255),
    .surface = RGB(250, 251, 254),
    .surface_alt = RGB(205, 212, 220),
    .titlebar = RGB(238, 242, 249),
    .border = RGB(206, 214, 228),
    .text = RGB(26, 36, 56),
    .text_dim = RGB(98, 110, 134),
    .text_on_accent = RGB(255, 255, 255),
    .accent = RGB(36, 108, 236),
    .accent_hi = RGB(76, 144, 250),
    .success = RGB(28, 158, 98),
    .warning = RGB(214, 140, 20),
    .danger = RGB(220, 62, 70),
    .radius_s = 6,
    .radius_m = 10,
    .radius_l = 14,
    .pad = 20,
    .gap = 10,
    .titlebar_h = 40,
    .shadow = 90,
    .font_small = &font_ui_12,
    .font_body = &font_ui_14,
    .font_label = &font_med_14,
    .font_title = &font_med_16,
    .font_heading = &font_bold_20,
};

static const UiTheme *THEME = &ui_theme_dark;

const UiTheme *ui_theme(void) { return THEME; }
void ui_set_theme(const UiTheme *t) { THEME = t ? t : &ui_theme_dark; }

/* ---- layout -------------------------------------------------- */
UiRect ui_rect(int x, int y, int w, int h)
{
    UiRect r = {x, y, w, h};
    return r;
}

UiRect ui_inset(UiRect r, int a)
{
    return ui_rect(r.x + a, r.y + a, r.w - 2 * a > 0 ? r.w - 2 * a : 0, r.h - 2 * a > 0 ? r.h - 2 * a : 0);
}

UiRect ui_center(UiRect o, int w, int h)
{
    return ui_rect(o.x + (o.w - w) / 2, o.y + (o.h - h) / 2, w, h);
}

UiRect ui_cut_top(UiRect *r, int h)
{
    if (h > r->h)
        h = r->h;
    UiRect s = ui_rect(r->x, r->y, r->w, h);
    r->y += h;
    r->h -= h;
    return s;
}

UiRect ui_cut_bottom(UiRect *r, int h)
{
    if (h > r->h)
        h = r->h;
    UiRect s = ui_rect(r->x, r->y + r->h - h, r->w, h);
    r->h -= h;
    return s;
}

UiRect ui_cut_left(UiRect *r, int w)
{
    if (w > r->w)
        w = r->w;
    UiRect s = ui_rect(r->x, r->y, w, r->h);
    r->x += w;
    r->w -= w;
    return s;
}

UiRect ui_cut_right(UiRect *r, int w)
{
    if (w > r->w)
        w = r->w;
    UiRect s = ui_rect(r->x + r->w - w, r->y, w, r->h);
    r->w -= w;
    return s;
}

int ui_contains(UiRect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void ui_clip(UiRect r) { gfx_set_clip(r.x, r.y, r.w, r.h); }

/* ---- text ---------------------------------------------------- */
const Font *ui_font(UiTextStyle s)
{
    switch (s)
    {
    case UI_TEXT_SMALL:
        return THEME->font_small;
    case UI_TEXT_LABEL:
        return THEME->font_label;
    case UI_TEXT_TITLE:
        return THEME->font_title;
    case UI_TEXT_HEADING:
        return THEME->font_heading;
    default:
        return THEME->font_body;
    }
}

static UiRgb style_color(UiTextStyle s)
{
    return (s == UI_TEXT_DIM || s == UI_TEXT_SMALL) ? THEME->text_dim : THEME->text;
}

int ui_text_width(const char *s, UiTextStyle st) { return gfx_text_w(ui_font(st), s); }
int ui_text_height(UiTextStyle st) { return font_line_height(ui_font(st)); }

void ui_text_color(int x, int y, const char *s, UiTextStyle st, UiRgb c)
{
    gfx_text(ui_font(st), x, y, s, ui_color(c));
}

void ui_text(int x, int y, const char *s, UiTextStyle st)
{
    ui_text_color(x, y, s, st, style_color(st));
}

void ui_text_in_color(UiRect r, const char *s, UiTextStyle st, UiAlign align, UiRgb color)
{
    const Font *f = ui_font(st);
    char buf[96];
    int w = gfx_text_w(f, s);
    if (w > r.w)
    {
        /* too long: cut it and add "..." */
        int dots = gfx_text_w(f, "...");
        int n = gfx_text_fit(f, s, r.w - dots);
        if (n > (int)sizeof(buf) - 4)
            n = (int)sizeof(buf) - 4;
        for (int i = 0; i < n; i++)
            buf[i] = s[i];
        buf[n] = '.';
        buf[n + 1] = '.';
        buf[n + 2] = '.';
        buf[n + 3] = 0;
        s = buf;
        w = gfx_text_w(f, s);
    }
    int x = r.x;
    if (align == UI_ALIGN_CENTER)
        x = r.x + (r.w - w) / 2;
    else if (align == UI_ALIGN_RIGHT)
        x = r.x + r.w - w;
    int y = r.y + (r.h - font_line_height(f)) / 2;
    gfx_text(f, x, y, s, ui_color(color));
}

void ui_text_in(UiRect r, const char *s, UiTextStyle st, UiAlign align)
{
    ui_text_in_color(r, s, st, align, style_color(st));
}

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
    default: /* restore: a small square in front of a second one (kept apart so it stays readable) */
        L(-44, -2, 2, -2);   /* front square */
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
    for (int i = 0; i < 3; i++)
    {
        int cxq = (r.x + 22 + 20 * i) * 16 + 8;
        UiRgb col = dots[i];
        if (!active)
            col = ui_rgb_mix(t->titlebar, t->text_dim, 90);
        gfx_fill_circle_q(cxq, cyq, 8 * 16, ui_color(col), 255);
        if (active)
            wb_glyph(i == 0 ? 0 : i == 1 ? 1 : (flat ? 3 : 2), cxq, cyq, ui_rgb_mix(col, ui_rgb(0, 0, 0), 175));
    }

    UiRect tr = ui_rect(r.x + 80, r.y, r.w - 160 > 0 ? r.w - 160 : 0, tb);
    ui_text_in_color(tr, title, UI_TEXT_LABEL, UI_ALIGN_CENTER, active ? t->text : t->text_dim);

    return ui_rect(r.x + 1, r.y + tb + 1, r.w - 2, r.h - tb - 2);
}

UiRect ui_window(UiRect r, const char *title, int active) { return window_impl(r, title, active, 0); }
UiRect ui_window_flat(UiRect r, const char *title, int active) { return window_impl(r, title, active, 1); }

void ui_separator(int x, int y, int w)
{
    gfx_hline(x, y, w, ui_color(THEME->border));
}

/* ---- controls ------------------------------------------------ */
static UiRgb lighten(UiRgb c, int amount) { return ui_rgb_mix(c, ui_rgb(255, 255, 255), (unsigned char)amount); }
static UiRgb darken(UiRgb c, int amount) { return ui_rgb_mix(c, ui_rgb(0, 0, 0), (unsigned char)amount); }

void ui_button(UiRect r, const char *label, UiButtonKind kind, UiState state)
{
    const UiTheme *t = THEME;
    int rad = t->radius_m;
    if (rad > r.h / 2)
        rad = r.h / 2;
    int disabled = state == UI_STATE_DISABLED;
    UiRgb top, bottom, text;

    switch (kind)
    {
    case UI_BTN_PRIMARY:
        top = t->accent_hi;
        bottom = t->accent;
        text = t->text_on_accent;
        break;
    case UI_BTN_DANGER:
        top = lighten(t->danger, 40);
        bottom = t->danger;
        text = t->text_on_accent;
        break;
    case UI_BTN_SECONDARY:
        top = lighten(t->surface_alt, 14);
        bottom = t->surface_alt;
        text = t->text;
        break;
    default: /* ghost: only visible when hovered or pressed */
        top = bottom = t->surface_alt;
        text = t->text;
        break;
    }
    if (state == UI_STATE_HOVER)
    {
        top = lighten(top, 22);
        bottom = lighten(bottom, 22);
    }
    else if (state == UI_STATE_PRESSED)
    {
        top = darken(top, 22);
        bottom = darken(bottom, 22);
    }
    else if (disabled)
    {
        top = bottom = ui_rgb_mix(t->surface, t->surface_alt, 160);
        text = t->text_dim;
    }

    if ((kind == UI_BTN_PRIMARY || kind == UI_BTN_DANGER) && !disabled && state != UI_STATE_PRESSED)
        gfx_shadow(r.x, r.y, r.w, r.h, rad, 8, 0, 3, 70);
    if (kind == UI_BTN_GHOST && (state == UI_STATE_NORMAL || disabled))
    {
        /* nothing behind the label */
    }
    else
        gfx_fill_round_rect_gradient(r.x, r.y, r.w, r.h, rad, ui_color(top), ui_color(bottom));
    if (kind == UI_BTN_SECONDARY && !disabled)
        gfx_round_rect(r.x, r.y, r.w, r.h, rad, 1, ui_color(t->border));
    ui_text_in_color(ui_rect(r.x + 10, r.y, r.w - 20, r.h), label, UI_TEXT_LABEL, UI_ALIGN_CENTER, text);
}

/* ---- button widgets with events ------------------------------ */
void ui_buttons_init(UiButtonSet *s)
{
    s->count = 0;
    s->hover = -1;
    s->pressed = -1;
    s->last_buttons = 0;
}

void ui_buttons_clear(UiButtonSet *s) { ui_buttons_init(s); }

int ui_buttons_add(UiButtonSet *s, UiButton *b)
{
    if (!b || s->count >= UI_MAX_BUTTONS)
        return -1;
    s->items[s->count++] = b;
    return 0;
}

void ui_buttons_draw(const UiButtonSet *s)
{
    for (int i = 0; i < s->count; i++)
    {
        const UiButton *b = s->items[i];
        UiState st = UI_STATE_NORMAL;
        if (!b->enabled)
            st = UI_STATE_DISABLED;
        else if (i == s->pressed)
            st = UI_STATE_PRESSED;
        else if (i == s->hover && s->pressed < 0)
            st = UI_STATE_HOVER;
        ui_button(b->r, b->label, b->kind, st);
    }
}

UiRect ui_buttons_bounds(const UiButtonSet *s)
{
    if (s->count == 0)
        return ui_rect(0, 0, 0, 0);
    int x0 = s->items[0]->r.x, y0 = s->items[0]->r.y;
    int x1 = x0 + s->items[0]->r.w, y1 = y0 + s->items[0]->r.h;
    for (int i = 1; i < s->count; i++)
    {
        UiRect r = s->items[i]->r;
        if (r.x < x0)
            x0 = r.x;
        if (r.y < y0)
            y0 = r.y;
        if (r.x + r.w > x1)
            x1 = r.x + r.w;
        if (r.y + r.h > y1)
            y1 = r.y + r.h;
    }
    return ui_rect(x0, y0, x1 - x0, y1 - y0);
}

static int buttons_hit(const UiButtonSet *s, int x, int y)
{
    for (int i = 0; i < s->count; i++)
        if (s->items[i]->enabled && s->items[i]->r.w && ui_contains(s->items[i]->r, x, y))
            return i;
    return -1;
}

static void buttons_fire(UiButton *b, UiButtonFn fn)
{
    if (fn)
        fn(b, b->user);
}

int ui_buttons_pointer(UiButtonSet *s, int x, int y, unsigned buttons)
{
    int changed = 0;
    int over = buttons_hit(s, x, y);
    int down = (buttons & 1) != 0, was_down = (s->last_buttons & 1) != 0;
    s->last_buttons = buttons;

    if (over != s->hover)
    {
        int old = s->hover;
        s->hover = over;
        changed = 1;
        if (old >= 0 && old < s->count)
            buttons_fire(s->items[old], s->items[old]->on_leave);
        if (over >= 0 && s->hover == over) /* a callback may have changed things */
            buttons_fire(s->items[over], s->items[over]->on_hover);
    }

    if (down && !was_down && over >= 0)
    {
        s->pressed = over;
        changed = 1;
        buttons_fire(s->items[over], s->items[over]->on_press);
    }
    else if (!down && was_down && s->pressed >= 0)
    {
        int p = s->pressed;
        s->pressed = -1;
        changed = 1;
        if (p < s->count && p == over) /* released on the same button = a click */
            buttons_fire(s->items[p], s->items[p]->on_click);
    }
    return changed;
}

void ui_progress(UiRect r, int percent)
{
    const UiTheme *t = THEME;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    int rad = r.h / 2;
    gfx_fill_round_rect(r.x, r.y, r.w, r.h, rad, ui_color(t->surface_alt));
    int fw = r.w * percent / 100;
    if (percent > 0 && fw < r.h)
        fw = r.h; /* keep the rounded end visible */
    if (percent > 0)
        gfx_fill_round_rect_gradient(r.x, r.y, fw, r.h, rad, ui_color(t->accent_hi), ui_color(t->accent));
}

void ui_toggle(UiRect r, int on)
{
    const UiTheme *t = THEME;
    int rad = r.h / 2;
    if (on)
        gfx_fill_round_rect_gradient(r.x, r.y, r.w, r.h, rad, ui_color(t->accent_hi), ui_color(t->accent));
    else
    {
        gfx_fill_round_rect(r.x, r.y, r.w, r.h, rad, ui_color(t->surface_alt));
        gfx_round_rect(r.x, r.y, r.w, r.h, rad, 1, ui_color(t->border));
    }
    int kr = rad - 3;
    int kx = on ? r.x + r.w - rad : r.x + rad;
    gfx_fill_circle_q(kx * 16, (r.y + rad) * 16, kr * 16, gfx_rgb(255, 255, 255), 255);
}

int ui_badge(int x, int y, const char *s, UiRgb color)
{
    const UiTheme *t = THEME;
    const Font *f = t->font_small;
    int w = gfx_text_w(f, s) + 20, h = font_line_height(f) + 6;
    gfx_fill_round_rect_alpha(x, y, w, h, h / 2, ui_color(color), 46);
    gfx_round_rect_alpha(x, y, w, h, h / 2, 1, ui_color(color), 110);
    gfx_fill_circle_q((x + 9) * 16, (y + h / 2) * 16, 3 * 16, ui_color(color), 255);
    gfx_text(f, x + 16, y + 3, s, ui_color(color));
    return w;
}

void ui_text_field(UiRect r, const char *text, const char *placeholder, int focused)
{
    const UiTheme *t = THEME;
    gfx_fill_round_rect(r.x, r.y, r.w, r.h, t->radius_m, ui_color(t->surface_alt));
    gfx_round_rect(r.x, r.y, r.w, r.h, t->radius_m, focused ? 2 : 1, ui_color(focused ? t->accent : t->border));
    UiRect in = ui_rect(r.x + 12, r.y, r.w - 24, r.h);
    if (text && *text)
        ui_text_in_color(in, text, UI_TEXT_BODY, UI_ALIGN_LEFT, t->text);
    else if (placeholder)
        ui_text_in_color(in, placeholder, UI_TEXT_BODY, UI_ALIGN_LEFT, t->text_dim);
}

/* ---- single-line text input ---------------------------------- */
void ui_edit_clear(UiEdit *e)
{
    e->text[0] = 0;
    e->len = 0;
    e->cursor = 0;
}

int ui_edit_key(UiEdit *e, const KeyEvent *k)
{
    if (!k->pressed)
        return 0;
    if (e->cursor > e->len)
        e->cursor = e->len;
    switch (k->key)
    {
    case KEY_LEFT:
        if (e->cursor == 0)
            return 0;
        e->cursor--;
        return 1;
    case KEY_RIGHT:
        if (e->cursor == e->len)
            return 0;
        e->cursor++;
        return 1;
    case KEY_HOME:
        if (e->cursor == 0)
            return 0;
        e->cursor = 0;
        return 1;
    case KEY_END:
        if (e->cursor == e->len)
            return 0;
        e->cursor = e->len;
        return 1;
    case KEY_BACKSPACE:
        if (e->cursor == 0)
            return 0;
        for (int i = e->cursor - 1; i < e->len; i++)
            e->text[i] = e->text[i + 1];
        e->cursor--;
        e->len--;
        return 1;
    case KEY_DELETE:
        if (e->cursor == e->len)
            return 0;
        for (int i = e->cursor; i < e->len; i++)
            e->text[i] = e->text[i + 1];
        e->len--;
        return 1;
    default:
        break;
    }
    if (k->ch >= 32 && k->ch < 127 && e->len < UI_EDIT_MAX)
    {
        for (int i = e->len; i >= e->cursor; i--) /* includes the 0 at the end */
            e->text[i + 1] = e->text[i];
        e->text[e->cursor++] = (char)k->ch;
        e->len++;
        return 1;
    }
    return 0;
}

/* pixel width of text[from .. to) */
static int edit_width(const UiEdit *e, int from, int to)
{
    char tmp[UI_EDIT_MAX + 1];
    int n = 0;
    for (int i = from; i < to && i < e->len; i++)
        tmp[n++] = e->text[i];
    tmp[n] = 0;
    return gfx_text_w(THEME->font_body, tmp);
}

void ui_edit_draw(UiRect r, const UiEdit *e, const char *placeholder, int focused)
{
    const UiTheme *t = THEME;
    ui_text_field(r, "", e->len ? 0 : placeholder, focused);
    UiRect in = ui_rect(r.x + 12, r.y, r.w - 24, r.h);
    if (in.w <= 4)
        return;
    int cur = e->cursor > e->len ? e->len : e->cursor;

    /* scroll: drop characters from the left until the cursor fits */
    int start = 0;
    while (start < cur && edit_width(e, start, cur) > in.w - 3)
        start++;

    int x = in.x;
    if (e->len)
    {
        GfxClip saved = gfx_get_clip();
        gfx_clip_intersect(in.x, in.y, in.w, in.h);
        ui_text_color(x, r.y + (r.h - ui_text_height(UI_TEXT_BODY)) / 2, e->text + start, UI_TEXT_BODY, t->text);
        gfx_restore_clip(saved);
    }
    if (focused)
    {
        int cx = x + edit_width(e, start, cur);
        if (cx > in.x + in.w - 2)
            cx = in.x + in.w - 2;
        gfx_fill_rect(cx, r.y + 7, 2, r.h - 14, ui_color(t->accent_hi));
    }
}
