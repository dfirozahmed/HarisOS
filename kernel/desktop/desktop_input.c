/* Desktop keyboard and pointer event routing. */
#include "desktop.h"
#include "taskbar.h"
#include "ui.h"
#include "wm.h"
#include "desktop_internal.h"

int desktop_pointer(int x, int y, unsigned buttons)
{
    int H = (int)gfx_height();
    int on_bar = y >= H - TASKBAR_HEIGHT;

    /* The taskbar is on top of the windows: while the pointer is over it (and no
     * window is being dragged) the windows must not see the mouse. */
    int to_windows = !(on_bar && !wm_dragging());
    wm_pointer(to_windows ? x : -1, to_windows ? y : -1, buttons);

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

int desktop_key(const KeyEvent *k)
{
    ui_caret_restart(); /* typing: the caret is shown and its blink starts over */
    if (k->pressed)
    {
        if ((k->mods & KMOD_ALT) && k->key == KEY_F4)
            wm_close(wm_top()); /* Alt+F4: close the focused window */
        else if ((k->mods & KMOD_ALT) && k->key == KEY_TAB)
            cycle_windows((k->mods & KMOD_SHIFT) != 0);
        else if (k->key == KEY_F11)
        {
            Window *w = wm_top(); /* F11: maximize / restore */
            if (w)
                wm_set_maximized(w, !w->maximized);
        }
        else
            wm_key(k);
    }
    else
        wm_key(k);
    return desktop_finish_input(0);
}

