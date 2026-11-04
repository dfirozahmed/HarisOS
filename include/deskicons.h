/* HarisOS desktop icons: a shortcut for every app of the registry (app.h), on the wallpaper below the windows.
 *
 *   Click    selects an icon, a click on the bare desktop selects none.
 *   Double click (or the menu's Open) opens the app.
 *   Drag     moves an icon; it snaps to the nearest cell of the grid on release. If that cell has an icon, the two swap places.
 *   Where each icon is stays in ICONS.CFG, next to THEME.CFG (one line per app: "File Explorer=0,1" = column 0, row 1), read by
 *   deskicons_load() at boot and written after every move. "Arrange icons" in the desktop menu puts them back in the default order.
 *
 * The desktop (desktop_input.c) feeds the pointer in, draws the icons after the wallpaper and before the windows
 * (desktop_draw, desktop_draw_region), and, with the GPU compositor, keeps them in a layer of their own (desktop_comp.c). */
#ifndef HARIS_DESKICONS_H
#define HARIS_DESKICONS_H

#include "ui.h"

void deskicons_load(void); /* at boot, after the drives are mounted: the places of ICONS.CFG (none: the default order) */

/* The pointer, on every mouse event. on_desktop = it is on the bare desktop (no window, menu or taskbar under it, no window being
 * dragged): only then a click starts something. Returns 1 while the icons own the event (a press on an icon, or a drag going on),
 * so the windows must not see it. */
int deskicons_pointer(int x, int y, unsigned buttons, int on_desktop);

int deskicons_at(int x, int y); /* the icon under (x, y), -1 = none */
void deskicons_select(int icon); /* -1 = none */
void deskicons_open(int icon);   /* open its app */
void deskicons_arrange(void);    /* back to the default order (and saved) */

void deskicons_draw(void);        /* clipped to the current clip */
int deskicons_visible(void);      /* there are icons to draw */
UiRect deskicons_bounds(void);    /* everything they draw: the grid cells in use */

/* An icon that is being dragged is not in the grid layer: it follows the pointer above the windows (and the outline of the cell it
 * would land in), as a layer of its own that is there only during the drag. */
int deskicons_dragging(void);
UiRect deskicons_ghost_bounds(void); /* the whole desktop above the taskbar */
void deskicons_ghost_draw(void);

#endif
