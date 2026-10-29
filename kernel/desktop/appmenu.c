/* HarisOS taskbar app menu (right click on a running app's button). */
#include "appmenu.h"
#include "startmenu.h"
#include "taskbar.h"
#include "ui.h"
#include "wm.h"
#include "desktop_internal.h"

#define MENU_W 212
#define MARGIN 8 /* gap to the taskbar and to the screen edge */
#define PAD 8
#define HEAD_H 40
#define ROW_H 38
#define ROWS 3

static int OPEN;
static Window *WIN;
static int ANCHOR_X;
static int SEL = -1;
static int PRESS = -1;
static unsigned LAST_BUTTONS;

/* ---- geometry ---- */
static int menu_h(void) { return PAD + HEAD_H + 10 + ROWS * ROW_H + 8 + PAD; }

static UiRect menu_rect(void)
{
    int W = (int)gfx_width(), H = (int)gfx_height();
    int h = menu_h();
    int x = ANCHOR_X - MENU_W / 2;
    if (x > W - MENU_W - MARGIN)
        x = W - MENU_W - MARGIN;
    if (x < MARGIN)
        x = MARGIN;
    int y = H - TASKBAR_HEIGHT - MARGIN - h;
    if (y < 4)
        y = 4;
    return ui_rect(x, y, MENU_W, h);
}
/* Everything the open menu draws, its shadow included (the same area its redraws mark). */
UiRect appmenu_bounds(void)
{
    UiRect m = menu_rect();
    return ui_rect(m.x - 36, m.y - 30, m.w + 72, m.h + 80);
}

static UiRect row_rect(int i)
{
    UiRect m = menu_rect();
    int y = m.y + PAD + HEAD_H + 10 + i * ROW_H + (i == 2 ? 8 : 0); /* a gap before "Close window" */
    return ui_rect(m.x + 6, y, m.w - 12, ROW_H - 4);
}

static void dmg_all(void)
{
    UiRect m = menu_rect();
    wm_add_screen_damage(ui_rect(m.x - 36, m.y - 30, m.w + 72, m.h + 80));
}

static void dmg_row(int i)
{
    if (i >= 0 && i < ROWS)
        wm_add_screen_damage(row_rect(i));
}

/* ---- open / close ---- */
int appmenu_is_open(void) { return OPEN; }

void appmenu_close(void)
{
    if (!OPEN)
        return;
    dmg_all();
    OPEN = 0;
    WIN = 0;
    SEL = PRESS = -1;
}

void appmenu_open(Window *w, int anchor_x)
{
    if (!w)
        return;
    startmenu_close(); /* only one menu at a time */
    if (OPEN)
        appmenu_close();
    OPEN = 1;
    WIN = w;
    ANCHOR_X = anchor_x;
    SEL = PRESS = -1;
    dmg_all();
}

static void activate(int row)
{
    Window *w = WIN;
    appmenu_close();
    if (!w || !wm_is_open(w))
        return;
    switch (row)
    {
    case 0:
        if (w->minimized)
            wm_activate(w);
        else
            wm_minimize(w);
        break;
    case 1:
        if (w->minimized)
            wm_activate(w);
        wm_set_maximized(w, !w->maximized);
        break;
    case 2:
        wm_request_close(w); /* "Close window" in the taskbar menu: the window may say no */
        break;
    default:
        break;
    }
}

