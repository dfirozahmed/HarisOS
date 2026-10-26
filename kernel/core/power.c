/* HarisOS power control. See include/power.h. */
#include "power.h"
#include "io.h"

static void (*FW_RESET)(unsigned type); /* the firmware's reset service, or NULL */

void power_init(const BootInfo *info)
{
    FW_RESET = info ? info->reset_system : 0;
}

#if defined(__x86_64__) || defined(__i386__)
#ifdef HARIS_HOST_TEST /* the unit test runs as a normal program: no privileged instructions */
static void no_interrupts(void) {}
#else
#include "fault.h"
/* Interrupts off, and the other processors stopped (fault.h): they must not keep running user threads, or writing to the disk, while the
 * firmware or the chipset takes the machine down. */
static void no_interrupts(void)
{
    __asm__ volatile("cli");
    fault_stop_others();
}
#endif
#else
static void no_interrupts(void) {}
#endif

void power_shutdown(void)
{
    no_interrupts();
    if (FW_RESET)
        FW_RESET(BOOT_RESET_SHUTDOWN); /* does not return when it works */

#if defined(__x86_64__) || defined(__i386__)
    /* emulators: the ACPI "sleep type 5 + enable" write that each of them watches for */
    io_out16(0x604, 0x2000);  /* QEMU (q35 and newer pc machines) */
    io_out16(0xB004, 0x2000); /* QEMU (older) and Bochs */
    io_out16(0x4004, 0x3400); /* VirtualBox */
    io_out16(0x600, 0x34);    /* Cloud Hypervisor */
#endif
}

void power_restart(void)
{
    no_interrupts();
    if (FW_RESET)
        FW_RESET(BOOT_RESET_COLD);

#if defined(__x86_64__) || defined(__i386__)
    /* 1) the keyboard controller: pulse the CPU reset line (command 0xFE) */
    for (int i = 0; i < 100000; i++)
        if (!(io_in8(0x64) & 2)) /* wait until it can take a command */
            break;
    io_out8(0x64, 0xFE);
    for (volatile int i = 0; i < 1000000; i++)
    {
    }

    /* 2) the chipset's reset control register: first "system reset", then "full reset" */
    io_out8(0xCF9, 0x02);
    io_out8(0xCF9, 0x06);
    for (volatile int i = 0; i < 1000000; i++)
    {
    }

    /* 3) last resort: an empty interrupt table and an interrupt = triple fault = the CPU resets */
    struct
    {
        unsigned short limit;
        unsigned long long base;
    } __attribute__((packed)) empty = {0, 0};
    __asm__ volatile("lidt %0; int3" : : "m"(empty));
#endif
}
