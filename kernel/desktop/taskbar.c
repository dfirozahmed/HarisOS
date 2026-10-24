/* HarisOS taskbar. Drawn only with HarisUI (include/ui.h). */
#include "taskbar.h"
#include "ui.h"

#define SLOT 40 /* size of one clickable app slot */
#define GAP 4
#define MAX_APPS TASKBAR_MAX_APPS
#define MIN_SLOT 22       /* the narrowest app button when many windows are open */
#define RIGHT_RESERVE 290 /* room kept for the tray and the clock on the right */

static TaskbarApp APPS[MAX_APPS] = {};
static int APP_COUNT = 4;
static int HAS_TIME, T_HOUR, T_MIN, T_DAY, T_MON, T_YEAR;

void taskbar_set_apps(const TaskbarApp *apps, int count)
{
    if (!apps || count < 0)
        return;
    APP_COUNT = count > MAX_APPS ? MAX_APPS : count;
    for (int i = 0; i < APP_COUNT; i++)
        APPS[i] = apps[i];
}

/* ---- clicks -------------------------------------------------- */
static UiButton ITEM[1 + MAX_APPS];
static UiButtonSet SET;
static int SET_READY;
static TaskbarStartFn CB_START;
static TaskbarAppFn CB_APP;

static void item_click(UiButton *b, void *user)
{
    (void)b;
    int i = (int)(unsigned long long)user;
    if (i == 0)
    {
        if (CB_START)
            CB_START();
    }
    else if (CB_APP)
        CB_APP(i - 1);
}

static void init_set(void)
{
    if (SET_READY)
        return;
    SET_READY = 1;
    ui_buttons_init(&SET);
    for (int i = 0; i < 1 + MAX_APPS; i++)
    {
        ITEM[i].r = ui_rect(0, 0, 0, 0);
        ITEM[i].label = "";
        ITEM[i].kind = UI_BTN_GHOST;
        ITEM[i].enabled = 1;
        ITEM[i].on_click = item_click;
        ITEM[i].user = (void *)(unsigned long long)i;
        ui_buttons_add(&SET, &ITEM[i]);
    }
}

void taskbar_set_callbacks(TaskbarStartFn on_start, TaskbarAppFn on_app)
{
    CB_START = on_start;
    CB_APP = on_app;
}

int taskbar_pointer(int x, int y, unsigned buttons)
{
    init_set();
    return ui_buttons_pointer(&SET, x, y, buttons);
}

int taskbar_app_at(int x, int y)
{
    init_set();
    for (int i = 0; i < APP_COUNT; i++)
        if (ui_contains(ITEM[1 + i].r, x, y))
            return i;
    return -1;
}

int taskbar_app_rect(int index, int *x, int *y, int *w, int *h)
{
    init_set();
    if (index < 0 || index >= APP_COUNT)
        return 0;
    UiRect r = ITEM[1 + index].r;
    *x = r.x;
    *y = r.y;
    *w = r.w;
    *h = r.h;
    return 1;
}

void taskbar_bounds(int *x, int *y, int *w, int *h)
{
    *x = 0;
    *y = (int)gfx_height() - TASKBAR_HEIGHT;
    *w = (int)gfx_width();
    *h = TASKBAR_HEIGHT;
}

/* background color of a button: normal, hover or pressed */
static UiRgb state_bg(UiRgb base, int idx, int active)
{
    if (SET.pressed == idx)
        return ui_rgb_mix(base, ui_rgb(0, 0, 0), 55);
    if (SET.hover == idx && SET.pressed < 0)
        return ui_rgb_mix(base, ui_rgb(255, 255, 255), active ? 22 : 34);
    return base;
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
    case TASKBAR_ICON_LOGO:
        return UI_ICON_LOGO;
    case TASKBAR_ICON_TASKMGR:
        return UI_ICON_TASKMGR;
    case TASKBAR_ICON_DISPLAY:
        return UI_ICON_DISPLAY;
    case TASKBAR_ICON_DEVMGR:
        return UI_ICON_DEVMGR;
    case TASKBAR_ICON_NOTEPAD:
        return UI_ICON_NOTEPAD;
    case TASKBAR_ICON_WIFI:
        return UI_ICON_WIFI;
    default:
        return UI_ICON_BROWSER;
    }
}

void taskbar_draw(void)
{
    if (!gfx_ready())
        return;
    init_set();
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
    ITEM[0].r = ui_rect(x, cy - SLOT / 2 + 2, start_w, SLOT - 4);
    gfx_fill_round_rect(x, cy - SLOT / 2 + 2, start_w, SLOT - 4, t->radius_m, ui_color(state_bg(t->surface_alt, 0, 0)));
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
    for (int i = 0; i < MAX_APPS; i++)
        ITEM[1 + i].r = ui_rect(0, 0, 0, 0); /* unused buttons: no hit area */
    /* The buttons shrink (down to MIN_SLOT) so that more windows fit; what still does not fit is not shown (the task manager lists all). */
    int avail = W - RIGHT_RESERVE - x;
    int fit = avail > 0 ? avail / (MIN_SLOT + GAP) : 0;
    int shown = APP_COUNT < fit ? APP_COUNT : fit;
    int slot = SLOT;
    if (shown > 0 && shown * (SLOT + GAP) > avail)
        slot = avail / shown - GAP;
    if (slot < MIN_SLOT)
        slot = MIN_SLOT;
    int isz = slot < 32 ? slot - 8 : 24; /* icon size */
    for (int i = 0; i < shown; i++)
    {
        int icx = x + slot / 2;
        ITEM[1 + i].r = ui_rect(x, cy - SLOT / 2 + 2, slot, SLOT - 4);
        UiRgb bg = APPS[i].active ? ui_rgb_mix(t->surface_alt, t->accent, 40) : t->surface;
        if (APPS[i].active || SET.hover == 1 + i || SET.pressed == 1 + i)
        {
            if (!APPS[i].active)
                bg = t->surface_alt;
            gfx_fill_round_rect(x, cy - SLOT / 2 + 2, slot, SLOT - 4, t->radius_m, ui_color(state_bg(bg, 1 + i, APPS[i].active)));
        }
        ui_icon(app_icon(APPS[i].icon), icx, cy - 1, isz, APPS[i].minimized ? t->text_dim : t->text);
        if (APPS[i].active)
            gfx_fill_round_rect(icx - (isz / 3), by + TASKBAR_HEIGHT - 6, (isz / 3) * 2, 3, 1, ui_color(t->accent_hi));
        else if (APPS[i].running)
            gfx_fill_circle_q(icx * 16 + 8, (by + TASKBAR_HEIGHT - 5) * 16 + 8, 2 * 16, ui_color(t->text_dim), 255);
        x += slot + GAP;
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
