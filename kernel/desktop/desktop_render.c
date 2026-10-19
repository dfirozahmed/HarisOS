/* Partial redraw and damage-region presentation. */
#include "desktop.h"
#include "taskbar.h"
#include "startmenu.h"
#include "appmenu.h"
#include "ui.h"
#include "wm.h"
#include "desktop_internal.h"

extern int WALL_VALID;

/* Partial redraw of one rectangle: put the saved wallpaper back, then draw the
 * windows and taskbar on top, all clipped to that rectangle. */
static void desktop_draw_region(UiRect r)
{
    gfx_set_clip(r.x, r.y, r.w, r.h);
    gfx_cache_restore(r.x, r.y, r.w, r.h);
    wm_draw();
    desktop_refresh_taskbar();
    startmenu_draw();
    appmenu_draw();
    taskbar_draw();
    gfx_reset_clip();
}

/* After any input: refresh the taskbar, then redraw whatever changed. Returns 1 if something was drawn. */
int desktop_finish_input(int bar_changed)
{
    int bar_dirty = desktop_refresh_taskbar(); /* a window may have opened/closed/changed focus */
    bar_dirty |= bar_changed;
    if (bar_dirty)
    {
        int bx, by, bw, bh;
        taskbar_bounds(&bx, &by, &bw, &bh);
        wm_add_damage(ui_rect(bx, by, bw, bh));
    }

    UiRect area;
    if (!wm_take_damage(&area))
        return 0;

    /* With a back buffer the scene is drawn in RAM and gfx_present() handles the
     * pointer, so it stays visible the whole time (no flicker). Without one we
     * draw straight to the screen and must hide the pointer meanwhile. */
    int direct = !gfx_double_buffered();
    if (direct)
        gfx_cursor_show(0);
    if (WALL_VALID && gfx_cache_valid())
        desktop_draw_region(area); /* fast: only what changed */
    else
        desktop_draw(); /* first frame, new theme, or no cache */
    gfx_present();
    if (direct)
        gfx_cursor_show(1);
    return 1;
}
