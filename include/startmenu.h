/* HarisOS start menu: a light, translucent panel above the taskbar, opened by the
 * "Haris" button (or the Windows key / Ctrl+Esc). Built from HarisUI (ui.h).
 *
 * It has a search box (type to filter), a list of apps and a footer with "Close all".
 * Mouse: hover, click; click outside or press Esc to close.
 * Keyboard: type to search, Up/Down to choose, Enter to open, Esc to close. */
#ifndef HARIS_STARTMENU_H
#define HARIS_STARTMENU_H

#include "keyboard.h"
#include "ui.h"

/* The light glass theme the menus are drawn with (also used by the taskbar app menu). */
const UiTheme *startmenu_theme(void);

/* Draw the glass panel itself (soft shadow outside, translucent light fill, bright edge). Used by both menus. */
void startmenu_glass(UiRect r, int radius);

int startmenu_is_open(void);
void startmenu_toggle(void);
void startmenu_close(void);

/* Mouse state. Returns 1 when the pointer is over the menu (the windows below must not see it).
 * Pass x = -1 to ignore the pointer (e.g. while a window is dragged). Redraw damage is added itself. */
int startmenu_pointer(int x, int y, unsigned buttons);

/* A key press while the menu is open. Returns 1 if the menu used it (always, while open). */
int startmenu_key(const KeyEvent *k);

/* Call when the text caret blinked, so the search box is redrawn. */
void startmenu_tick(int caret_flipped);

/* Draw the menu (does nothing when closed). Call after the windows, before the taskbar. */
void startmenu_draw(void);

#endif
