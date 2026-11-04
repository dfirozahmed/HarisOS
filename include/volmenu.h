/* HarisOS volume popup: click the speaker in the taskbar tray to get a small light translucent panel above it with a mute button, a
 * slider and the percentage. Same look as the start menu and the app menu. Mouse: click or drag the slider, click the speaker tile to
 * mute / unmute, click elsewhere closes it. Keyboard: Left / Down quieter, Right / Up louder (5 points), M mute, Esc / Enter close.
 * The volume itself lives in the sound driver (audio.h). */
#ifndef HARIS_VOLMENU_H
#define HARIS_VOLMENU_H

#include "keyboard.h"
#include "ui.h"

int volmenu_is_open(void);
/* Open the panel centered above x (pixels; the middle of the tray speaker). Does nothing without sound hardware. */
void volmenu_open(int anchor_x);
void volmenu_close(void);

/* Same contract as startmenu_pointer(): returns 1 while the pointer is over the panel (or a slider drag is going on). */
int volmenu_pointer(int x, int y, unsigned buttons);
int volmenu_key(const KeyEvent *k);
/* The volume was changed from elsewhere (keys, wheel, a program): redraw the panel if it shows another value. Also steps the opening slide.
 * Call on every main-loop pass; returns 1 when something needs drawing again. */
int volmenu_tick(void);
void volmenu_draw(void);     /* after the windows, before the taskbar */
UiRect volmenu_bounds(void); /* everything the open panel draws, its shadow included */

#endif
