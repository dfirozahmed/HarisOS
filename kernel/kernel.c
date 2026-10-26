/* HarisOS kernel core: UEFI-independent entry point. */
#include "blk.h"
#include "bootinfo.h"
#include "bootsplash.h"
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
#include "ioapic.h"
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
#include "smp.h"
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

static void wifi_boot_thread(void *arg)
{
    (void)arg;
    net_wifi_boot_run();
}

#if HARIS_BOOT_SELFTEST
/* SMP stress (slow, so only with HARIS_BOOT_SELFTEST): the user test programs, each of which exits with a known code when every one of its
 * checks holds. First one after the other, then several at once, so that their threads (and the children they start) are spread over all
 * processors. A fault in the big kernel lock scheme shows up as a wrong code, a program that does not finish, or a hang. */
typedef struct
{
    const char *file;
    int ok_code;
} StressProgram;

static const StressProgram STRESS[] = {
    {"HEAPTEST.ELF", 45}, {"PROCTEST.ELF", 46}, {"PIPETEST.ELF", 48}, {"SIGTEST.ELF", 49}, {"FORKTEST.ELF", 50},
    {"IPCTEST.ELF", 51},  {"SEMTEST.ELF", 52},  {"SIGHAND.ELF", 53},  {"MUXTEST.ELF", 54},  {"STKTEST.ELF", 55},
};

static int stress_finish(int id)
{
    int waited = -1;
    for (unsigned long long end = timer_ms() + 25000; id >= 0 && waited != 0 && timer_ms() < end;)
        waited = process_wait(id);
    if (waited != 0) /* it did not finish: say what every thread is doing */
    {
        serial_puts("HARISOS: SMP: stress: threads at the timeout (id name state cpus):\n");
        for (int t = 0; t < task_slots(); t++)
            if (task_state(t) != TASK_UNUSED)
            {
                serial_putd(t);
                serial_puts(" ");
                serial_puts(task_name(t));
                serial_puts(" st=");
                serial_putd((long long)task_state(t));
                serial_puts(" cpus=");
                serial_putd(task_cpu_mask(t));
                serial_puts("\n");
            }
    }
    return waited == 0 ? task_exit_code(id) : -1000; /* read right away: the slot is free to be used again */
}

static void smp_stress_programs(void)
{
    int n = (int)(sizeof(STRESS) / sizeof(STRESS[0])), failed = 0;
    unsigned mask = 0;
    for (int i = 0; i < n; i++) /* one after the other */
    {
        int err = 0, id = exec_find(STRESS[i].file) >= 0 ? exec_run(STRESS[i].file, &err) : -1;
        if (id < 0)
        {
            serial_puts("HARISOS: SMP: stress: ");
            serial_puts(STRESS[i].file);
            serial_puts(" skipped\n");
            continue;
        }
        int code = stress_finish(id);
        mask |= task_cpu_mask(id);
        if (code != STRESS[i].ok_code)
        {
            failed++;
            serial_puts("HARISOS: SMP: stress: ");
            serial_puts(STRESS[i].file);
            serial_puts(" FAILED, exit code ");
            serial_putd(code);
            serial_puts("\n");
        }
    }
    for (int round = 0; round < 3; round++) /* then two independent ones at once */
    {
        static const int GROUP[] = {0, 9}; /* heaptest, stktest: the others start, signal and count each other's processes, so they cannot run at once */
        int ids[2];
        for (int g = 0; g < 2; g++)
        {
            int err = 0;
            ids[g] = exec_find(STRESS[GROUP[g]].file) >= 0 ? exec_run(STRESS[GROUP[g]].file, &err) : -1;
        }
        for (int g = 0; g < 2; g++)
        {
            if (ids[g] < 0)
                continue;
            int code = stress_finish(ids[g]);
            mask |= task_cpu_mask(ids[g]);
            if (code != STRESS[GROUP[g]].ok_code)
            {
                failed++;
                serial_puts("HARISOS: SMP: stress: ");
                serial_puts(STRESS[GROUP[g]].file);
                serial_puts(" FAILED in a group, exit code ");
                serial_putd(code);
                serial_puts("\n");
            }
        }
    }
    serial_puts("HARISOS: SMP: stress ");
    serial_puts(failed ? "FAILED (" : "ok (");
    serial_putd(failed);
    serial_puts(" failures), programs ran on CPUs");
    for (int c = 0; c < smp_cpu_total(); c++)
        if (mask & (1u << c))
        {
            serial_puts(" ");
            serial_putd(c);
        }
    serial_puts("\n");
}
#endif

