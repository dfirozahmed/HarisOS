/* Desktop keyboard and pointer event routing. */
#include "desktop.h"
#include "taskbar.h"
#include "startmenu.h"
#include "appmenu.h"
#include "mouse.h"
#include "ui.h"
#include "wm.h"
#include "desktop_internal.h"

static unsigned LAST_BTN;

int desktop_pointer(int x, int y, unsigned buttons)
{
    int H = (int)gfx_height();
    int on_bar = y >= H - TASKBAR_HEIGHT;

    /* The taskbar is on top of the windows: while the pointer is over it (and no
     * window is being dragged) the windows must not see the mouse. */
    int to_windows = !(on_bar && !wm_dragging());
    /* The start menu is above the windows: while the pointer is over it the windows must not see the mouse. */
    if (startmenu_pointer(wm_dragging() ? -1 : x, y, buttons) && !wm_dragging())
        to_windows = 0;
    if (appmenu_pointer(wm_dragging() ? -1 : x, y, buttons) && !wm_dragging())
        to_windows = 0;

    /* right click on a running app's taskbar button: its menu (Minimize / Maximize / Close) */
    int right_edge = (buttons & MOUSE_RIGHT) && !(LAST_BTN & MOUSE_RIGHT);
    LAST_BTN = buttons;
    if (right_edge && on_bar && !wm_dragging())
    {
        int idx = taskbar_app_at(x, y);
        Window *w = desktop_taskbar_window(idx);
        int bx, by, bw, bh;
        if (w && taskbar_app_rect(idx, &bx, &by, &bw, &bh))
            appmenu_open(w, bx + bw / 2);
    }
    wm_pointer(to_windows ? x : -1, to_windows ? y : -1, buttons);

    /* pointer shape: a double arrow on a window edge or corner (and during a resize) */
    int edge = (to_windows || wm_dragging()) ? wm_resize_hint(x, y) : 0;
    int shape = GFX_CURSOR_ARROW;
    if ((edge & (WM_EDGE_LEFT | WM_EDGE_RIGHT)) && (edge & (WM_EDGE_TOP | WM_EDGE_BOTTOM)))
        shape = ((edge & WM_EDGE_LEFT) != 0) == ((edge & WM_EDGE_TOP) != 0) ? GFX_CURSOR_SIZE_NWSE : GFX_CURSOR_SIZE_NESW;
    else if (edge & (WM_EDGE_LEFT | WM_EDGE_RIGHT))
        shape = GFX_CURSOR_SIZE_H;
    else if (edge)
        shape = GFX_CURSOR_SIZE_V;
    gfx_cursor_set_shape(shape);

    /* The taskbar only sees the mouse when no window is being dragged. */
    int bar_changed = desktop_refresh_taskbar(); /* its buttons must match the windows before a click is handled */
    bar_changed |= taskbar_pointer(wm_dragging() ? -1 : x, wm_dragging() ? -1 : y, buttons);
    return desktop_finish_input(bar_changed);
}

/* Alt+Tab: focus the next window (a minimized one is restored). shift = go backwards. */
static void cycle_windows(int backwards)
{
    Window *list[TASKBAR_MAX_APPS];
    int n = wm_list(list, TASKBAR_MAX_APPS);
    if (n == 0)
        return;
    Window *top = wm_top();
    if (top && top->dialog)
        top = top->owner;
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (list[i] == top)
            cur = i;
    int next = cur < 0 ? 0 : (backwards ? (cur + n - 1) % n : (cur + 1) % n);
    wm_activate(list[next]);
}

static int SUPER_DOWN; /* the Windows key repeats while held: toggle once per press */

int desktop_key(const KeyEvent *k)
{
    ui_caret_restart(); /* typing: the caret is shown and its blink starts over */
    if (k->key == KEY_SUPER && !k->pressed)
        SUPER_DOWN = 0;
    if (k->pressed)
    {
        if (k->key == KEY_SUPER || ((k->mods & KMOD_CTRL) && k->key == KEY_ESC))
        {
            if (k->key != KEY_SUPER || !SUPER_DOWN)
                startmenu_toggle();
            if (k->key == KEY_SUPER)
                SUPER_DOWN = 1;
        }
        else if ((k->mods & KMOD_ALT) && k->key == KEY_F4)
            wm_close(wm_top()); /* Alt+F4: close the focused window */
        else if ((k->mods & KMOD_ALT) && k->key == KEY_TAB)
            cycle_windows((k->mods & KMOD_SHIFT) != 0);
        else if (k->key == KEY_F11)
        {
            Window *w = wm_top(); /* F11: maximize / restore */
            if (w)
                wm_set_maximized(w, !w->maximized);
        }
        else if (appmenu_is_open())
            appmenu_key(k);
        else if (startmenu_is_open())
            startmenu_key(k); /* the open menu has the keyboard */
        else
            wm_key(k);
    }
    else if (!startmenu_is_open() && !appmenu_is_open())
        wm_key(k);
    return desktop_finish_input(0);
}

