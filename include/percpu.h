/* ============================================================
 * HarisOS per-CPU data: every processor finds its own record through the GS segment base.
 *
 *   this_cpu()   the PerCpu of the processor that runs this code: one instruction (mov %gs:0), no lock, no APIC id lookup
 *
 * Each processor has its own record; its address is stored in the IA32_GS_BASE register (MSR 0xC0000101) of that processor, so
 * "%gs:N" means "field at offset N of MY record". The first field points back to the record itself, which is what lets C code
 * get a normal pointer with one load. The interrupt handlers in idle.c use %gs: directly (the offsets below are fixed).
 *
 * Every processor has one (smp.c: smp_bsp_init, ap_entry). Ring 3 code may load a selector into GS and so zero the base, so the kernel
 * keeps its record in the GS base only while it runs in ring 0: every entry from ring 3 executes SWAPGS (it exchanges GS base with
 * IA32_KERNEL_GS_BASE) and so does every return to ring 3: SYSCALL entry / SYSRET and the IRETQ exits (syscall.c), the timer and network
 * interrupt (idle.c), all CPU exceptions (fault.c), and the first entry into a process (process.c). The handlers test the saved CS to
 * see where the interrupt came from, so an interrupt in ring 0 swaps nothing. Code that can run in the few instructions between SYSCALL
 * and its SWAPGS (an NMI, which fault.c handles) must not use %gs.
 * ============================================================ */
#ifndef HARIS_PERCPU_H
#define HARIS_PERCPU_H

#define PERCPU_OFF_SELF 0
#define PERCPU_OFF_TICKS 8

typedef struct PerCpu
{
    struct PerCpu *self;               /* offset 0: this record's own address (gs:0) */
    volatile unsigned long long ticks; /* offset 8: timer interrupts this processor has taken (gs:8) */
    unsigned index;                    /* 0 = boot processor */
    unsigned apic_id;
} PerCpu;

_Static_assert(__builtin_offsetof(PerCpu, self) == PERCPU_OFF_SELF, "idle.c reads gs:0");
_Static_assert(__builtin_offsetof(PerCpu, ticks) == PERCPU_OFF_TICKS, "idle.c increments gs:8");

#if defined(__x86_64__) && !defined(HARIS_HOST)
static inline PerCpu *this_cpu(void)
{
    PerCpu *p;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(p));
    return p;
}

/* Make `p` the record of the calling processor. */
static inline void percpu_set(PerCpu *p)
{
    unsigned long long v = (unsigned long long)p;
    p->self = p;
    __asm__ volatile("wrmsr" : : "c"(0xC0000101u), "a"((unsigned)v), "d"((unsigned)(v >> 32)));
}
#endif

#endif
