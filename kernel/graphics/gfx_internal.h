/* Internal shared state for HarisOS graphics modules.
 * Not a public application API; include/gfx.h remains the public interface.
 */
#ifndef HARIS_GFX_INTERNAL_H
#define HARIS_GFX_INTERNAL_H

#include "gfx.h"

extern volatile unsigned int *DRAW;
extern int W, H;
extern int CX0, CY0, CX1, CY1;

/* Clips [x0,x1) x [y0,y1) to the active clip rectangle.
 * Returns non-zero only when a drawable area remains.
 */
int gfx_internal_clip_box(int *x0, int *y0, int *x1, int *y1);

#endif
