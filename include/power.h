/* ============================================================
 * HarisOS power control: shut down and restart the computer.
 *
 *   power_init(info);     once at boot (remembers the firmware's reset service)
 *   power_shutdown();     turn the machine off
 *   power_restart();      reboot
 *
 * Both work in this order and stop at the first one that works:
 *   1. the firmware's own reset service (UEFI ResetSystem: shutdown / cold reset). This is the
 *      right way on real PCs and on QEMU with OVMF.
 *   2. well-known fallbacks for emulators: ACPI power-off ports of QEMU / Bochs / VirtualBox;
 *      for restart the keyboard controller reset line, the PCI reset register, and finally a CPU
 *      triple fault.
 * They do not return when they work. If they do return, nothing worked: the caller should tell
 * the user ("it is now safe to turn off...") and halt.
 * ============================================================ */
#ifndef HARIS_POWER_H
#define HARIS_POWER_H

#include "bootinfo.h"

void power_init(const BootInfo *info);
void power_shutdown(void);
void power_restart(void);

#endif
