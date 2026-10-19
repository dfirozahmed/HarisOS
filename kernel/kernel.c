/* HarisOS kernel core: UEFI-independent entry point. */
#include "bootinfo.h"
#include "desktop.h"
#include "gfx.h"
#include "keyboard.h"
#include "mouse.h"
#include "pmm.h"
#include "power.h"
#include "rtc.h"
#include "taskbar.h"
#include "timer.h"

void kernel_main(const BootInfo *info)
{
    gfx_init(info->framebuffer, info->width, info->height,
             info->pitch, info->red_mask_mode);

    power_init(info); /* shut down / restart, used by the start menu */

    int pmm_ok = (pmm_init(info) == 0);

    /* Double buffering: needs a contiguous block of RAM, so it comes after pmm_init.
     * If no block is found we simply keep drawing directly to the screen. */
    int double_buffered = 0;
    if (pmm_ok && gfx_ready())
    {
        unsigned long long pages = (gfx_backbuffer_size() + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        unsigned long long mem = pmm_alloc_contiguous(pages);
        if (mem && gfx_enable_backbuffer((void *)mem) == 0)
            double_buffered = 1;
        else if (mem)
            pmm_free_contiguous(mem, pages);
    }

    /* Wallpaper cache: a second buffer of the same size. With it, moving a window only
     * redraws the small area that changed. Optional: without it everything still works. */
    if (double_buffered)
    {
        unsigned long long pages = (gfx_backbuffer_size() + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        unsigned long long mem = pmm_alloc_contiguous(pages);
        if (mem && gfx_cache_enable((void *)mem) != 0)
            pmm_free_contiguous(mem, pages);
    }

    /* Tell the desktop what to show (after the allocation above, so the free-memory figure includes it). */
    if (pmm_ok)
        desktop_set_memory(pmm_free_pages() * PMM_PAGE_SIZE / (1024 * 1024),
                           pmm_total_pages() * PMM_PAGE_SIZE / (1024 * 1024));
    else
        desktop_set_memory(0, 0);
    desktop_set_double_buffered(double_buffered);

    /* keyboard: initialised before the mouse; if it does not answer we still poll it */
    keyboard_init();

    /* mouse: if there is none, the desktop simply stays non-interactive */
    int have_mouse = gfx_ready() && mouse_init((int)info->width, (int)info->height) == 0;
    desktop_set_mouse(have_mouse);

    timer_init(); /* needed for double-click */

    RtcTime now;
    if (rtc_read(&now) == 0)
        taskbar_set_time(now.hour, now.minute, now.day, now.month, now.year);

    desktop_draw();
    gfx_present(); /* show the finished frame (does nothing without a back buffer) */

    MouseState m;
    if (have_mouse)
    {
        mouse_poll(&m);
        gfx_cursor_move(m.x, m.y);
        gfx_cursor_show(1);
    }

    for (;;)
    {
        if (have_mouse && mouse_poll(&m))
        {
            gfx_cursor_move(m.x, m.y);
            desktop_pointer(m.x, m.y, m.buttons);
        }
        KeyEvent ke;
        while (gfx_ready() && keyboard_poll(&ke))
            desktop_key(&ke);
        if (gfx_ready())
            desktop_tick(); /* blinking text caret */
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause");
#endif
    }
}
