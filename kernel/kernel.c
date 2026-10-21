/* HarisOS kernel core: UEFI-independent entry point. */
#include "bootinfo.h"
#include "desktop.h"
#include "devmgr.h"
#include "exfat.h"
#include "fat32.h"
#include "acpi.h"
#include "gfx.h"
#include "keyboard.h"
#include "mouse.h"
#include "ntfs.h"
#include "pci.h"
#include "pmm.h"
#include "power.h"
#include "rtc.h"
#include "storage.h"
#include "taskbar.h"
#include "timer.h"
#include "vfs.h"
#include "volume.h"

/* the clock for FAT timestamps: the CMOS clock, in FAT date / time format */
static void fat_clock(unsigned *date, unsigned *time)
{
    RtcTime t;
    if (rtc_read(&t) == 0)
    {
        *date = fat_make_date((unsigned)t.year, (unsigned)t.month, (unsigned)t.day);
        *time = fat_make_time((unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second);
    }
    else
    {
        *date = fat_make_date(1980, 1, 1);
        *time = 0;
    }
}

/* the clock for NTFS timestamps: the CMOS clock as a FILETIME (100 ns ticks since 1601-01-01) */
static unsigned long long nt_clock(void)
{
    RtcTime t;
    if (rtc_read(&t) != 0)
        return 133801632000000000ULL; /* 2025-01-01: no clock, but a believable date */
    long long y = t.year - (t.month <= 2);
    long long era = y / 400, yoe = y - era * 400;
    long long doy = (153 * (t.month + (t.month > 2 ? -3 : 9)) + 2) / 5 + t.day - 1;
    long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468; /* days since 1970-01-01 */
    long long secs = days * 86400 + t.hour * 3600LL + t.minute * 60LL + t.second;
    return (unsigned long long)(secs + 11644473600LL) * 10000000ULL;
}

void kernel_main(const BootInfo *info)
{
    gfx_init(info->framebuffer, info->width, info->height,
             info->pitch, info->red_mask_mode);

    power_init(info); /* shut down / restart, used by the start menu */

    acpi_init(info->acpi_rsdp); /* device tables: where the I2C touchpad is described */

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
    int kbd_ok = (keyboard_init() == 0);

    /* mouse: if there is none, the desktop simply stays non-interactive */
    int have_mouse = gfx_ready() && mouse_init((int)info->width, (int)info->height) == 0;
    desktop_set_mouse(have_mouse);

    /* Device Manager list: PCI scan + what the input drivers found (after mouse_init). */
    pci_scan();
    if (pmm_ok)
        storage_init(); /* NVMe and SATA disks, their partitions */
    devmgr_init(kbd_ok);
    vfs_init(); /* the RAM Disk for the File Explorer */
    if (pmm_ok)
    {
        fat_set_clock(fat_clock); /* timestamps of files made or changed on a drive */
        exfat_set_clock(fat_clock);
        ntfs_set_clock(nt_clock);
        volume_mount_all();       /* FAT32, exFAT and NTFS drives found on the disks */
    }

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
            if (m.wheel)
                desktop_wheel(m.x, m.y, m.wheel);
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
