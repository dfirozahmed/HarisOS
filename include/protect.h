/* ============================================================
 * HarisOS CPU protections against kernel mistakes (SMEP, SMAP)
 *
 *   protect_init();      once, early: switch on whatever the CPU has
 *   protect_status();    PROTECT_SMEP | PROTECT_SMAP: which ones are on
 *   protect_selftest();  boot self test: proves the protections and the NMI stack really work
 *
 * SMEP (Supervisor Mode Execution Prevention): ring 0 may not execute code on a page that ring 3 can use. A kernel bug
 * that jumps to a user address (a corrupted function pointer, a return address overwritten from ring 3) then faults
 * instead of running attacker code.
 *
 * SMAP (Supervisor Mode Access Prevention): ring 0 may not read or write such pages either, except in a window the
 * kernel opens on purpose (uaccess.h). It catches a forgotten pointer check where it happens.
 *
 * Both are bits of CR4, announced by CPUID leaf 7. Without them the kernel still works, just without the net.
 * protect_cr4() has no hardware access and is tested on the host.
 * ============================================================ */
#ifndef HARIS_PROTECT_H
#define HARIS_PROTECT_H

#define PROTECT_SMEP 1
#define PROTECT_SMAP 2

#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)

/* CR4 with the bits for what CPUID leaf 7 (EBX) says exists: bit 7 = SMEP, bit 20 = SMAP. Other bits are kept. */
unsigned long long protect_cr4(unsigned cpuid7_ebx, unsigned long long cr4);

void protect_init(void);
int protect_status(void);

/* 0 = fine (or nothing to test), otherwise a negative number that says which check failed. Needs the address space
 * code (aspace_init), the GDT / IDT of idle_init, and for the NMI part the local APIC (idle_ready). */
int protect_selftest(void);

#endif
