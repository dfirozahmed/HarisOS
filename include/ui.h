#ifndef HARIS_UI_H
#define HARIS_UI_H

#include "gfx.h"

/* ---- colors -------------------------------------------------- */
typedef struct { unsigned char r, g, b; } UiRgb;

UiRgb ui_rgb(unsigned char r, unsigned char g, unsigned char b);
UiRgb ui_rgb_mix(UiRgb a, UiRgb b, unsigned char t); /* t: 0 = a, 255 = b */
GfxColor ui_color(UiRgb c);                           /* to the framebuffer format */

/* ---- theme --------------------------------------------------- */
typedef struct
{
    /* wallpaper */
    UiRgb bg_top, bg_bottom, glow1, glow2;
    /* surfaces */
    UiRgb surface;     /* windows, cards, menus */
    UiRgb surface_alt; /* inputs, secondary buttons, tracks */
    UiRgb titlebar;
    UiRgb border;
    /* text */
    UiRgb text, text_dim, text_on_accent;
    /* colors with a meaning */
    UiRgb accent, accent_hi;
    UiRgb success, warning, danger;
    /* shape and spacing */
    int radius_s, radius_m, radius_l;
    int pad, gap;
    int titlebar_h;
    int shadow; /* 0..255 */
} UiTheme;

extern const UiTheme ui_theme_dark, ui_theme_light;
const UiTheme *ui_theme(void);
void ui_set_theme(const UiTheme *t); /* NULL = default (dark) */

/* ---- icons --------------------------------------------------- */
typedef enum
{
    UI_ICON_LOGO,     /* HarisOS "H" */
    UI_ICON_FILES,
    UI_ICON_TERMINAL,
    UI_ICON_SETTINGS,
    UI_ICON_BROWSER,
    UI_ICON_SEARCH,
    UI_ICON_WIFI,
    UI_ICON_VOLUME,
    UI_ICON_BATTERY,
    UI_ICON_CLOSE,
    UI_ICON_CHECK
} UiIcon;

/* Draw a vector icon centered at (cx, cy), `size` pixels wide. App icons
 * (files, terminal, browser, logo) have their own colors; the others use `tint`. */
void ui_icon(UiIcon icon, int cx, int cy, int size, UiRgb tint);
#endif
