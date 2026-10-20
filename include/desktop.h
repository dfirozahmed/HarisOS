/* HarisOS desktop: wallpaper + welcome window + taskbar, drawn with HarisUI (ui.h).
 * Call gfx_init() first. */
#ifndef HARIS_DESKTOP_H
#define HARIS_DESKTOP_H

#include "keyboard.h"

/* System info shown in the welcome window. */
void desktop_set_memory(unsigned long long free_mb, unsigned long long total_mb); /* total 0 = failed */
void desktop_set_double_buffered(int on);
void desktop_set_status(const char *line); /* optional extra line at the bottom (may be NULL) */
void desktop_set_mouse(int present); /* shows a "Mouse" badge */

void desktop_draw(void);

/* Mouse input. Call whenever the pointer moved or a button changed.
 * Hover and press effects are redrawn here (only the small area that changed).
 * Returns 1 if something was drawn, so the caller should gfx_present(). */
int desktop_pointer(int x, int y, unsigned buttons);

/* Mouse wheel (steps: + = down). Goes to the window under the pointer; ignored while a menu is
 * open or the pointer is on the taskbar. Returns 1 if something was drawn. */
int desktop_wheel(int x, int y, int steps);

int desktop_key(const KeyEvent *k);

/* Call on every main-loop pass (cheap): makes the text caret blink.
 * Returns 1 if something was drawn. */
int desktop_tick(void);

#endif
