/* HarisOS dirty-region tracking.
 * Keeps the bounding rectangle of modified pixels while drawing to a backbuffer.
 */
#include "gfx_internal.h"

int DX0, DY0, DX1, DY1;

void gfx_internal_dirty_reset(void)
{
    DX0 = DY0 = DX1 = DY1 = 0;
}

/* Remember which part of the backbuffer changed, so gfx_present() copies only it. */
void gfx_internal_mark_dirty(int x0, int y0, int x1, int y1)
{
    if (!BACK || x1 <= x0 || y1 <= y0)
        return;

    if (DX1 <= DX0 || DY1 <= DY0)
    {
        DX0 = x0;
        DY0 = y0;
        DX1 = x1;
        DY1 = y1;
        return;
    }

    if (x0 < DX0)
        DX0 = x0;
    if (y0 < DY0)
        DY0 = y0;
    if (x1 > DX1)
        DX1 = x1;
    if (y1 > DY1)
        DY1 = y1;
}
