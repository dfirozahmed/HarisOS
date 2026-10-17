/* ============================================================
 * HarisOS window manager
 *
 * ============================================================ */
#ifndef HARIS_WM_H
#define HARIS_WM_H

#include "ui.h"

#define WM_MAX_WINDOWS 8
#define WM_MAX_BUTTONS 4 /* per window */
#define WM_TITLE_MAX 32

struct Window;
/* Draw the window's content. `body` is the content area (already clipped). */
typedef void (*WmDrawFn)(struct Window *w, UiRect body);

typedef struct Window
{
    int used;
    int id;        /* unique, increasing: 1, 2, 3 ... */
    int minimized; /* hidden, but still open (shown in the taskbar) */
    int icon;      /* taskbar icon (a TaskbarIcon value, see taskbar.h) */
    UiRect r;      /* outer rectangle on screen */
    char title[WM_TITLE_MAX];
    WmDrawFn on_draw;                   /* may be NULL */
    void (*on_close)(struct Window *w); /* called just before the window is closed (may be NULL) */
    void *user;                         /* your own data */

    UiButton btn[WM_MAX_BUTTONS];
    UiRect btn_rel[WM_MAX_BUTTONS]; /* position inside the content area */
    int btn_count;
    UiButtonSet set;
} Window;

/* NULL when all WM_MAX_WINDOWS slots are in use. The new window is on top and focused. */
Window *wm_create(const char *title, int x, int y, int w, int h, WmDrawFn draw, void *user);
void wm_close(Window *w);
void wm_raise(Window *w);

/* Add a button; rect is relative to the content area. -1 = window is full. */
int wm_add_button(Window *w, const char *label, UiButtonKind kind, UiRect rel,
                  UiButtonFn on_click, void *user);

UiRect wm_content(const Window *w); /* the content area, in screen coordinates */
int wm_count(void);                 /* open windows */
Window *wm_top(void);               /* focused window (topmost visible one), or NULL */

/* Minimize / restore. wm_activate() restores a minimized window and brings it to the front. */
void wm_minimize(Window *w);
void wm_activate(Window *w);
int wm_is_open(const Window *w);
/* Open windows (visible or minimized) in the order they were created, for the taskbar.
 * Returns how many were stored in out[] (at most max). */
int wm_list(Window **out, int max);
int wm_dragging(void); /* 1 while a window is being dragged */
/* Mark a screen rectangle as needing a redraw (e.g. the taskbar). */
void wm_add_damage(UiRect r);

/* Mark the whole screen as needing a redraw (e.g. after a theme change). */
void wm_invalidate(void);

/* The screen area that changed since the last call (windows moved, closed,
 * raised, button hover...). Returns 0 if nothing changed. After a change,
 * redraw only that rectangle: restore the wallpaper there, then wm_draw()
 * with the clip set to it. */
int wm_take_damage(UiRect *out);

/* Draw every window, back to front. Draw the wallpaper first. */
void wm_draw(void);

/* Feed the mouse state. Returns 1 when something changed (then call wm_take_damage()). */
int wm_pointer(int x, int y, unsigned buttons);

#endif
