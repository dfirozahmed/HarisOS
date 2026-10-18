/* Internal shared state for HarisOS graphics modules.
 * Not a public application API; include/gfx.h remains the public interface.
 */
#ifndef HARIS_GFX_INTERNAL_H
#define HARIS_GFX_INTERNAL_H

#include "gfx.h"

/* Shared graphics state, private to kernel/graphics. */
extern volatile unsigned int *FB;
extern volatile unsigned int *DRAW;
extern int W, H, PITCH, DPITCH;
extern int BACK;
extern volatile unsigned int *CACHE;
extern int CACHE_OK;
extern int DX0, DY0, DX1, DY1;
extern int CUR_DRAWN, CUR_Y_SHARED;
extern int RED_FIRST;
void gfx_internal_buffer_init(void *framebuffer, int width, int height, int pitch);
void gfx_internal_mark_dirty(int x0, int y0, int x1, int y1);
void gfx_internal_cursor_restore(void);
void gfx_internal_cursor_paint(void);

extern int CX0, CY0, CX1, CY1;

/* Clips [x0,x1) x [y0,y1) to the active clip rectangle.
 * Returns non-zero only when a drawable area remains.
 */
int gfx_internal_clip_box(int *x0, int *y0, int *x1, int *y1);

#endif
