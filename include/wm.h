/* ============================================================
 * HarisOS window manager (small, no malloc)
 *
 * Keeps a fixed pool of windows with a z-order (back to front), and handles
 * focus (click raises), dragging by the title bar, resizing by the edges and corners,
 * the red close button, and
 * routing mouse events to the buttons of the window under the pointer.
 *
 *   Window *w = wm_create("Hello", 100, 80, 360, 200, my_draw, my_data);
 *   wm_add_button(w, "Close", UI_BTN_DANGER, ui_rect(240, 140, 100, 34), on_close, w);
 *
 *   wm_draw();                              // draws all windows, back to front
 *   if (wm_pointer(x, y, buttons))          // on every mouse update
 *       { redraw the desktop; wm_draw(); }
 *
 * Button rectangles are relative to the window's content area, so they follow
 * the window when it is dragged.
 * ============================================================ */
#ifndef HARIS_WM_H
#define HARIS_WM_H

#include "ui.h"

#define WM_MAX_WINDOWS 8
#define WM_MAX_BUTTONS 4 /* per window */
#define WM_TITLE_MAX 32
#define WM_MAX_EDITS 3 /* text inputs per window */

struct Window;
/* Draw the window's content. `body` is the content area (already clipped). */
typedef void (*WmDrawFn)(struct Window *w, UiRect body);

typedef struct Window
{
    int used;
    int id;               /* unique, increasing: 1, 2, 3 ... */
    int dialog;           /* 1 = modal dialog: only a close button, not in the taskbar */
    struct Window *owner; /* a dialog blocks only this window (its parent) */
    int minimized;        /* hidden, but still open (shown in the taskbar) */
    int maximized;        /* fills the screen above the taskbar */
    UiRect normal;        /* size and place to go back to when un-maximized */
    int base_w, base_h;   /* outer size at creation: reference for button anchors */
    int fixed_size;       /* 1 = cannot be resized with the mouse (dialogs never can) */
    int min_w, min_h;     /* smallest outer size when resizing (set by wm_create; see wm_set_min_size) */
    int icon;             /* taskbar icon (a TaskbarIcon value, see taskbar.h) */
    UiRect r;             /* outer rectangle on screen */
    char title[WM_TITLE_MAX];
    WmDrawFn on_draw;                                    /* may be NULL */
    void (*on_close)(struct Window *w);                  /* called just before the window is closed (may be NULL) */
    void (*on_key)(struct Window *w, const KeyEvent *k); /* key press / release while focused (may be NULL) */
    int (*on_press)(struct Window *w, int x, int y);     /* left press inside the window below the title bar; return 1 if it changed what is shown (may be NULL) */
    int (*on_drag)(struct Window *w, int x, int y, unsigned buttons); /* called while the left button stays down after an on_press (and once when it is released, buttons & 1 == 0); return 1 if it changed what is shown (may be NULL) */
    UiEdit edits[WM_MAX_EDITS];                          /* ready-made text inputs for the window to use (start empty) */
    int edit_count;                                      /* how many of them the window uses (0 = none) */
    int focus;                                           /* which input has the keyboard (0 .. edit_count-1) */
    int caret;                                           /* 1 = the window shows a blinking caret in the focused input */
    void *user;                                          /* your own data */

    UiButton btn[WM_MAX_BUTTONS];
    UiRect btn_rel[WM_MAX_BUTTONS]; /* position inside the content area (at the creation size) */
    int btn_anchor[WM_MAX_BUTTONS]; /* WM_ANCHOR_* flags */
    int btn_count;
    UiButtonSet set;
} Window;

/* NULL when all WM_MAX_WINDOWS slots are in use. The new window is on top and focused. */
Window *wm_create(const char *title, int x, int y, int w, int h, WmDrawFn draw, void *user);
/* Like wm_create, but a modal dialog (message box) of `owner`: while it is open the owner
 * cannot be used (clicking the owner brings the dialog to the front); every other window
 * keeps working normally. The dialog is not in the taskbar and closes with its owner. */
Window *wm_create_dialog(Window *owner, const char *title, int x, int y, int w, int h, WmDrawFn draw, void *user);
void wm_close(Window *w);
void wm_raise(Window *w);

/* Add a button; rect is relative to the content area. -1 = window is full. */
int wm_add_button(Window *w, const char *label, UiButtonKind kind, UiRect rel,
                  UiButtonFn on_click, void *user);

/* Same, with an anchor. When the window grows (maximize), a button anchored to
 * the right / bottom keeps its distance from that edge instead of the left / top. */
#define WM_ANCHOR_RIGHT 1
#define WM_ANCHOR_BOTTOM 2
int wm_add_button_ex(Window *w, const char *label, UiButtonKind kind, UiRect rel, int anchor,
                     UiButtonFn on_click, void *user);

/* Resizing: drag a window's edge or corner. Windows can be resized by default (not dialogs, not
 * maximized ones). The smallest size defaults to about 3/4 of the creation size. */
void wm_set_resizable(Window *w, int on);
void wm_set_min_size(Window *w, int min_w, int min_h);

/* Which resize edge the point (x, y) is on: 0 = none, otherwise a mix of WM_EDGE_* (for the
 * pointer shape). While a resize is going on it keeps returning that edge. */
#define WM_EDGE_LEFT 1
#define WM_EDGE_RIGHT 2
#define WM_EDGE_TOP 4
#define WM_EDGE_BOTTOM 8
int wm_resize_hint(int x, int y);

/* Maximize: fill the screen above the taskbar; un-maximize: go back to the old size. */
void wm_set_maximized(Window *w, int on);

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
int wm_dragging(void); /* 1 while a window is being dragged or resized */
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

/* Send a key event to the focused window (its on_key). Returns 1 if a window got it;
 * that window is then marked for redraw. */
int wm_key(const KeyEvent *k);

/* Feed the mouse state. Returns 1 when something changed (then call wm_take_damage()). */
int wm_pointer(int x, int y, unsigned buttons);

#endif
