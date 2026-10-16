/* HarisUI: theme */
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
    .shadow = 150
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
    .shadow = 90
};

static const UiTheme *THEME = &ui_theme_dark;

const UiTheme *ui_theme(void) { return THEME; }
void ui_set_theme(const UiTheme *t) { THEME = t ? t : &ui_theme_dark; }