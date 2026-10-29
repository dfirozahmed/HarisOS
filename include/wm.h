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

#define WM_WINDOW_CAP 4096 /* windows are made as needed (see wm.c); this only stops a program that opens them in a loop */
#define WM_MAX_BUTTONS 14 /* per window */
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
    int (*on_close_request)(struct Window *w);           /* the USER asked to close it (close button, Alt+F4, the taskbar menu): return 1 to keep it open, e.g. while it asks about unsaved work, then call wm_close() later; 0 or NULL = close. Not called by wm_close() itself (may be NULL) */
    void (*on_key)(struct Window *w, const KeyEvent *k); /* key press / release while focused (may be NULL) */
    int (*on_press)(struct Window *w, int x, int y);     /* left press inside the window below the title bar; return 1 if it changed what is shown (the whole window is redrawn), or 2 if only an area it has already passed to wm_add_damage() changed (may be NULL) */
    int (*on_drag)(struct Window *w, int x, int y, unsigned buttons); /* called while the left button stays down after an on_press (and once when it is released, buttons & 1 == 0); return 1 if it changed what is shown (the whole window is redrawn), or 2 if only an area it has already passed to wm_add_damage() changed (may be NULL) */
    int (*on_wheel)(struct Window *w, int steps);        /* mouse wheel over the window (steps: + = down, like MouseState.wheel); return 1 if it changed what is shown (may be NULL) */
    int (*on_rpress)(struct Window *w, int x, int y);    /* right button went down inside the window below the title bar (the window is raised first); return 1 if it changed what is shown (the whole window is redrawn), or 2 if it changed only an area it has already passed to wm_add_damage() (may be NULL) */
    int (*on_tick)(struct Window *w);                    /* called on every main-loop pass for open, visible windows (see wm_tick); return 1 if it changed what is shown, e.g. a tooltip that is due (the whole window is redrawn), or 2 if it changed only an area it has already passed to wm_add_damage() (may be NULL) */
    int (*on_hover)(struct Window *w, int x, int y);     /* the pointer moved over the window (x, y on screen, may be on the title bar); called once with x = y = -1 when it leaves. Return 1 only if the look changed (the whole window is redrawn), or 2 if only an area it has already passed to wm_add_damage() changed (may be NULL) */
    int (*on_cursor)(struct Window *w, int x, int y);    /* which pointer shape to show at (x, y) over the window: a GFX_CURSOR_* value (GFX_CURSOR_ARROW = the normal arrow). Also asked while a press that started in the window is held, wherever the pointer is (may be NULL) */
    UiEdit edits[WM_MAX_EDITS];                         /* ready-made text inputs for the window to use (start empty) */
    int edit_count;                                      /* how many of them the window uses (0 = none) */
    int focus;                                           /* which input has the keyboard (0 .. edit_count-1) */
    int caret;                                           /* 1 = the window shows a blinking caret in the focused input */
    void *user;                                          /* your own data */

    UiButton btn[WM_MAX_BUTTONS];
    UiRect btn_rel[WM_MAX_BUTTONS]; /* position inside the content area (at the creation size) */
    int btn_anchor[WM_MAX_BUTTONS]; /* WM_ANCHOR_* flags */
    int btn_count;
    /* CPU time of the app (Task Manager): the microseconds spent in its own callbacks (drawing, input, timers), counted by the window manager
     * in wm_charge; cpu_prev_us and cpu_pct are the Task Manager's sample (tenths of a percent of the capacity of all processors, 0..1000). */
    unsigned long long cpu_us;
    unsigned long long cpu_prev_us;
    int cpu_pct;

    UiButtonSet set;
} Window;

/* NULL when the heap is out of memory or WM_WINDOW_CAP windows are open. The new window is on top and focused. */
Window *wm_create(const char *title, int x, int y, int w, int h, WmDrawFn draw, void *user);
/* Like wm_create, but a modal dialog (message box) of `owner`: while it is open the owner
 * cannot be used (clicking the owner brings the dialog to the front); every other window
 * keeps working normally. The dialog is not in the taskbar and closes with its owner. */
