/* HarisOS desktop. Everything here is built from HarisUI (include/ui.h), so
 * restyling the whole desktop is a theme change: ui_set_theme(&ui_theme_light). */
#include "desktop.h"
#include "taskbar.h"
#include "startmenu.h"
#include "appmenu.h"
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
int desktop_refresh_taskbar(void)
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

/* The window behind the i-th taskbar button (NULL if there is none). */
Window *desktop_taskbar_window(int i)
{
    return (i >= 0 && i < TB_N && wm_is_open(TB_WIN[i])) ? TB_WIN[i] : 0;
}

/* Start button: show / hide the start menu. */
static void on_taskbar_start(void)
{
    startmenu_toggle();
}

/* App button: restore a minimized window, minimize the focused one, or focus another. */
static void on_taskbar_app(int i)
{
    startmenu_close();
    appmenu_close();
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
    desktop_refresh_taskbar();
    startmenu_draw();
    appmenu_draw();
    taskbar_draw();
}
