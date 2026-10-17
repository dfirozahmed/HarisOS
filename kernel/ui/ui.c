/* HarisUI: theme, layout, text */
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
    .surface_alt = RGB(232, 237, 246),
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

static const UiTheme *THEME = &ui_theme_light;

const UiTheme *ui_theme(void) { return THEME; }
void ui_set_theme(const UiTheme *t) { THEME = t ? t : &ui_theme_dark; }

/* ---- layout -------------------------------------------------- */
UiRect ui_rect(int x, int y, int w, int h)
{
    UiRect r = {x, y, w, h};
    return r;
}

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