void kernel_main(const BootInfo *info)
{
    fault_init(info->framebuffer, info->width, info->height, info->pitch, info->red_mask_mode); /* crash screen */
    gfx_init(info->framebuffer, info->width, info->height,
             info->pitch, info->red_mask_mode);
    splash_show(); /* "Welcome to HarisOS" with a progress bar until the desktop's first frame (bootsplash.h) */

    power_init(info); /* shut down / restart, used by the start menu */

    splash_progress(8, "Reading system tables");
    acpi_init(info->acpi_rsdp); /* device tables: where the I2C touchpad is described */

    splash_progress(15, "Setting up memory");
    int pmm_ok = (pmm_init(info) == 0);

    /* Page tables of our own, before the allocator gives out memory that gets used: the firmware's tables can be
     * read-only, and so can the free pages around them (see paging.h). If this fails everything still runs, only
     * without guard pages. */
    int paging_ok = pmm_ok && paging_init() == 0;

    /* Double buffering: needs a contiguous block of RAM, so it comes after pmm_init.
     * If no block is found we simply keep drawing directly to the screen. */
    splash_progress(25, "Preparing graphics");
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
    splash_progress(40, "Starting keyboard and pointer");
    int kbd_ok = (keyboard_init() == 0);

    /* mouse: if there is none, the desktop simply stays non-interactive */
    int have_mouse = gfx_ready() && mouse_init((int)info->width, (int)info->height) == 0;
    desktop_set_mouse(have_mouse);

    /* Device Manager list: PCI scan + what the input drivers found (after mouse_init). */
    splash_progress(55, "Scanning devices and disks");
    pci_scan();
    display_init((unsigned long long)info->framebuffer, 0); /* GPUs on PCI; which one shows the framebuffer */
    if (pmm_ok)
        storage_init(); /* NVMe and SATA disks, their partitions */
    devmgr_init(kbd_ok);
    vfs_init(); /* the RAM Disk for the File Explorer */
    if (pmm_ok)
    {
        splash_progress(62, "Mounting drives");
        fat_set_clock(fat_clock); /* timestamps of files made or changed on a drive */
        exfat_set_clock(fat_clock);
        ntfs_set_clock(nt_clock);
        volume_mount_all();       /* FAT32, exFAT and NTFS drives found on the disks */
        volume_unlock_clean();    /* ordinary clean drives can be written at once; system and unclean ones stay locked */
    }

    unsigned blk_seen = blk_generation(); /* USB drives plugged in (or out) later change it: see volume_sync() */

    splash_progress(68, "Starting timers");
    timer_init(); /* needed for double-click */
    idle_init();  /* 1 ms LAPIC tick so the loop can sleep (hlt) when idle; see idle.h */
    ioapic_init(); /* the I/O APICs, every line masked until a driver asks for one (the network card does, below) */

    int wifi_boot = 0; /* the Wi-Fi card still has its first scan and connection to do: a thread does it (see below) */

    /* Kernel threads: this loop becomes task 0. The boot self test switches between test threads and checks that the
     * timer interrupt can take the CPU from one that never yields. A broken context switch or interrupt handler would
     * corrupt memory later in confusing ways, so stop now (the crash screen shows #UD). The result goes to COM1. */
    if (pmm_ok)
    {
        serial_init();

        smp_bsp_init(); /* this processor's per-CPU record (GS base): before the scheduler and the system call code need it */

        splash_progress(74, "Starting the network");
        net_init(); /* Intel e1000 on PCI, if any; the self test needs the serial port and the timer */
        wifi_boot = net_wifi_boot_begin(); /* claims the Wi-Fi card for the thread started before the main loop; nothing uses it until then */
#if HARIS_BOOT_SELFTEST
        net_self_test(); /* (starts with net_configure) */
#else
        if (net_present() && net_link_up()) /* a wired card has its link now; Wi-Fi gets its address from the main loop, once connected */
            net_configure();
#endif

        /* SMEP / SMAP before anything touches user memory (the address space test below does). */
        splash_progress(82, "Checking the system");
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
        net_set_yield(task_yield); /* from now on a thread that waits for the network lets the others run (net.h) */
        if (net_present())
        {
            /* receive interrupts for the card, if its I/O APIC line can be found (the self test above proved the polled path) */
            int gsi = net_irq_init();
            serial_puts("HARISOS: network: ");
            if (gsi < 0)
                serial_puts("no receive interrupt, polling\n");
            else
            {
                serial_puts("receive interrupt on I/O APIC line ");
                serial_putd(gsi);
                serial_puts("\n");
            }
        }
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
#if HARIS_BOOT_SELFTEST /* slow (~6 s on real hardware) and only logged to serial: see CMakeLists.txt */
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
#endif
        if (ps == 0 && net_present() && exec_find("NETTEST.ELF") >= 0)
        {
            /* the socket system calls from ring 3 (its own lines go to the serial port); logged, not fatal */
            int err = 0;
            int id = exec_run("nettest.elf", &err);
            ProcessStats ns;
            serial_puts("HARISOS: socket self test ");
            /* process_wait gives a program 400 turns, enough for the others; this one waits for the network, so keep asking */
            int waited = -1;
            for (unsigned long long end = timer_ms() + 15000; id >= 0 && waited != 0 && timer_ms() < end;)
                waited = process_wait(id);
            if (id < 0 || waited != 0)
                serial_puts("could not run or did not finish in 15 s\n");
            else
            {
                process_stats(&ns);
                serial_puts(ns.last_exit == 0 ? "ok\n" : "FAILED\n");
            }
        }
        if (ps == 0 && exec_find("GSTEST.ELF") < 0)
            serial_puts("HARISOS: per-CPU GS across ring 3 skipped (no GSTEST.ELF on any drive)\n");
        else if (ps == 0)
        {
            /* A program that wipes its own GS base, then makes system calls and takes timer interrupts: the SWAPGS pairs must keep
             * the kernel's per-CPU record reachable (percpu.h). Logged, not fatal. */
            int err = 0;
            int id = exec_run("gstest.elf", &err);
            int waited = -1;
            for (unsigned long long end = timer_ms() + 10000; id >= 0 && waited != 0 && timer_ms() < end;)
                waited = process_wait(id);
            serial_puts("HARISOS: per-CPU GS across ring 3 ");
            serial_puts(id >= 0 && waited == 0 && smp_bsp_gs_check() == 0 ? "ok\n" : "FAILED\n");
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

        /* SMP: wake the other processors (smp.h). Late on purpose: they copy this processor's CR0 / CR4 / PAT, which protect_init,
         * fpu_init and the frame buffer setup have changed by now, and turn on SYSCALL themselves. Needs the local APIC (idle_init)
         * and the MADT (acpi_init). */
        int smp_up = smp_start(info->smp_code, info->smp_data);
        serial_puts("HARISOS: SMP: ");
        serial_putd(smp_cpu_total());
        serial_puts(" processor(s) listed, ");
        serial_putd(smp_up + 1);
        serial_puts(" running\n");
        for (int c = 0; c < smp_cpu_total(); c++)
        {
            serial_puts("HARISOS: SMP: CPU ");
            serial_putd(c);
            serial_puts(" (APIC id ");
            serial_putd(smp_cpu_apic_id(c));
            serial_puts(smp_cpu_online(c) ? ") online" : ") did not start");
            if (c > 0 && smp_cpu_online(c))
            {
                serial_puts(", timer ticks ");
                serial_putd((long long)smp_cpu_ticks(c));
            }
            serial_puts("\n");
        }
        int spin = smp_selftest_spinlock(); /* every processor increments one counter under one lock */
        serial_puts("HARISOS: SMP: spinlock self test ");
        if (spin == 1)
            serial_puts("skipped (no other processor)\n");
        else if (spin == 0)
            serial_puts("ok\n");
        else
        {
            serial_puts("FAILED ");
            serial_putd(spin);
            serial_puts("\n");
        }

        int alloc = smp_selftest_alloc(); /* heap and page allocator on every processor at once, without the big kernel lock */
        serial_puts("HARISOS: SMP: allocator self test ");
        if (alloc == 1)
            serial_puts("skipped (no other processor)\n");
        else if (alloc == 0)
            serial_puts("ok\n");
        else
        {
            serial_puts("FAILED ");
            serial_putd(alloc);
            serial_puts("\n");
        }

        int tlb = smp_selftest_tlb(); /* one TLB flush request, answered by every application processor */
        serial_puts("HARISOS: SMP: TLB shootdown self test ");
        if (tlb == 1)
            serial_puts("skipped (no other processor)\n");
        else if (tlb == 0)
            serial_puts("ok\n");
        else
        {
            serial_puts("FAILED ");
            serial_putd(tlb);
            serial_puts("\n");
        }

        /* User programs on the other processors: start three CPU-bound programs at once. The boot processor runs one of them, the
         * application processors pick up the others (they only run user threads, under the big kernel lock: bkl.h). task_cpu_mask says
         * where each one has been. Logged, not fatal. */
        if (smp_cpu_total() > 1 && exec_find("GSTEST.ELF") >= 0)
        {
            int ids[3];
            for (int i = 0; i < 3; i++)
            {
                int err = 0;
                ids[i] = exec_run("gstest.elf", &err);
            }
            unsigned mask = 0;
            int bad = 0;
            for (int i = 0; i < 3; i++)
            {
                int waited = -1;
                for (unsigned long long end = timer_ms() + 20000; ids[i] >= 0 && waited != 0 && timer_ms() < end;)
                    waited = process_wait(ids[i]);
                if (ids[i] < 0 || waited != 0)
                    bad = 1;
                else
                    mask |= task_cpu_mask(ids[i]);
            }
            serial_puts("HARISOS: SMP: user programs ran on CPUs ");
            for (int c = 0; c < smp_cpu_total(); c++)
                if (mask & (1u << c))
                {
                    serial_putd(c);
                    serial_puts(" ");
                }
            serial_puts(bad ? "FAILED (a program did not finish)\n" : (mask & ~1u) ? "ok\n" : "FAILED (only the boot processor)\n");
        }
#if HARIS_BOOT_SELFTEST /* slow: runs a dozen test programs while the other processors take their share of the threads */
        smp_stress_programs();
        serial_puts("HARISOS: SMP: system calls without the big kernel lock: ");
        serial_putd((long long)syscall_fast_count());
        serial_puts(" of ");
        serial_putd((long long)syscall_count());
        serial_puts("\n");
#endif
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

    /* The Wi-Fi card's first scan and connection take several seconds: a thread of its own, the desktop does not wait (rtl8821ce.c,
     * "starting in the background"). Without a thread the network stack does the same work in small steps. */
    splash_progress(95, wifi_boot ? "Starting the desktop (Wi-Fi connects in the background)" : "Starting the desktop");
    if (wifi_boot && task_create_flags("WIFIBOOT", wifi_boot_thread, 0, TASK_PREEMPTIBLE) < 0)
        net_wifi_boot_abort();
    splash_progress(100, "Welcome");

    for (int c = 1; c < smp_cpu_total(); c++) /* seconds after smp_start: the application processors must still be ticking */
    {
        serial_puts("HARISOS: SMP: CPU ");
        serial_putd(c);
        serial_puts(" timer ticks now ");
        serial_putd((long long)smp_cpu_ticks(c));
        serial_puts("\n");
    }

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
    enum { NET_POLL_US = 10000 };
    unsigned long long net_last_poll = 0;
    unsigned long long net_cfg_next = 0; /* not before this time (ms): the next try to get an address */

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
        /* The network keeps running while nobody waits for it: ACKs, retransmit timers, ping. With a receive interrupt the card says when
         * frames wait, so the loop looks only then (and wakes at once from hlt); without one it looks every pass. A look every
         * NET_POLL_US is made either way: TCP timers need it, and a missed interrupt then costs a few milliseconds, not a frame. */
        if (net_present() && (!net_irq_active() || net_irq_take() || pass_start - net_last_poll >= NET_POLL_US))
        {
            net_last_poll = pass_start;
            if (net_poll() > 0)
                worked = 1; /* frames arrived: look again at once, more may be behind them */
        }
        /* The Wi-Fi card connects in the background after boot (see "starting in the background" in rtl8821ce.c): once the link is up
         * and there is no address yet, ask for one. net_configure() falls back to a fixed address, so this runs once per boot. */
        if (pmm_ok && net_present() && net_ip_source() == 0 && net_link_up() && timer_ms() >= net_cfg_next)
        {
            net_cfg_next = timer_ms() + 30000;
            net_configure();
            worked = 1;
        }
        if (gfx_ready() && desktop_tick()) /* blinking text caret, clock, app timers */
            worked = 1;
        int others = task_others_ready();
        unsigned long long yielded_us = 0; /* time the other threads had the processor during this pass */
        if (others)
        {
            unsigned long long y0 = timer_us();
            task_yield(); /* preemptible kernel threads run until their slice ends or they yield (see task.h) */
            yielded_us = timer_us() - y0;
        }
        unsigned long long pass_end = timer_us();
        /* everything except the sleep below is CPU time (see cpuload.h); what other threads used while we yielded is not the main loop's: the
         * timer ticks count it for the boot processor (task.c), and cpuload_boot_percent adds the two */
        cpuload_add_busy(pass_end - pass_start - yielded_us);
        cpuload_poll(pass_end);
        for (int c = 0; c < smp_cpu_total(); c++) /* every processor: its busy ticks (see cpuload.h) */
        {
            unsigned long long busy, all;
            task_cpu_ticks(c, &busy, &all);
            cpuload_cpu_poll(c, busy, all);
        }
        if (!worked && !others)
            idle_wait(); /* nothing to do: halt until the next 1 ms tick */
    }
}
