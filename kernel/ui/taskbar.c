/* HarisOS taskbar. All drawing uses the gfx layer (include/gfx.h). */
#include "taskbar.h"
#include "gfx.h"

#define SLOT 40 /* size of one clickable slot */
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

/* ---- tiny helpers ------------------------------------------ */
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

/* ---- icons: each is drawn around the center point (cx, cy) ---- */
static void icon_files(int cx, int cy)
{
    gfx_fill_round_rect(cx - 11, cy - 9, 10, 6, 2, gfx_rgb(222, 150, 40));  /* tab */
    gfx_fill_round_rect(cx - 11, cy - 6, 22, 15, 3, gfx_rgb(238, 168, 52)); /* back */
    gfx_fill_round_rect(cx - 11, cy - 3, 22, 12, 3, gfx_rgb(252, 201, 92)); /* front */
}

static void icon_terminal(int cx, int cy)
{
    GfxColor green = gfx_rgb(110, 231, 160);
    gfx_fill_round_rect(cx - 11, cy - 9, 22, 18, 3, gfx_rgb(95, 110, 135)); /* border */
    gfx_fill_round_rect(cx - 10, cy - 8, 20, 16, 2, gfx_rgb(18, 24, 36));   /* screen */
    gfx_line(cx - 7, cy - 3, cx - 3, cy, green);
    gfx_line(cx - 7, cy - 2, cx - 3, cy + 1, green);
    gfx_line(cx - 3, cy, cx - 7, cy + 3, green);
    gfx_line(cx - 3, cy + 1, cx - 7, cy + 4, green);
    gfx_hline(cx, cy + 4, 6, green);
}

static void icon_settings(int cx, int cy)
{
    GfxColor metal = gfx_rgb(176, 188, 208);
    static const signed char teeth[8][2] = {{-9, 0}, {9, 0}, {0, -9}, {0, 9}, {-6, -6}, {6, -6}, {-6, 6}, {6, 6}};
    for (int i = 0; i < 8; i++)
        gfx_fill_rect(cx + teeth[i][0] - 2, cy + teeth[i][1] - 2, 4, 4, metal);
    gfx_fill_circle(cx, cy, 7, metal);
    gfx_fill_circle(cx, cy, 3, gfx_rgb(28, 38, 56));
}

static void icon_browser(int cx, int cy)
{
    GfxColor line = gfx_rgb(200, 226, 255);
    gfx_fill_circle(cx, cy, 10, gfx_rgb(52, 130, 240));
    gfx_circle(cx, cy, 10, line);
    gfx_circle(cx, cy, 5, line);
    gfx_hline(cx - 10, cy, 21, line);
    gfx_vline(cx, cy - 10, 21, line);
}

static void draw_icon(TaskbarIcon icon, int cx, int cy)
{
    switch (icon)
    {
    case TASKBAR_ICON_FILES:
        icon_files(cx, cy);
        break;
    case TASKBAR_ICON_TERMINAL:
        icon_terminal(cx, cy);
        break;
    case TASKBAR_ICON_SETTINGS:
        icon_settings(cx, cy);
        break;
    case TASKBAR_ICON_BROWSER:
        icon_browser(cx, cy);
        break;
    }
}

/* ---- tray icons (x = left edge, cy = vertical center) --------- */
static void tray_wifi(int x, int cy, GfxColor c)
{
    for (int i = 0; i < 4; i++)
        gfx_fill_rect(x + i * 5, cy + 6 - (3 + i * 3), 3, 3 + i * 3, c);
}

static void tray_volume(int x, int cy, GfxColor c)
{
    gfx_fill_rect(x, cy - 3, 4, 7, c);
    for (int i = 0; i < 3; i++)
        gfx_vline(x + 4 + i, cy - 4 - i, 9 + 2 * i, c);
    gfx_vline(x + 10, cy - 3, 7, c);
    gfx_vline(x + 13, cy - 5, 11, c);
}

static void tray_battery(int x, int cy, GfxColor c)
{
    gfx_rect(x, cy - 5, 20, 11, c);
    gfx_fill_rect(x + 20, cy - 2, 2, 5, c);
    gfx_fill_rect(x + 2, cy - 3, 16, 7, gfx_rgb(90, 214, 140));
}

