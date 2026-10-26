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
#define PERCPU_OFF_INDEX 16
#define PERCPU_OFF_KSTACK 24
#define PERCPU_OFF_USER_RSP 32
#define PERCPU_OFF_USE_IRET 40
#define PERCPU_OFF_IRET_RIP 48
#define PERCPU_OFF_IRET_RFLAGS 56

typedef struct PerCpu
{
    struct PerCpu *self;               /* offset 0: this record's own address (gs:0) */
    volatile unsigned long long ticks; /* offset 8: timer interrupts this processor has taken (gs:8) */
    unsigned index;                    /* 16: 0 = boot processor */
    unsigned apic_id;
    /* The SYSCALL entry (syscall.c) works in these through %gs, so two processors can enter the kernel at once. */
    unsigned long long kstack;       /* 24: top of the kernel stack of the thread that runs here */
    unsigned long long user_rsp;     /* 32: the user's RSP, between SYSCALL and the switch to the kernel stack */
    unsigned long long use_iret;     /* 40: leave through IRETQ instead of SYSRETQ (signal return from a timer-tick frame) */
    unsigned long long iret_rip;     /* 48 */
    unsigned long long iret_rflags;  /* 56 */
    volatile unsigned long long idle; /* 64: 1 while the processor waits in HLT without the big kernel lock (bkl.h) */
    volatile unsigned long long nolock; /* 72: 1 while this processor runs a system call that does not take the big kernel lock (syscall.c) */
    volatile unsigned long long lazy_ticks; /* 80: timer ticks an idle application processor answered without the big kernel lock (idle.c; part of ticks) */
} PerCpu;

_Static_assert(__builtin_offsetof(PerCpu, self) == PERCPU_OFF_SELF, "idle.c reads gs:0");
_Static_assert(__builtin_offsetof(PerCpu, ticks) == PERCPU_OFF_TICKS, "idle.c increments gs:8");
_Static_assert(__builtin_offsetof(PerCpu, index) == PERCPU_OFF_INDEX, "percpu_index reads gs:16");
_Static_assert(__builtin_offsetof(PerCpu, kstack) == PERCPU_OFF_KSTACK, "syscall_entry uses gs:24");
_Static_assert(__builtin_offsetof(PerCpu, user_rsp) == PERCPU_OFF_USER_RSP, "syscall_entry uses gs:32");
_Static_assert(__builtin_offsetof(PerCpu, use_iret) == PERCPU_OFF_USE_IRET, "syscall_entry uses gs:40");
_Static_assert(__builtin_offsetof(PerCpu, iret_rip) == PERCPU_OFF_IRET_RIP, "syscall_entry uses gs:48");
_Static_assert(__builtin_offsetof(PerCpu, iret_rflags) == PERCPU_OFF_IRET_RFLAGS, "syscall_entry uses gs:56");
_Static_assert(__builtin_offsetof(PerCpu, idle) == 64, "bkl.h reads gs:64");
_Static_assert(__builtin_offsetof(PerCpu, nolock) == 72, "syscall_entry uses gs:72");
_Static_assert(__builtin_offsetof(PerCpu, lazy_ticks) == 80, "idle_isr_timer uses gs:80");

#if defined(HARIS_HOST)
static inline unsigned percpu_index(void) { return 0; } /* host tests run on "processor 0" */
#elif defined(__x86_64__)
extern volatile int percpu_ready; /* smp.c: 1 once smp_bsp_init has set the boot processor's GS base */

/* The number of the processor that runs this code (0 = boot processor). 0 before smp_bsp_init, when no GS base is set up yet. */
static inline unsigned percpu_index(void)
{
    unsigned i;
    if (!percpu_ready)
        return 0;
    __asm__ volatile("movl %%gs:16, %0" : "=r"(i));
    return i;
}
#endif

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
