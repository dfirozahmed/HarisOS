/* HarisOS desktop. Everything here is built from HarisUI (include/ui.h), so
 * restyling the whole desktop is a theme change: ui_set_theme(&ui_theme_light). */
#include "desktop.h"
#include "taskbar.h"
#include "ui.h"
#include "timer.h"
#include "wm.h"
#include "rtc.h"
#include "desktop_internal.h"

const char *STATUS_LINE;
unsigned long long MEM_FREE_MB, MEM_TOTAL_MB;
int DOUBLE_BUFFERED;
int MOUSE_PRESENT;

Window *WELCOME; /* the welcome window (NULL once the user closes it) */
int DARK = 1;
int WALL_VALID; /* 1 = the wallpaper is in gfx's cache and matches the current theme */

void desktop_set_status(const char *line) { STATUS_LINE = line; }

void desktop_set_memory(unsigned long long free_mb, unsigned long long total_mb)
{
    MEM_FREE_MB = free_mb;
    MEM_TOTAL_MB = total_mb;
}

void desktop_set_double_buffered(int on) { DOUBLE_BUFFERED = on; }
void desktop_set_mouse(int present) { MOUSE_PRESENT = present; }

char *desktop_put_str(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;
    return o;
}

char *desktop_put_num(char *o, unsigned long long v)
{
    char tmp[24];
    int n = 0;
    do
    {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *o++ = tmp[--n];
    return o;
}

/* ---- taskbar: one button per open window ---------------------------- */
static Window *TB_WIN[TASKBAR_MAX_APPS];
static int TB_N;
static unsigned TB_SIG[TASKBAR_MAX_APPS]; /* what the taskbar shows now, to spot changes */

/* Rebuild the taskbar's app list from the open windows.
 * Returns 1 if what the taskbar shows has changed (it must be redrawn). */
static int refresh_taskbar(void)
{
    Window *list[TASKBAR_MAX_APPS];
    TaskbarApp apps[TASKBAR_MAX_APPS];
    unsigned sig[TASKBAR_MAX_APPS];
    int n = wm_list(list, TASKBAR_MAX_APPS);
    Window *top = wm_top();
    if (top && top->dialog)
        top = top->owner; /* a dialog counts as its owner */
    for (int i = 0; i < n; i++)
    {
        apps[i].icon = (TaskbarIcon)list[i]->icon;
        apps[i].running = 1;
        apps[i].active = (list[i] == top);
        apps[i].minimized = list[i]->minimized;
        sig[i] = (unsigned)list[i]->id * 8u + (unsigned)apps[i].icon + (apps[i].active ? 64u : 0u) +
                 (apps[i].minimized ? 128u : 0u);
    }
    int changed = (n != TB_N);
    for (int i = 0; i < n && !changed; i++)
        if (sig[i] != TB_SIG[i])
            changed = 1;
    for (int i = 0; i < n; i++)
    {
        TB_WIN[i] = list[i];
        TB_SIG[i] = sig[i];
    }
    TB_N = n;
    taskbar_set_apps(apps, n);
    return changed;
}

/* Start button: open the welcome window again (or bring it to the front). */
static void on_taskbar_start(void)
{
    if (!WELCOME)
        welcome_create();
    else
        wm_activate(WELCOME);
}

/* App button: restore a minimized window, minimize the focused one, or focus another. */
static void on_taskbar_app(int i)
{
    if (i < 0 || i >= TB_N || !wm_is_open(TB_WIN[i]))
        return;
    Window *w = TB_WIN[i];
    if (w->minimized)
        wm_activate(w);
    else if (w == wm_top())
        wm_minimize(w);
    else
        wm_raise(w);
}

static int FIRST = 1;

/* Full redraw: wallpaper, windows, taskbar. The finished wallpaper is saved in
 * gfx's cache (if there is one) so later redraws can be partial. */
void desktop_draw(void)
{
    if (!gfx_ready())
        return;
    if (FIRST)
    {
        FIRST = 0;
        taskbar_set_callbacks(on_taskbar_start, on_taskbar_app);
        welcome_create();
    }
    gfx_reset_clip();
    ui_background();
    gfx_cache_save(); /* does nothing without a cache */
    WALL_VALID = gfx_cache_valid();
    wm_draw();
    gfx_reset_clip();
    refresh_taskbar();
    taskbar_draw();
}

/* Partial redraw of one rectangle: put the saved wallpaper back, then draw the
 * windows and taskbar on top, all clipped to that rectangle. */
static void desktop_draw_region(UiRect r)
{
    gfx_set_clip(r.x, r.y, r.w, r.h);
    gfx_cache_restore(r.x, r.y, r.w, r.h);
    wm_draw();
    refresh_taskbar();
    taskbar_draw();
    gfx_reset_clip();
}

/* After any input: refresh the taskbar, then redraw whatever changed. Returns 1 if something was drawn. */
static int finish_input(int bar_changed)
{
    int bar_dirty = refresh_taskbar(); /* a window may have opened/closed/changed focus */
    bar_dirty |= bar_changed;
    if (bar_dirty)
    {
        int bx, by, bw, bh;
        taskbar_bounds(&bx, &by, &bw, &bh);
        wm_add_damage(ui_rect(bx, by, bw, bh));
    }

    UiRect area;
    if (!wm_take_damage(&area))
        return 0;

    /* With a back buffer the scene is drawn in RAM and gfx_present() handles the
     * pointer, so it stays visible the whole time (no flicker). Without one we
     * draw straight to the screen and must hide the pointer meanwhile. */
    int direct = !gfx_double_buffered();
    if (direct)
        gfx_cursor_show(0);
    if (WALL_VALID && gfx_cache_valid())
        desktop_draw_region(area); /* fast: only what changed */
    else
        desktop_draw(); /* first frame, new theme, or no cache */
    gfx_present();
    if (direct)
        gfx_cursor_show(1);
    return 1;
}

int desktop_pointer(int x, int y, unsigned buttons)
{
    int H = (int)gfx_height();
    int on_bar = y >= H - TASKBAR_HEIGHT;

    /* The taskbar is on top of the windows: while the pointer is over it (and no
     * window is being dragged) the windows must not see the mouse. */
    int to_windows = !(on_bar && !wm_dragging());
    wm_pointer(to_windows ? x : -1, to_windows ? y : -1, buttons);

    /* The taskbar only sees the mouse when no window is being dragged. */
    int bar_changed = refresh_taskbar(); /* its buttons must match the windows before a click is handled */
    bar_changed |= taskbar_pointer(wm_dragging() ? -1 : x, wm_dragging() ? -1 : y, buttons);
    return finish_input(bar_changed);
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
    return finish_input(0);
}

/* Refresh the CMOS clock periodically. The displayed clock has minute
 * precision, so reading RTC once per second is enough; redraw only when a
 * displayed field changes. A loop-count fallback keeps clock refresh working
 * on machines where PIT/TSC calibration is unavailable. */
static int refresh_taskbar_clock(void)
{
    static unsigned long long next_poll_ms;
    static unsigned fallback_polls;
    static int clock_seen;
    static int last_hour = -1, last_minute = -1;
    static int last_day = -1, last_month = -1, last_year = -1;

    if (timer_ready())
    {
        unsigned long long now = timer_ms();
        if (now < next_poll_ms)
            return 0;
        next_poll_ms = now + 1000;
    }
    else
    {
        /* Avoid CMOS port I/O on every tight-loop iteration. */
        if (++fallback_polls < 65536u)
            return 0;
        fallback_polls = 0;
    }

    RtcTime now;
    if (rtc_read(&now) != 0)
        return 0; /* keep the last valid time if the RTC is unavailable */

    int changed = !clock_seen || now.hour != last_hour || now.minute != last_minute ||
                  now.day != last_day || now.month != last_month || now.year != last_year;
    clock_seen = 1;
    last_hour = now.hour;
    last_minute = now.minute;
    last_day = now.day;
    last_month = now.month;
    last_year = now.year;
    if (changed)
        taskbar_set_time(now.hour, now.minute, now.day, now.month, now.year);
    return changed;
}

/* Called on every main-loop pass: updates the taskbar clock and makes the text
 * caret blink. Only the taskbar or small text field is redrawn when needed. */
int desktop_tick(void)
{
    int clock_changed = refresh_taskbar_clock();
    int flipped = ui_caret_update();
    Window *top = wm_top();

    if (clock_changed)
    {
        int bx, by, bw, bh;
        taskbar_bounds(&bx, &by, &bw, &bh);
        wm_add_damage(ui_rect(bx, by, bw, bh));
    }
    if (flipped && top && top->caret && top->edit_count > 0)
    {
        UiRect f = top->edits[top->focus].field;
        if (f.w > 0)
            wm_add_damage(f);
    }
    if (!clock_changed && !flipped)
        return 0;
    return finish_input(clock_changed);
}
