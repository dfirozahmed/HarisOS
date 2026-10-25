/* ============================================================
 * HarisOS GDT: the segments the CPU needs in 64-bit mode
 *
 *   gdt_init();                  once (idle_init does it): build our GDT, load it, load the task register
 *   gdt_set_kernel_stack(rsp0);  the stack the CPU switches to when an interrupt or fault arrives from ring 3
 *   gdt_check();                 0 if the CPU accepts the selectors (a boot self test), else a negative number
 *
 * In 64-bit mode segmentation is nearly switched off: base and limit are ignored, only the privilege level (ring 0
 * kernel, ring 3 user) and the "this is 64-bit code" bit matter. The layout is in segments.h.
 *
 * Why a stack in the TSS: when ring 3 code is interrupted, the CPU must not keep using the user's stack (it may be
 * invalid or hostile), so it loads RSP from the TSS (rsp0) before it pushes the interrupt frame. Every thread that
 * can enter user mode needs its own kernel stack there; the scheduler will set it on each switch.
 *
 * gdt_build() has no hardware access and is tested on the host (HARIS_HOST).
 * ============================================================ */
#ifndef HARIS_GDT_H
#define HARIS_GDT_H

#include "segments.h"

/* Fill a GDT of GDT_ENTRIES words; tss = the two words of the TSS descriptor (see fault_tss_descriptor). */
void gdt_build(unsigned long long gdt[GDT_ENTRIES], const unsigned long long tss[2]);

void gdt_init(void);
void gdt_load(unsigned long long gdt[GDT_ENTRIES]); /* lgdt + segment registers + ltr on the calling processor (smp.c) */
void gdt_set_kernel_stack(unsigned long long rsp0);
int gdt_check(void);

#endif