/* ---- Haris logo */
#define HLOGO 26
static void draw_h_logo(int x, int y)
{
    GfxColor white = gfx_rgb(255, 255, 255);
    gfx_fill_round_rect(x, y, HLOGO, HLOGO, 7, gfx_rgb(70, 150, 255));
    gfx_fill_rect(x + 7, y + 6, 4, 14, white);  /* left stem */
    gfx_fill_rect(x + 15, y + 6, 4, 14, white); /* right stem */
    gfx_fill_rect(x + 7, y + 11, 12, 4, white); /* crossbar */
}

/* ---- the taskbar ------------------------------------------- */
void taskbar_draw(void)
{
    if (!gfx_ready())
        return;

    int W = (int)gfx_width(), H = (int)gfx_height();
    int by = H - TASKBAR_HEIGHT;
    int cy = by + TASKBAR_HEIGHT / 2 - 2; /* leave room for the indicators below */
    GfxColor accent = gfx_rgb(70, 150, 255);
    GfxColor text = gfx_rgb(236, 242, 252);
    GfxColor dim = gfx_rgb(150, 164, 190);

    /* translucent bar with a soft highlight along the top edge */
    gfx_fill_rect_alpha(0, by, W, TASKBAR_HEIGHT, 14, 22, 38, 228);
    gfx_fill_rect_alpha(0, by, W, 1, 255, 255, 255, 45);

    /* left side: start button (H logo + "HARIS"), then search and apps */
    int search_w = W >= 760 ? 150 : 0;
    int n = APP_COUNT;
    int x = 10;

    int name_w = gfx_text_width("HARIS", 2);
    int start_w = 8 + HLOGO + 8 + name_w + 12;
    gfx_fill_round_rect(x, cy - SLOT / 2 + 2, start_w, SLOT - 4, 8, gfx_rgb(36, 48, 72));
    draw_h_logo(x + 8, cy - HLOGO / 2);
    gfx_draw_text(x + 8 + HLOGO + 8, cy - 7, "HARIS", text, 2);
    x += start_w + GAP;

    if (search_w)
    {
        gfx_fill_round_rect(x, cy - 15, search_w, 30, 15, gfx_rgb(34, 45, 68));
        gfx_circle(x + 18, cy - 1, 5, dim);
        gfx_line(x + 22, cy + 3, x + 26, cy + 7, dim);
        gfx_draw_text(x + 36, cy - 3, "SEARCH", gfx_rgb(122, 136, 162), 1);
        x += search_w + GAP;
    }

    for (int i = 0; i < n; i++)
    {
        int icx = x + SLOT / 2;
        if (APPS[i].active)
            gfx_fill_round_rect(x, cy - SLOT / 2 + 2, SLOT, SLOT - 4, 8, gfx_rgb(46, 60, 88));
        draw_icon(APPS[i].icon, icx, cy);
        if (APPS[i].active)
            gfx_fill_round_rect(icx - 8, by + TASKBAR_HEIGHT - 6, 16, 3, 1, accent);
        else if (APPS[i].running)
            gfx_fill_round_rect(icx - 2, by + TASKBAR_HEIGHT - 6, 4, 3, 1, dim);
        x += SLOT + GAP;
    }

    /* right: clock (two lines) */
    char tbuf[8], dbuf[12];
    if (HAS_TIME)
    {
        char *o = put2(tbuf, T_HOUR);
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
        const char *t = "--:--", *d = "--/--/----";
        char *o = tbuf;
        while ((*o++ = *t++))
        {
        }
        o = dbuf;
        while ((*o++ = *d++))
        {
        }
    }
    int rx = W - 14;
    int dw = gfx_text_width(dbuf, 1), tw = gfx_text_width(tbuf, 1);
    int ty = by + TASKBAR_HEIGHT / 2 - 9;
    gfx_draw_text(rx - tw, ty, tbuf, text, 1);
    gfx_draw_text(rx - dw, ty + 11, dbuf, dim, 1);

    /* tray icons, right to left, with a divider before the clock */
    int tx = rx - dw - 14;
    int mid = by + TASKBAR_HEIGHT / 2;
    gfx_vline(tx, mid - 11, 22, gfx_rgb(60, 74, 100));
    tx -= 12 + 22;
    tray_battery(tx, mid, text);
    tx -= 14 + 14;
    tray_volume(tx, mid, text);
    tx -= 12 + 18;
    tray_wifi(tx, mid, text);
}
