/* HarisOS taskbar. Drawn only with HarisUI (include/ui.h). */
#include "taskbar.h"
#include "ui.h"

#define SLOT 40 /* size of one clickable app slot */
#define GAP 4
#define MAX_APPS 8

static const TaskbarApp DEFAULT_APPS[] = {
    {TASKBAR_ICON_FILES, 1, 0},
    {TASKBAR_ICON_TERMINAL, 1, 1},
    {TASKBAR_ICON_SETTINGS, 0, 0},
    {TASKBAR_ICON_BROWSER, 0, 0},
};
static const TaskbarApp *APPS = DEFAULT_APPS;
static int APP_COUNT = (int)(sizeof(DEFAULT_APPS) / sizeof(DEFAULT_APPS[0]));
static int HAS_TIME, T_HOUR, T_MIN, T_DAY, T_MON, T_YEAR;

void taskbar_set_apps(const TaskbarApp *apps, int count)
{
    if (!apps || count < 0)
        return;
    APPS = apps;
    APP_COUNT = count > MAX_APPS ? MAX_APPS : count;
}

void taskbar_set_time(int hour, int minute, int day, int month, int year)
{
    HAS_TIME = 1;
    T_HOUR = hour;
    T_MIN = minute;
    T_DAY = day;
    T_MON = month;
    T_YEAR = year;
}

static char *put2(char *o, int v) /* two digits */
{
    *o++ = (char)('0' + (v / 10) % 10);
    *o++ = (char)('0' + v % 10);
    return o;
}

static char *put4(char *o, int v) /* four digits */
{
    o = put2(o, v / 100);
    return put2(o, v % 100);
}

static UiIcon app_icon(TaskbarIcon i)
{
    switch (i)
    {
    case TASKBAR_ICON_FILES:
        return UI_ICON_FILES;
    case TASKBAR_ICON_TERMINAL:
        return UI_ICON_TERMINAL;
    case TASKBAR_ICON_SETTINGS:
        return UI_ICON_SETTINGS;
    default:
        return UI_ICON_BROWSER;
    }
}

void taskbar_draw(void)
{
    if (!gfx_ready())
        return;
    const UiTheme *t = ui_theme();
    int W = (int)gfx_width(), H = (int)gfx_height();
    int by = H - TASKBAR_HEIGHT;
    int cy = by + TASKBAR_HEIGHT / 2;

    /* translucent bar: the wallpaper shows through, with a light top edge */
    gfx_fill_rect_alpha(0, by, W, TASKBAR_HEIGHT, t->surface.r, t->surface.g, t->surface.b, 232);
    gfx_fill_rect_alpha(0, by, W, 1, 255, 255, 255, 40);

    int x = 10;

    /* start button: logo + name */
    const Font *nf = t->font_title;
    int name_w = gfx_text_w(nf, "Haris");
    int start_w = 8 + 28 + 8 + name_w + 14;
    gfx_fill_round_rect(x, cy - SLOT / 2 + 2, start_w, SLOT - 4, t->radius_m, ui_color(t->surface_alt));
    ui_icon(UI_ICON_LOGO, x + 8 + 14, cy, 28, t->text);
    ui_text_in(ui_rect(x + 8 + 28 + 8, cy - 14, name_w + 4, 28), "Haris", UI_TEXT_TITLE, UI_ALIGN_LEFT);
    x += start_w + GAP + 4;

    /* search pill (only on wide screens) */
    if (W >= 780)
    {
        int sw = 170;
        gfx_fill_round_rect(x, cy - 16, sw, 32, 16, ui_color(t->surface_alt));
        ui_icon(UI_ICON_SEARCH, x + 18, cy, 15, t->text_dim);
        ui_text_in(ui_rect(x + 34, cy - 16, sw - 44, 32), "Search", UI_TEXT_BODY, UI_ALIGN_LEFT);
        /* (colored by the "dim" style below) */
        x += sw + GAP + 6;
    }

    /* apps */
    for (int i = 0; i < APP_COUNT; i++)
    {
        int icx = x + SLOT / 2;
        if (APPS[i].active)
        {
            gfx_fill_round_rect(x, cy - SLOT / 2 + 2, SLOT, SLOT - 4, t->radius_m, ui_color(ui_rgb_mix(t->surface_alt, t->accent, 40)));
        }
        ui_icon(app_icon(APPS[i].icon), icx, cy - 1, 24, t->text);
        if (APPS[i].active)
            gfx_fill_round_rect(icx - 8, by + TASKBAR_HEIGHT - 6, 16, 3, 1, ui_color(t->accent_hi));
        else if (APPS[i].running)
            gfx_fill_circle_q(icx * 16 + 8, (by + TASKBAR_HEIGHT - 5) * 16 + 8, 2 * 16, ui_color(t->text_dim), 255);
        x += SLOT + GAP;
    }

    /* clock */
    char tbuf[8], dbuf[12];
    char *o;
    if (HAS_TIME)
    {
        o = put2(tbuf, T_HOUR);
        *o++ = ':';
        o = put2(o, T_MIN);
        *o = 0;
        o = put2(dbuf, T_DAY);
        *o++ = '/';
        o = put2(o, T_MON);
        *o++ = '/';
        o = put4(o, T_YEAR);
        *o = 0;
    }
    else
    {
        const char *a = "--:--", *b = "--/--/----";
        for (o = tbuf; (*o = *a); o++, a++)
        {
        }
        for (o = dbuf; (*o = *b); o++, b++)
        {
        }
    }
    int rx = W - 14;
    int dw = ui_text_width(dbuf, UI_TEXT_SMALL), tw = ui_text_width(tbuf, UI_TEXT_LABEL);
    int cw = dw > tw ? dw : tw;
    ui_text_in(ui_rect(rx - cw, cy - 20, cw, 20), tbuf, UI_TEXT_LABEL, UI_ALIGN_RIGHT);
    ui_text_in(ui_rect(rx - cw, cy, cw, 18), dbuf, UI_TEXT_SMALL, UI_ALIGN_RIGHT);

    /* tray: wifi, volume, battery in one rounded pill */
    int tray_w = 14 + 20 + 14 + 22 + 14 + 26 + 14;
    int tx = rx - cw - 14 - tray_w;
    if (tx > x)
    {
        gfx_fill_round_rect(tx, cy - 17, tray_w, 34, t->radius_m, ui_color(t->surface_alt));
        int ix = tx + 14 + 10;
        ui_icon(UI_ICON_WIFI, ix, cy, 20, t->text);
        ix += 10 + 14 + 11;
        ui_icon(UI_ICON_VOLUME, ix, cy, 22, t->text);
        ix += 11 + 14 + 13;
        ui_icon(UI_ICON_BATTERY, ix, cy, 26, t->text);
    }
}