Window *wm_create_dialog(Window *owner, const char *title, int x, int y, int w, int h, WmDrawFn draw, void *user);
void wm_close(Window *w);
/* Close because the user asked: first asks the window (on_close_request). Returns 1 if it was closed, 0 if the
 * window wanted to stay open. wm_close() closes without asking. */
int wm_request_close(Window *w);
/* Before shutting the computer down: asks every window (its on_close_request) whether it may be closed, without
 * closing any. The first one that wants to stay open (it is asking the user about unsaved work) is brought to the
 * front and 0 is returned: do not shut down yet. 1 = nobody objected. */
int wm_confirm_close_all(void);
void wm_raise(Window *w);

/* Add a button; rect is relative to the content area. -1 = window is full. */
int wm_add_button(Window *w, const char *label, UiButtonKind kind, UiRect rel,
                  UiButtonFn on_click, void *user);

/* Put an icon before the label of button `index` (0 = the first one added). */
void wm_set_button_icon(Window *w, int index, UiIcon icon);

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

/* The pointer shape the window under (x, y) asks for with its on_cursor (GFX_CURSOR_ARROW if none). */
int wm_cursor_hint(int x, int y);

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
void wm_add_screen_damage(UiRect r); /* the same for what is not inside a window (taskbar, clock, menus): with the compositor no window is drawn again */

/* Compositor (gfx.h "compositor"): each visible window in a surface of its own. wm_comp_set(1) starts tracking what
 * each window shows; wm_comp_update() draws again the surfaces of the windows that changed (a and b: two drawing
 * buffers of bytes each), 0 = done, -1 = it cannot (a window too big, no surface): draw without the compositor.
 * wm_comp_items() lists the windows back to front, wm_comp_drop() frees every surface and stops. */
void wm_comp_set(int on);
int wm_comp_on(void);
int wm_comp_update(const GfxCompOps *ops, void *a, void *b, unsigned long long bytes);
int wm_comp_items(GfxCompItem *out, int max);
void wm_comp_drop(const GfxCompOps *ops);
int wm_drag_memory(void **a, void **b, unsigned long long *bytes); /* the two buffers of wm_set_drag_memory(); 0 = none */

/* Call often (every main-loop pass): lets windows react to time passing (on_tick). Returns 1 when some
 * window changed, then call wm_take_damage() and redraw. */
int wm_tick(void);

/* Mark the whole screen as needing a redraw (e.g. after a theme change). */
void wm_invalidate(void);

/* The screen area that changed since the last call (windows moved, closed,
 * raised, button hover...). Returns 0 if nothing changed. After a change,
 * redraw only that rectangle: restore the wallpaper there, then wm_draw()
 * with the clip set to it. */
int wm_take_damage(UiRect *out);

/* Same, but as separate non-overlapping rectangles (at most max, never more than WM_DAMAGE_MAX), so a
 * window dragged far leaves two small areas to redraw instead of one big box. Returns how many. */
#define WM_DAMAGE_MAX 8
int wm_take_damage_rects(UiRect *out, int max);

/* Two buffers of `bytes` each (about the screen size) for the picture of a window being dragged.
 * Optional: without them a dragged window is redrawn from scratch every frame. */
void wm_set_drag_memory(void *a, void *b, unsigned long long bytes);

/* Draw every window, back to front. Draw the wallpaper first. */
void wm_draw(void);

/* Send a key event to the focused window (its on_key). Returns 1 if a window got it;
 * that window is then marked for redraw. */
int wm_key(const KeyEvent *k);

/* Call from inside on_key, after wm_add_damage() for the areas that changed: only those areas are redrawn,
 * instead of the whole window (typing in a big text area). Applies to the current key event only. */
void wm_key_partial(void);

/* Feed the mouse state. Returns 1 when something changed (then call wm_take_damage()). */
int wm_pointer(int x, int y, unsigned buttons);

/* Mouse wheel turned while the pointer is at (x, y): goes to the window under the pointer
 * (its on_wheel), without focusing or raising it. Ignored while a window is being dragged, and
 * for a window blocked by its dialog. Returns 1 when something changed (then call wm_take_damage()). */
int wm_wheel(int x, int y, int steps);

#endif
