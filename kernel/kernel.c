/* HarisOS kernel core: UEFI-independent entry point. */
#include "bootinfo.h"
#include "desktop.h"
#include "gfx.h"
#include "pmm.h"
#include "rtc.h"
#include "taskbar.h"

static char *put_str(char *out, const char *s)
{
    while (*s)
        *out++ = *s++;
    return out;
}

static char *put_num(char *out, unsigned long long v)
{
    char tmp[24];
    int n = 0;
    do
    {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *out++ = tmp[--n];
    return out;
}

void kernel_main(const BootInfo *info)
{
    static char status[160];

    gfx_init(info->framebuffer, info->width, info->height,
             info->pitch, info->red_mask_mode);

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

    /* Status text (after the allocation above, so the free-memory figure includes it). */
    char *p = status;
    if (pmm_ok)
    {
        p = put_str(p, "MEMORY ");
        p = put_num(p, pmm_free_pages() * PMM_PAGE_SIZE / (1024 * 1024));
        p = put_str(p, " MB FREE OF ");
        p = put_num(p, pmm_total_pages() * PMM_PAGE_SIZE / (1024 * 1024));
        p = put_str(p, " MB");
    }
    else
    {
        p = put_str(p, "MEMORY MANAGER FAILED");
    }
    p = put_str(p, double_buffered ? "\nGRAPHICS DOUBLE BUFFERED" : "\nGRAPHICS DIRECT");
    *p = 0;
    desktop_set_status(status);

    RtcTime now;
    if (rtc_read(&now) == 0)
        taskbar_set_time(now.hour, now.minute, now.day, now.month, now.year);

    desktop_draw();
    gfx_present(); /* show the finished frame (does nothing without a back buffer) */

    for (;;)
    {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("hlt");
#endif
    }
}
