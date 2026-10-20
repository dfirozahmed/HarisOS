/* Device Manager app: the list from kernel/core/devmgr.c grouped by category, with a
 * details strip for the selected device. Up/Down/Home/End/PageUp/PageDown or a click select.
 * An app uses only public headers (ui.h, wm.h, desktop.h, devmgr.h) - never desktop_internal.h. */
#include "app.h"
#include "desktop.h"
#include "devmgr.h"
#include "taskbar.h"
#include "ui.h"
#include "wm.h"

#define ROW_H 26
#define DETAIL_H 78
#define REFRESH_W 96 /* the Refresh button sits at the right end of the details strip */

/* A flat row: a category header (dev < 0) or one device. */
typedef struct
{
    short cat;
    short dev;
} Row;

static Window *DM_WIN;
static Row ROWS[DEVMGR_MAX + DEV_CAT_COUNT];
static int NROWS;
static int SEL = -1;  /* selected row (always a device row), -1 = none */
static int SCROLL;    /* first visible row */

static void rebuild_rows(void)
{
    NROWS = 0;
    SEL = -1;
    SCROLL = 0;
    for (int c = 0; c < DEV_CAT_COUNT; c++)
    {
        int header = -1;
        for (int i = 0; i < devmgr_count(); i++)
        {
            if (devmgr_get(i)->category != (DevCategory)c)
                continue;
            if (header < 0)
            {
                header = NROWS;
                ROWS[NROWS].cat = (short)c;
                ROWS[NROWS++].dev = -1;
            }
            ROWS[NROWS].cat = (short)c;
            ROWS[NROWS++].dev = (short)i;
            if (SEL < 0)
                SEL = NROWS - 1;
        }
    }
}

static int same_text(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static UiRect list_rect(UiRect body)
{
    body = ui_inset(body, 8);
    ui_cut_bottom(&body, DETAIL_H + 8);
    return body;
}

static int visible_rows(UiRect body)
{
    int n = list_rect(body).h / ROW_H;
    return n < 1 ? 1 : n;
}

/* Move the selection by `step` device rows (headers are skipped) and keep it on screen. */
static void select_to(UiRect body, int row)
{
    if (row < 0 || row >= NROWS || ROWS[row].dev < 0)
        return;
    SEL = row;
    int vis = visible_rows(body);
    int top = row;
    if (row > 0 && ROWS[row - 1].dev < 0)
        top = row - 1; /* show the category header above the first device */
    if (top < SCROLL)
        SCROLL = top;
    if (row >= SCROLL + vis)
        SCROLL = row - vis + 1;
    if (SCROLL < 0)
        SCROLL = 0;
}

static void select_step(UiRect body, int dir)
{
    for (int r = SEL + dir; r >= 0 && r < NROWS; r += dir)
    {
        if (ROWS[r].dev >= 0)
        {
            select_to(body, r);
            return;
        }
    }
}

static void dm_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    UiRect list = list_rect(body);
    int vis = visible_rows(body);

    if (NROWS == 0)
    {
        ui_text_in(list, "No devices found.", UI_TEXT_DIM, UI_ALIGN_CENTER);
        return;
    }
    if (SCROLL > NROWS - 1)
        SCROLL = NROWS - 1;

    for (int i = 0; i < vis && SCROLL + i < NROWS; i++)
    {
        int ri = SCROLL + i;
        const Row *row = &ROWS[ri];
        UiRect r = ui_rect(list.x, list.y + i * ROW_H, list.w, ROW_H);
        if (row->dev < 0)
        {
            ui_text_in(ui_rect(r.x + 4, r.y, r.w - 8, r.h), devmgr_category_name((DevCategory)row->cat),
                       UI_TEXT_LABEL, UI_ALIGN_LEFT);
            continue;
        }
        const DevEntry *e = devmgr_get(row->dev);
        if (ri == SEL)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h - 2, t->radius_s, ui_color(t->accent), 40);
        int status_w = 74;
        ui_text_in(ui_rect(r.x + 22, r.y, r.w - 22 - status_w - 8, r.h), e->name, UI_TEXT_BODY, UI_ALIGN_LEFT);
        ui_text_in_color(ui_rect(r.x + r.w - status_w - 4, r.y, status_w, r.h),
                         e->has_driver ? "Driver" : "No driver", UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                         e->has_driver ? t->success : t->text_dim);
    }

    /* details strip */
    UiRect d = ui_inset(body, 8);
    d = ui_cut_bottom(&d, DETAIL_H);
    ui_separator(d.x, d.y - 4, d.w);
    if (SEL >= 0)
    {
        const DevEntry *e = devmgr_get(ROWS[SEL].dev);
        int tw = d.w - 8 - REFRESH_W - 8; /* leave room for the Refresh button */
        ui_text_in(ui_rect(d.x + 4, d.y + 4, tw, 24), e->name, UI_TEXT_TITLE, UI_ALIGN_LEFT);
        ui_text_in(ui_rect(d.x + 4, d.y + 30, tw, 20), e->detail, UI_TEXT_DIM, UI_ALIGN_LEFT);
        ui_text_in_color(ui_rect(d.x + 4, d.y + 52, tw, 20),
                         e->note[0] ? e->note : (e->has_driver ? "Driver loaded" : "No HarisOS driver for this device"),
                         UI_TEXT_SMALL, UI_ALIGN_LEFT, e->has_driver ? t->success : t->warning);
    }
}

