/* File Explorer: the AppInfo and opening / closing the window. */
#include "explorer.h"

static void ex_close(Window *w)
{
    (void)w;
    EX_WIN = 0;
    EX_RENAME_ID = -1;
    EX_MENU.open = 0;
    EX_TIP_BTN = -1;
    EX_TIP_SHOWN = 0;
    EX_SIDE_DRAG = 0;
}

static void explorer_app_open(void)
{
    if (EX_WIN)
    {
        wm_activate(EX_WIN);
        return;
    }
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 760, h = 540;
    if (w > W - 20)
        w = W - 20;
    if (h > H - TASKBAR_HEIGHT - 8)
        h = H - TASKBAR_HEIGHT - 8;
    int x = (W - w) / 2, y = (H - TASKBAR_HEIGHT - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;

    Window *win = wm_create("File Explorer", x, y, w, h, ex_draw, 0);
    if (!win)
    {
        desktop_set_status("Window limit reached - close one first.");
        wm_invalidate();
        return;
    }
    win->on_key = ex_key;
    win->on_press = ex_press;
    win->on_wheel = ex_wheel;
    win->on_drag = ex_drag;
    win->on_rpress = ex_rpress;
    win->on_hover = ex_hover;
    win->on_tick = ex_tick;
    win->on_cursor = ex_cursor;
    win->on_close = ex_close;
    win->icon = TASKBAR_ICON_FILES;
    wm_set_min_size(win, 480, 320);
    EX_WIN = win;
    desktop_set_status(0);

    ex_toolbar_create(win, w - 2);
    wm_set_maximized(win, 1); /* opens filling the screen; "restore" goes back to the size above */

    EX_CUR = VFS_ROOT;
    EX_HIST_N = 0;
    EX_FWD_N = 0;
    EX_CLIP_ID = -1;
    EX_SEL = -1;
    EX_SCROLL = 0;
    EX_NOTE = 0;
    EX_LAST_ROW = -1;
    EX_HOVER_ROW = EX_HOVER_PLACE = -1;
    EX_RENAME_ID = -1;
    EX_MENU.open = 0;
    ex_reload();
    if (EX_NITEMS > 0)
        EX_SEL = 0;
}

const AppInfo APP_EXPLORER = {
    .name = "File Explorer",
    .desc = "Browse files and folders",
    .icon = UI_ICON_FILES,
    .open = explorer_app_open,
};

