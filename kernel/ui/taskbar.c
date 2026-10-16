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
#define HLOGO 26
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

    int name_w = gfx_text_width("HARIS", 2);
    int start_w = 8 + HLOGO + 8 + name_w + 12;
    gfx_fill_round_rect(x, cy - SLOT / 2 + 2, start_w, SLOT - 4, t->radius_m, ui_color(t->surface_alt));
    ui_icon(UI_ICON_LOGO, x + 8 + 14, cy, 28, t->text);
    gfx_draw_text(x + 8 + HLOGO + 8, cy - 7, "HARIS", ui_color(t->text), 2);
    x += start_w + GAP;

    /* search pill (only on wide screens) */
    if (W >= 780)
    {
        int sw = 170;
        gfx_fill_round_rect(x, cy - 16, sw, 32, 16, ui_color(t->surface_alt));
        ui_icon(UI_ICON_SEARCH, x + 18, cy, 15, t->text_dim);
        gfx_draw_text(x + 36, cy - 3, "SEARCH", ui_color(t->text), 1);
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
    int dw = gfx_text_width(dbuf, 1), tw = gfx_text_width(tbuf, 1);
    int cw = dw > tw ? dw : tw;
    int ty = by + TASKBAR_HEIGHT / 2 - 9;
    gfx_draw_text(rx - tw, ty, tbuf, ui_color(t->text), 1);
    gfx_draw_text(rx - dw, ty + 11, dbuf, ui_color(t->text_dim), 1);

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
