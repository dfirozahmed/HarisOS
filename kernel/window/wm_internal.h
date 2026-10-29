/* Private shared declarations for the window-manager modules. */
#ifndef HARIS_WM_INTERNAL_H
#define HARIS_WM_INTERNAL_H

#include "wm.h"
#include "gfx.h"

#define WM_SHADOW_MARGIN 48

extern Window **WM_Z;
extern int WM_ZN;
extern int WM_HAS_DMG;

Window *wm_moving_window_internal(void); /* the window being dragged by its title bar once it has really moved, or 0 */
void wm_damage_internal(int x0, int y0, int x1, int y1);
void wm_damage_window_internal(const Window *w);
void wm_damage_content_internal(const Window *w, int why); /* the same, and what the window shows changed (its surface); why: WM_WHY_* */
void wm_comp_why_internal(int why); /* count one request (WM_WHY_*) */
/* compositor (wm_comp.c): what a window shows changed in a screen rectangle; in every window under one; in all of them */
void wm_comp_content_internal(const Window *w, int x0, int y0, int x1, int y1);
void wm_comp_content_screen_internal(int x0, int y0, int x1, int y1);
void wm_comp_all_internal(void);

/* CPU accounting per window (Window.cpu_us): run a handler of the window and add the time it took. `call` is an expression; its value is the
 * value of the macro. A handler may close its window; the slot is only cleared, never freed meanwhile (wm_tick reclaims chunks). */
unsigned long long wm_now_us(void);
void wm_charge(Window *w, unsigned long long start_us);
/* While a handler of a window runs, WM_CALLING is that window: what it marks with wm_add_damage() is its own content (the
 * compositor draws only that window there, not the ones under it). */
extern Window *WM_CALLING;
#define WM_TIMED(w, call)                                                                                                        \
    ({                                                                                                                           \
        Window *cw_ = WM_CALLING;                                                                                                \
        WM_CALLING = (w);                                                                                                        \
        unsigned long long t0_ = wm_now_us();                                                                                    \
        __typeof__(call) r_ = (call);                                                                                            \
        wm_charge((w), t0_);                                                                                                     \
        WM_CALLING = cw_;                                                                                                        \
        r_;                                                                                                                      \
    })
int wm_top_index_internal(void);
int wm_z_index_internal(const Window *w);
Window *wm_blocker_internal(const Window *w);
Window *wm_window_at_internal(int x, int y);
void wm_input_forget_window(Window *w);
void wm_layout_internal(Window *w);
void wm_draw_window_internal(Window *w, int i, int top); /* frame, content, buttons and grip at w->r; i = its place in WM_Z */
void wm_reclaim_internal(void); /* main loop only: gives back unused window memory */
/* window-button state for drawing: 0 close, 1 minimize, 2 maximize, -1 none */
int wm_titlebtn_hover(const Window *w);   /* the pointer is on this button */
int wm_titlebtn_pressed(const Window *w); /* the button is held down */

#endif
