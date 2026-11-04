/* HarisOS drag and drop of files from a window to the desktop.
 *
 * An app that lets the user drag files out of its window (the File Explorer) uses it from its on_drag handler:
 *
 *   dnd_begin(ids, n, 0);       the pointer moved far enough with the button held: these entries (vfs nodes) are being dragged
 *   dnd_move(x, y);             on every pointer move while the button is held
 *   dnd_drop(x, y);             when the button is let go: what is under the pointer decides (dnd_cancel() = never mind)
 *
 * While a drag goes on a small card with the name (or "3 items") follows the pointer, above the windows. When the button is let go on the
 * bare desktop (no window there, not on the taskbar) a copy of each entry goes into the Desktop folder (deskfiles_send_many); let go
 * over any window it does nothing. The desktop draws the card (dnd_draw, in the draw paths and as a layer of the GPU compositor). */
#ifndef HARIS_DND_H
#define HARIS_DND_H

#include "ui.h"

#define DND_MAX 256 /* entries one drag carries */

/* Start dragging `n` entries (vfs nodes, copied). label = the text on the card, or NULL: the name of one entry, "N items" for several. */
void dnd_begin(const int *nodes, int n, const char *label);
int dnd_active(void);
void dnd_move(int x, int y);       /* the pointer is at (x, y) */
int dnd_drop(int x, int y);        /* the button was let go at (x, y); 1 = the entries were sent to the desktop */
void dnd_cancel(void);             /* the drag ends, nothing happens */

/* for the desktop */
UiRect dnd_bounds(void);           /* everything the card draws, its shadow included */
void dnd_draw(void);               /* after the windows, with the menus and the dragged desktop icon */

#endif
