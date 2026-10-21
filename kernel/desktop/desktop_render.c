/* Partial redraw and damage-region presentation. */
#include "desktop.h"
#include "taskbar.h"
#include "startmenu.h"
#include "appmenu.h"
#include "ui.h"
#include "wm.h"
#include "timer.h"
#include "desktop_internal.h"

extern int WALL_VALID;

/* TEMPORARY PROFILING: average time per frame of each redraw step, drawn at the top-left. Remove after use. */
static unsigned long long P_RESTORE, P_WM, P_BAR, P_PRESENT, P_FRAMES, P_PIX;
static unsigned long long P_SHOW[7];
static unsigned long long P_T0;

static void prof_show(void)
{
    char s[96], *p = s;
    p = desktop_put_str(p, "fps ");
    p = desktop_put_num(p, P_SHOW[0]);
    p = desktop_put_str(p, " restore ");
    p = desktop_put_num(p, P_SHOW[1]);
    p = desktop_put_str(p, " wm ");
    p = desktop_put_num(p, P_SHOW[2]);
    p = desktop_put_str(p, " bar ");
    p = desktop_put_num(p, P_SHOW[3]);
    p = desktop_put_str(p, " present ");
    p = desktop_put_num(p, P_SHOW[4]);
    p = desktop_put_str(p, " us; px ");
    p = desktop_put_num(p, P_SHOW[5]);
    *p = 0;
    gfx_fill_rect(0, 0, 640, 22, gfx_rgb(0, 0, 0));
    ui_text(6, 2, s, UI_TEXT_SMALL);
    gfx_present();
}

/* Partial redraw of one rectangle: put the saved wallpaper back, then draw the
 * windows and taskbar on top, all clipped to that rectangle. */
static void desktop_draw_region(UiRect r)
{
    unsigned long long t0 = timer_us();
    gfx_set_clip(r.x, r.y, r.w, r.h);
    gfx_cache_restore(r.x, r.y, r.w, r.h);
    unsigned long long t1 = timer_us();
    wm_draw();
    unsigned long long t2 = timer_us();
    desktop_refresh_taskbar();
    startmenu_draw();
    appmenu_draw();
    taskbar_draw();
    gfx_reset_clip();
    unsigned long long t3 = timer_us();
    P_RESTORE += t1 - t0;
    P_WM += t2 - t1;
    P_BAR += t3 - t2;
    P_PIX += (unsigned long long)r.w * r.h;
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

    UiRect areas[WM_DAMAGE_MAX];
    int n = wm_take_damage_rects(areas, WM_DAMAGE_MAX);
    if (n == 0)
        return 0;

    /* With a back buffer the scene is drawn in RAM and gfx_present() handles the
     * pointer, so it stays visible the whole time (no flicker). Without one we
     * draw straight to the screen and must hide the pointer meanwhile. */
    int direct = !gfx_double_buffered();
    if (direct)
        gfx_cursor_show(0);
    if (WALL_VALID && gfx_cache_valid())
    {
        /* fast: only what changed. Each area is presented on its own: gfx_present() copies the box around
         * what was drawn since the last call, so one call after several areas would copy the gaps too. */
        for (int i = 0; i < n; i++)
        {
            desktop_draw_region(areas[i]);
            unsigned long long t = timer_us();
            gfx_present();
            P_PRESENT += timer_us() - t;
        }
    }
    else
    {
        desktop_draw(); /* first frame, new theme, or no cache */
        gfx_present();
    }
    if (direct)
        gfx_cursor_show(1);

    P_FRAMES++;
    unsigned long long now = timer_us();
    if (P_T0 == 0)
        P_T0 = now;
    if (now - P_T0 >= 1000000ULL)
    {
        unsigned long long f = P_FRAMES ? P_FRAMES : 1;
        P_SHOW[0] = P_FRAMES;
        P_SHOW[1] = P_RESTORE / f;
        P_SHOW[2] = P_WM / f;
        P_SHOW[3] = P_BAR / f;
        P_SHOW[4] = P_PRESENT / f;
        P_SHOW[5] = P_PIX / f;
        P_RESTORE = P_WM = P_BAR = P_PRESENT = P_FRAMES = P_PIX = 0;
        P_T0 = now;
    }
    prof_show();
    return 1;
}