/* ---- input ---- */
int appmenu_pointer(int x, int y, unsigned buttons)
{
    unsigned now = buttons & 3u;
    unsigned pressed = now & ~LAST_BUTTONS; /* buttons that went down just now */
    int left_release = (LAST_BUTTONS & 1u) && !(now & 1u);
    LAST_BUTTONS = now;
    if (!OPEN)
        return 0;
    if (!WIN || !wm_is_open(WIN))
    {
        appmenu_close();
        return 0;
    }

    int valid = x >= 0;
    int over = valid && ui_contains(menu_rect(), x, y);
    if (pressed && valid && !over) /* any click away from the menu closes it and still goes through */
    {
        appmenu_close();
        return 0;
    }

    int hit = -1;
    if (over)
        for (int i = 0; i < ROWS; i++)
            if (ui_contains(row_rect(i), x, y))
                hit = i;
    if (hit != SEL && PRESS < 0)
    {
        dmg_row(SEL);
        SEL = hit;
        dmg_row(SEL);
    }
    if ((pressed & 1u) && over)
    {
        PRESS = hit;
        dmg_row(hit);
    }
    if (left_release)
    {
        int act = (PRESS >= 0 && hit == PRESS) ? PRESS : -1;
        int old = PRESS;
        PRESS = -1;
        dmg_row(old);
        if (act >= 0)
            activate(act);
    }
    return over;
}

int appmenu_key(const KeyEvent *k)
{
    if (!OPEN || !k->pressed)
        return OPEN;
    switch (k->key)
    {
    case KEY_ESC:
        appmenu_close();
        break;
    case KEY_ENTER:
        if (SEL >= 0)
            activate(SEL);
        break;
    case KEY_UP:
    case KEY_DOWN:
    {
        int back = k->key == KEY_UP;
        int old = SEL;
        SEL = SEL < 0 ? (back ? ROWS - 1 : 0) : (SEL + (back ? ROWS - 1 : 1)) % ROWS;
        dmg_row(old);
        dmg_row(SEL);
        break;
    }
    default:
        break;
    }
    return 1;
}

/* ---- drawing ---- */
static UiIcon app_icon(int icon)
{
    switch ((TaskbarIcon)icon)
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

void appmenu_draw(void)
{
    if (!OPEN || !gfx_ready())
        return;
    if (!WIN || !wm_is_open(WIN))
        return; /* closed by something else: the next input event removes the menu */
    const UiTheme *prev = ui_theme();
    ui_set_theme(startmenu_theme()); /* light glass look, whatever the desktop theme is */
    const UiTheme *t = startmenu_theme();
    UiRect m = menu_rect();

    startmenu_glass(m, 16);

    /* header: which window this is */
    int hy = m.y + PAD;
    UiRect tile = ui_rect(m.x + PAD + 2, hy + 2, 36, 36);
    gfx_fill_round_rect_alpha(tile.x, tile.y, tile.w, tile.h, 10, gfx_rgb(255, 255, 255), 215);
    gfx_round_rect_alpha(tile.x, tile.y, tile.w, tile.h, 10, 1, ui_color(t->border), 150);
    ui_icon(app_icon(WIN->icon), tile.x + tile.w / 2, tile.y + tile.h / 2, 22, t->text);
    int tx = tile.x + tile.w + 10;
    ui_text_in(ui_rect(tx, hy, m.x + m.w - PAD - tx, 22), WIN->title, UI_TEXT_LABEL, UI_ALIGN_LEFT);
    ui_text_in(ui_rect(tx, hy + 20, m.x + m.w - PAD - tx, 18),
               WIN->minimized ? "Minimized" : (WIN->maximized ? "Maximized" : "Running"), UI_TEXT_SMALL,
               UI_ALIGN_LEFT);
    gfx_fill_rect_alpha(m.x + PAD, hy + HEAD_H + 4, m.w - 2 * PAD, 1, 40, 60, 110, 40);

    /* rows */
    const char *label[ROWS];
    label[0] = WIN->minimized ? "Restore" : "Minimize";
    label[1] = WIN->maximized ? "Restore size" : "Maximize";
    label[2] = "Close window";
    for (int i = 0; i < ROWS; i++)
    {
        UiRect r = row_rect(i);
        UiRgb tint = i == 2 ? t->danger : t->accent;
        if (i == SEL)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_m, ui_color(tint), PRESS == i ? 56 : 30);
        ui_text_in_color(ui_rect(r.x + 12, r.y, r.w - 24, r.h), label[i], UI_TEXT_LABEL, UI_ALIGN_LEFT,
                         i == 2 ? t->danger : t->text);
    }
    ui_set_theme(prev);
}
