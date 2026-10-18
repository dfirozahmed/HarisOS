/* Private shared declarations for the window-manager modules. */
#ifndef HARIS_WM_INTERNAL_H
#define HARIS_WM_INTERNAL_H

#include "wm.h"
#include "gfx.h"

#define WM_SHADOW_MARGIN 48

extern Window *WM_Z[WM_MAX_WINDOWS];
extern int WM_ZN;
extern int WM_HAS_DMG, WM_DMG_X0, WM_DMG_Y0, WM_DMG_X1, WM_DMG_Y1;

void wm_damage_internal(int x0, int y0, int x1, int y1);
void wm_damage_window_internal(const Window *w);
int wm_top_index_internal(void);
int wm_z_index_internal(const Window *w);
Window *wm_blocker_internal(const Window *w);
Window *wm_window_at_internal(int x, int y);
void wm_input_forget_window(Window *w);
void wm_layout_internal(Window *w);

#endif
