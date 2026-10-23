/* HarisOS taskbar: start button, search, app buttons on the left; system tray
 * and clock on the right. Built from HarisUI (ui.h), so it follows the theme. */
#ifndef HARIS_TASKBAR_H
#define HARIS_TASKBAR_H

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
    TASKBAR_ICON_NOTEPAD
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

/* Which app button is at (x, y): its index, or -1. taskbar_app_rect() gives its rectangle
 * (valid after the taskbar was drawn once); returns 0 if there is no such app. */
int taskbar_app_at(int x, int y);
int taskbar_app_rect(int index, int *x, int *y, int *w, int *h);

/* Where the taskbar is on screen (for partial redraws). */
void taskbar_bounds(int *x, int *y, int *w, int *h);

void taskbar_set_time(int hour, int minute, int day, int month, int year);

/* Draw the taskbar along the bottom of the screen. */
void taskbar_draw(void);

#endif
