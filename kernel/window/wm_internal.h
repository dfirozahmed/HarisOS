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
void wm_layout_internal(Window *w);

#endif
