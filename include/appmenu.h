/* HarisOS taskbar app menu: right-click an app button in the taskbar to get a small light
 * translucent menu above it with Minimize / Restore, Maximize / Restore size and Close window.
 * Same look as the start menu. Mouse: hover, click; click elsewhere closes it.
 * Keyboard: Up/Down, Enter, Esc. */
#ifndef HARIS_APPMENU_H
#define HARIS_APPMENU_H

#include "keyboard.h"
#include "wm.h"

int appmenu_is_open(void);
/* Open the menu for window w, centered above x (pixels; usually the center of its taskbar button). */
void appmenu_open(Window *w, int anchor_x);
void appmenu_close(void);

/* Same contract as startmenu_pointer(): returns 1 while the pointer is over the menu. */
int appmenu_pointer(int x, int y, unsigned buttons);
int appmenu_key(const KeyEvent *k);
void appmenu_draw(void); /* after the windows, before the taskbar */

#endif
