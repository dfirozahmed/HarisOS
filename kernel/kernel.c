/* HarisOS kernel core: UEFI-independent entry point. */
#include "blk.h"
#include "bootinfo.h"
#include "console.h"
#include "cpuinfo.h"
#include "cpuload.h"
#include "desktop.h"
#include "devmgr.h"
#include "display.h"
#include "exec.h"
#include "exfat.h"
#include "fbwc.h"
#include "fault.h"
#include "fpu.h"
#include "fat32.h"
#include "gdt.h"
#include "acpi.h"
#include "aspace.h"
#include "gfx.h"
#include "heap.h"
#include "heapmon.h"
#include "idle.h"
#include "keyboard.h"
#include "mouse.h"
#include "net.h"
#include "ntfs.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "power.h"
#include "rtc.h"
#include "serial.h"
#include "process.h"
#include "protect.h"
#include "storage.h"
#include "syscall.h"
#include "task.h"
#include "taskbar.h"
#include "terminal.h"
#include "timer.h"
#include "vfs.h"
#include "volume.h"
#include "wm.h"

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
    fault_init(info->framebuffer, info->width, info->height, info->pitch, info->red_mask_mode); /* crash screen */
    gfx_init(info->framebuffer, info->width, info->height,
             info->pitch, info->red_mask_mode);

    power_init(info); /* shut down / restart, used by the start menu */

    acpi_init(info->acpi_rsdp); /* device tables: where the I2C touchpad is described */

    int pmm_ok = (pmm_init(info) == 0);

    /* Page tables of our own, before the allocator gives out memory that gets used: the firmware's tables can be
     * read-only, and so can the free pages around them (see paging.h). If this fails everything still runs, only
     * without guard pages. */
    int paging_ok = pmm_ok && paging_init() == 0;

    /* Double buffering: needs a contiguous block of RAM, so it comes after pmm_init.
     * If no block is found we simply keep drawing directly to the screen. */
    int double_buffered = 0;
    if (pmm_ok && gfx_ready())
    {
        unsigned long long pages = (gfx_backbuffer_size() + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        unsigned long long mem = pmm_alloc_contiguous_tag(pages, PMM_TAG_GRAPHICS);
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
        unsigned long long mem = pmm_alloc_contiguous_tag(pages, PMM_TAG_GRAPHICS);
        if (mem && gfx_cache_enable((void *)mem) != 0)
            pmm_free_contiguous(mem, pages);
    }

    /* Drag layer: two more buffers of the same size hold the picture of a window while it is moved
     * (see wm_render.c). Optional too: without them a dragged window is redrawn every frame. */
    if (double_buffered)
    {
        unsigned long long bytes = gfx_backbuffer_size();
        unsigned long long pages = (bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        unsigned long long a = pmm_alloc_contiguous_tag(pages, PMM_TAG_GRAPHICS);
        unsigned long long b = a ? pmm_alloc_contiguous_tag(pages, PMM_TAG_GRAPHICS) : 0;
        if (a && b)
            wm_set_drag_memory((void *)a, (void *)b, bytes);
        else if (a)
            pmm_free_contiguous(a, pages);
    }

    /* Video memory write-combining: after the back buffer copied the screen (reads are slow once it is set). */
    if (pmm_ok)
        fbwc_enable((unsigned long long)info->framebuffer, (unsigned long long)info->pitch * info->height * 4ULL);

    /* The kernel heap takes its first arena after the big contiguous blocks above, so it does not split their space. */
    if (pmm_ok)
        heap_init();

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
    display_init((unsigned long long)info->framebuffer, 0); /* GPUs on PCI; which one shows the framebuffer */
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
        volume_unlock_clean();    /* ordinary clean drives can be written at once; system and unclean ones stay locked */
    }

    unsigned blk_seen = blk_generation(); /* USB drives plugged in (or out) later change it: see volume_sync() */

    timer_init(); /* needed for double-click */
    idle_init();  /* 1 ms LAPIC tick so the loop can sleep (hlt) when idle; see idle.h */

    /* Kernel threads: this loop becomes task 0. The boot self test switches between test threads and checks that the
     * timer interrupt can take the CPU from one that never yields. A broken context switch or interrupt handler would
     * corrupt memory later in confusing ways, so stop now (the crash screen shows #UD). The result goes to COM1. */
    if (pmm_ok)
    {
        serial_init();

        net_init(); /* Intel e1000 on PCI, if any; the self test needs the serial port and the timer */
        net_self_test();

        /* SMEP / SMAP before anything touches user memory (the address space test below does). */
        protect_init();
        fpu_init(); /* x87 / SSE usable, and a clean state to give every new process (see fpu.h) */
        serial_puts("HARISOS: protections: SMEP ");
        serial_puts(protect_status() & PROTECT_SMEP ? "on" : "not available");
        serial_puts(", SMAP ");
        serial_puts(protect_status() & PROTECT_SMAP ? "on\n" : "not available\n");

        serial_puts("HARISOS: page tables: ");
        if (paging_ok)
        {
            serial_puts("own copy, ");
            serial_putd(paging_tables());
            serial_puts(" pages\n");
        }
        else
            serial_puts("firmware's (no guard pages)\n");
        /* User address spaces: build one, let the CPU walk it, free it. Failing here only means no user mode later,
         * so it is logged, not fatal. */
        int as_init = paging_ok ? aspace_init() : -1;
        int as_test = as_init == 0 ? aspace_selftest() : 0;
        serial_puts("HARISOS: address spaces ");
        if (as_init != 0)
            serial_puts("unavailable\n");
        else if (as_test != 0)
        {
            serial_puts("self test FAILED ");
            serial_putd(as_test);
            serial_puts("\n");
        }
        else
            serial_puts("ok\n");

        /* Do the protections work? Tries what must fault (reading / running a user page from the kernel) and sends an NMI
         * to ourselves. Logged, not fatal. */
        if (as_init == 0)
        {
            int pt = protect_selftest();
            serial_puts("HARISOS: protection self test ");
            if (pt == 0)
                serial_puts("ok\n");
            else
            {
                serial_puts("FAILED ");
                serial_putd(pt);
                serial_puts("\n");
            }
        }

        int gd = gdt_check(); /* the CPU accepts the ring 3 selectors and the TSS is loaded */
        serial_puts("HARISOS: GDT check ");
        serial_puts(gd == 0 ? "ok" : "FAILED ");
        if (gd != 0)
            serial_putd(gd);
        serial_puts("\n");
        task_init();
        int st = task_selftest();
        int sp = st == 0 ? task_selftest_preempt() : 0;
        serial_puts("HARISOS: thread self test ");
        serial_puts(st == 0 && sp == 0 ? "ok" : "FAILED");
        serial_puts(" (switch ");
        serial_putd(st);
        serial_puts(", preempt ");
        serial_putd(sp);
        serial_puts(", ticks ");
        serial_putd((long long)task_ticks());
        serial_puts(")\n");
        if (st != 0 || sp != 0)
            __builtin_trap();

        /* Ring 3: turn on SYSCALL, then run three small user programs (see process.h). Like the address space test,
         * a failure is logged, not fatal: nothing else depends on user mode yet. */
        syscall_init();
        console_set_interrupt_hook(process_interrupt_console); /* Ctrl+C in a console window ends the programs that write to it */
        int ps = (as_init == 0 && gd == 0) ? process_selftest() : 1;
        if (ps == 0)
        {
            /* A program that is a real file: HELLO.ELF on one of the drives (if the boot disk has one; the volumes were
             * mounted above). It is run, and damaged copies of it must be refused. Logged, not fatal. */
            int ex = exec_selftest();
            serial_puts("HARISOS: ELF program self test ");
            if (ex == 1)
                serial_puts("skipped (no HELLO.ELF on any drive)\n");
            else if (ex == 0)
                serial_puts("ok\n");
            else
            {
                serial_puts("FAILED ");
                serial_putd(ex);
                serial_puts("\n");
            }
        }
        serial_puts("HARISOS: user mode self test ");
        if (ps == 1)
            serial_puts("skipped\n");
        else if (ps == 0)
            serial_puts("ok\n");
        else
        {
            serial_puts("FAILED ");
            serial_putd(ps);
            serial_puts("\n");
        }
        heapmon_start(); /* background thread: kernel heap integrity check once a second (shown in Task Manager) */
    }

    {
        unsigned apic_ids[ACPI_MAX_CPUS]; /* for the Task Manager's CPU page; needs timer_init (TSC speed) */
        int ncpu = acpi_cpu_count();
        for (int i = 0; i < ncpu; i++)
            apic_ids[i] = acpi_cpu_apic_id(i);
        cpuinfo_probe(cpuinfo_hw_cpuid, apic_ids, ncpu, timer_tsc_khz());
    }

    RtcTime now;
    if (rtc_read(&now) == 0)
        taskbar_set_time(now.hour, now.minute, now.day, now.month, now.year);

    desktop_draw();
    gfx_present(); /* show the finished frame (does nothing without a back buffer) */

    /* The first user program: INIT.ELF in a Terminal window, if the boot disk has it. It keeps the shell running; closing
     * the window ends it. Opened after the first frame so it is in front of the welcome window and has the keyboard.
     * (Remove "init" from USER_PROGRAMS in CMakeLists.txt for a desktop that starts empty.) */
    if (gfx_ready() && exec_find("INIT.ELF") >= 0)
        terminal_open("INIT.ELF");

    MouseState m;
    if (have_mouse)
    {
        mouse_poll(&m);
        gfx_cursor_move(m.x, m.y);
        gfx_cursor_show(1);
    }

    /* Wheel / touchpad scrolling arrives faster than a list can be redrawn. Like a display's refresh rate, the
     * steps are collected and applied at most every WHEEL_FRAME_US: the list scrolls by the sum, and the CPU
     * is not spent on frames nobody sees. */
    enum { WHEEL_FRAME_US = 16000 };
    int wheel_pending = 0, wheel_x = 0, wheel_y = 0;
    unsigned long long wheel_last = 0;

    for (;;)
    {
        unsigned long long pass_start = timer_us();
        int worked = 0; /* this pass handled something: poll again at once instead of sleeping */
        if (have_mouse && mouse_poll(&m))
        {
            gfx_cursor_move(m.x, m.y);
            desktop_pointer(m.x, m.y, m.buttons);
            if (m.wheel)
            {
                wheel_pending += m.wheel;
                wheel_x = m.x;
                wheel_y = m.y;
            }
            worked = 1;
        }
        if (wheel_pending && pass_start - wheel_last >= WHEEL_FRAME_US)
        {
            desktop_wheel(wheel_x, wheel_y, wheel_pending);
            if (have_mouse)
                desktop_pointer(m.x, m.y, m.buttons); /* the list moved under a still pointer: update the hover highlight */
            wheel_pending = 0;
            wheel_last = pass_start;
            worked = 1;
        }
        KeyEvent ke;
        /* At most a few events per pass: a USB key release only arrives through mouse_poll() above. If handling
         * one key press (a redraw) takes longer than the 33 ms repeat rate, a loop without this limit would
         * produce repeats forever and never see the release. */
        for (int nkeys = 0; nkeys < 4 && gfx_ready() && keyboard_poll(&ke); nkeys++)
        {
            desktop_key(&ke);
            worked = 1;
        }
        if (pmm_ok && blk_generation() != blk_seen) /* a USB drive came or went (found inside mouse_poll) */
        {
            volume_sync(); /* the File Explorer notices through vfs_generation() */
            blk_seen = blk_generation();
            worked = 1;
        }
        if (gfx_ready() && desktop_tick()) /* blinking text caret, clock, app timers */
            worked = 1;
        int others = task_others_ready();
        if (others)
            task_yield(); /* preemptible kernel threads run until their slice ends or they yield (see task.h) */
        unsigned long long pass_end = timer_us();
        cpuload_add_busy(pass_end - pass_start); /* everything except the sleep below is CPU time (see cpuload.h) */
        cpuload_poll(pass_end);
        if (!worked && !others)
            idle_wait(); /* nothing to do: halt until the next 1 ms tick */
    }
}