static int dm_press(Window *w, int x, int y)
{
    UiRect body = wm_content(w);
    UiRect list = list_rect(body);
    if (!ui_contains(list, x, y))
        return 0;
    int row = SCROLL + (y - list.y) / ROW_H;
    if (row < 0 || row >= NROWS || ROWS[row].dev < 0 || row == SEL)
        return 0;
    select_to(body, row);
    return 1;
}

/* wheel: scroll the view by 3 rows per step; the selection stays where it is */
#define WHEEL_ROWS 3
static int dm_wheel(Window *w, int steps)
{
    int max_scroll = NROWS - visible_rows(wm_content(w));
    if (max_scroll < 0)
        max_scroll = 0;
    int old = SCROLL;
    SCROLL += steps * WHEEL_ROWS;
    if (SCROLL > max_scroll)
        SCROLL = max_scroll;
    if (SCROLL < 0)
        SCROLL = 0;
    return SCROLL != old;
}

/* Ask the system again (PCI + drivers) and rebuild the rows. The selection stays on the same
 * device (matched by name and detail) if it is still there, otherwise it goes to the first one. */
static void refresh_list(void)
{
    char name[sizeof(((DevEntry *)0)->name)], detail[sizeof(((DevEntry *)0)->detail)];
    int had = SEL >= 0;
    if (had)
    {
        const DevEntry *e = devmgr_get(ROWS[SEL].dev);
        for (unsigned i = 0; i < sizeof name; i++)
            name[i] = e->name[i];
        for (unsigned i = 0; i < sizeof detail; i++)
            detail[i] = e->detail[i];
    }
    int top = SCROLL;
    devmgr_refresh();
    rebuild_rows();
    if (!had)
        return;
    for (int r = 0; r < NROWS; r++)
    {
        const DevEntry *e = ROWS[r].dev >= 0 ? devmgr_get(ROWS[r].dev) : 0;
        if (e && same_text(e->name, name) && same_text(e->detail, detail))
        {
            SEL = r;
            break;
        }
    }
    SCROLL = top < NROWS ? top : 0;
    if (DM_WIN)
        select_to(wm_content(DM_WIN), SEL); /* keep the selection on screen */
}

static void on_refresh(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    refresh_list();
}

static void dm_key(Window *w, const KeyEvent *k)
{
    if (k->pressed && k->key == KEY_F5)
    {
        refresh_list();
        return;
    }
    if (!k->pressed || NROWS == 0)
        return;
    UiRect body = wm_content(w);
    int page = visible_rows(body) - 1;
    switch (k->key)
    {
    case KEY_UP:
        select_step(body, -1);
        break;
    case KEY_DOWN:
        select_step(body, 1);
        break;
    case KEY_PAGEUP:
        for (int i = 0; i < page; i++)
            select_step(body, -1);
        break;
    case KEY_PAGEDOWN:
        for (int i = 0; i < page; i++)
            select_step(body, 1);
        break;
    case KEY_HOME:
        for (int r = 0; r < NROWS; r++)
            if (ROWS[r].dev >= 0)
            {
                select_to(body, r);
                break;
            }
        break;
    case KEY_END:
        for (int r = NROWS - 1; r >= 0; r--)
            if (ROWS[r].dev >= 0)
            {
                select_to(body, r);
                break;
            }
        break;
    case KEY_ESC:
        wm_close(w);
        break;
    default:
        break;
    }
}

static void dm_close(Window *w)
{
    (void)w;
    DM_WIN = 0;
}

static void devmgr_app_open(void)
{
    if (DM_WIN)
    {
        wm_activate(DM_WIN);
        return;
    }
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 560, h = 440;
    if (w > W - 20)
        w = W - 20;
    if (h > H - TASKBAR_HEIGHT - 8)
        h = H - TASKBAR_HEIGHT - 8;
    int x = (W - w) / 2, y = (H - TASKBAR_HEIGHT - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;

    devmgr_refresh(); /* the boot-time list may be out of date (USB devices come and go) */
    rebuild_rows();
    Window *win = wm_create("Device Manager", x, y, w, h, dm_draw, 0);
    if (!win)
    {
        desktop_set_status("Window limit reached - close one first.");
        wm_invalidate();
        return;
    }
    win->on_key = dm_key;
    win->on_press = dm_press;
    win->on_wheel = dm_wheel;
    win->on_close = dm_close;
    win->icon = TASKBAR_ICON_SETTINGS;
    DM_WIN = win;
    desktop_set_status(0);

    /* Refresh button in the corner of the details strip (F5 does the same) */
    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    wm_add_button_ex(win, "Refresh", UI_BTN_SECONDARY, ui_rect(cw - 8 - REFRESH_W, ch - 8 - 34, REFRESH_W, 34),
                     WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_refresh, 0);
}

const AppInfo APP_DEVMGR = {
    .name = "Device Manager",
    .desc = "PCI and input devices",
    .icon = UI_ICON_FILES,
    .open = devmgr_app_open,
};
