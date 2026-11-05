/* HarisOS desktop menu: right-click the empty desktop (no window under the pointer) to get a small menu with
 * Personalize, Display settings and Refresh. A HarisUI UiMenu (ui.h) drawn above the windows and below the taskbar.
 * Mouse: hover, press and release on an item runs it; a click away closes it (and still goes through).
 * Keyboard: Up / Down / Home / End, Enter, Esc. Only one of the start, app, volume and desktop menus is open at a time. */
#ifndef HARIS_DESKMENU_H
#define HARIS_DESKMENU_H

#include "keyboard.h"
#include "ui.h"

int deskmenu_is_open(void);
/* Open the menu with its corner at (x, y), moved to stay on the screen above the taskbar. */
void deskmenu_open(int x, int y);
/* The same for a right click on desktop icon `icon` (deskicons.h): "Open" comes first. */
void deskmenu_open_icon(int x, int y, int icon);
void deskmenu_close(void);

/* Same contract as appmenu_pointer(): returns 1 while the pointer is over the menu. */
int deskmenu_pointer(int x, int y, unsigned buttons);
int deskmenu_key(const KeyEvent *k);
/* Every main-loop pass: the hover highlight fades. 1 = draw again. */
int deskmenu_tick(unsigned long long now_ms);
/* Every main-loop pass: the hover highlight fades. 1 = draw again. */
int deskmenu_tick(unsigned long long now_ms);
void deskmenu_draw(void);     /* after the windows and the other menus, before the taskbar */
UiRect deskmenu_bounds(void); /* everything the open menu draws, its shadow included */

#endif
