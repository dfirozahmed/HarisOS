/* HarisOS taskbar: start button, search, app buttons on the left; system tray
 * and clock on the right. Built from HarisUI (ui.h), so it follows the theme. */
#ifndef HARIS_TASKBAR_H
#define HARIS_TASKBAR_H

#include "ui.h"

#define TASKBAR_HEIGHT 52

typedef enum
{
    TASKBAR_ICON_FILES,
    TASKBAR_ICON_TERMINAL,
    TASKBAR_ICON_SETTINGS,
    TASKBAR_ICON_BROWSER,
    TASKBAR_ICON_LOGO,
    TASKBAR_ICON_TASKMGR,
    TASKBAR_ICON_DISPLAY,
    TASKBAR_ICON_DEVMGR,
    TASKBAR_ICON_NOTEPAD,
    TASKBAR_ICON_WIFI
} TaskbarIcon;

typedef struct
{
    TaskbarIcon icon;
    int running; /* shows a small dot under the icon */
    int active;  /* focused app: highlighted, wide indicator */
    int minimized; /* hidden: the icon is dimmed */
} TaskbarApp;

#define TASKBAR_MAX_APPS 32 /* the most app buttons ever kept; how many are drawn depends on the screen width (they shrink to fit) */

/* The app buttons to show (copied; at most TASKBAR_MAX_APPS). */
void taskbar_set_apps(const TaskbarApp *apps, int count);

/* Click events. start = the "Haris" button, app(i) = the i-th app button. */
typedef void (*TaskbarStartFn)(void);
typedef void (*TaskbarAppFn)(int index);
void taskbar_set_callbacks(TaskbarStartFn on_start, TaskbarAppFn on_app);

/* Mouse input. Returns 1 if a button's hover/press look changed, so the taskbar
 * should be redrawn. Click callbacks run inside. Pass x = -1 when the pointer
 * must be ignored (e.g. while a window is being dragged). */
int taskbar_pointer(int x, int y, unsigned buttons);
/* Every main-loop pass: moves the buttons' hover tint one step. 1 = draw again, *dirty = the area. */
int taskbar_tick(unsigned long long now_ms, UiRect *dirty);

/* Which app button is at (x, y): its index, or -1. taskbar_app_rect() gives its rectangle
 * (valid after the taskbar was drawn once); returns 0 if there is no such app. */
int taskbar_app_at(int x, int y);
int taskbar_app_rect(int index, int *x, int *y, int *w, int *h);

/* Where the taskbar is on screen (for partial redraws). */
void taskbar_bounds(int *x, int *y, int *w, int *h);

/* The master volume shown under the tray's speaker icon: a small level bar (percent 0..100; -1 = no sound hardware: no bar). Returns 1 if it
 * changed, so the taskbar must be redrawn. taskbar_volume_at() says whether (x, y) is on the speaker (valid after the taskbar was drawn):
 * the mouse wheel over it changes the volume (desktop_input.c). */
int taskbar_set_volume(int percent);
int taskbar_volume_at(int x, int y);
int taskbar_volume_center(void);

/* The Wi-Fi state shown by the tray icon: -1 no Wi-Fi card (dim, slashed), 0 not connected (dim, slashed), 1 connected (normal), 2 connection lost,
 * connecting again (warning colour). Returns 1 if it changed. taskbar_wifi_at() says whether (x, y) is on the icon (valid after the taskbar was drawn):
 * a click on it opens the Wi-Fi app (desktop_input.c). */
int taskbar_set_wifi(int state);
int taskbar_wifi_at(int x, int y);
int taskbar_wifi_state(void); /* the state last set (-1 .. 2) */

/* The battery shown by the tray icon: percent 0..100 and whether it is charging; percent -1 = unknown (the plain icon, no number). `ac` = the mains
 * cable is connected: a small plug is shown beside the battery (also when the battery is full and does not charge, or when there is none). Returns 1
 * if anything changed, so the taskbar must be redrawn. */
int taskbar_set_battery(int percent, int charging, int ac); /* x of the middle of the speaker (the volume popup opens above it) */

void taskbar_set_time(int hour, int minute, int day, int month, int year);

/* Draw the taskbar along the bottom of the screen. */
void taskbar_draw(void);

#endif
