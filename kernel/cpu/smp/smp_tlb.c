/* SMP: TLB shootdown, interrupt target, waking idle processors, stopping the others. */
#include "smp_internal.h"

#if defined(__x86_64__) && !defined(HARIS_HOST)

/* ---- TLB shootdown (smp.h) ---- */
volatile u64 smp_tlb_acks; /* incremented by idle_isr_tlb (idle.c) */

void smp_tlb_flush_others(void)
{
    for (int k = 0; k < TOTAL; k++)
    {
        if (k == (int)percpu_index() || !__atomic_load_n(&CPU[k].online, __ATOMIC_ACQUIRE))
            continue;
        idle_send_ipi(CPU[k].pc.apic_id, 0x4000 | IDLE_VEC_TLB); /* fixed delivery, level assert */
    }
}

unsigned smp_irq_target(void)
{
    static unsigned next;
    int aps = 0;
    for (int k = 1; k < TOTAL; k++)
        if (CPU[k].online && CPU[k].pc.apic_id <= 0xFF) /* an I/O APIC destination has 8 bits */
            aps++;
    if (!aps)
        return CPU[0].pc.apic_id;
    unsigned want = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % (unsigned)aps;
    for (int k = 1; k < TOTAL; k++)
        if (CPU[k].online && CPU[k].pc.apic_id <= 0xFF && want-- == 0)
            return CPU[k].pc.apic_id;
    return CPU[0].pc.apic_id;
}

void smp_wake_idle(void)
{
    unsigned me = idle_lapic_id();
    for (int k = 0; k < TOTAL; k++)
        if (CPU[k].pc.apic_id != me && __atomic_load_n(&CPU[k].online, __ATOMIC_ACQUIRE) && CPU[k].pc.idle)
            idle_send_ipi(CPU[k].pc.apic_id, 0x4000 | IDLE_VEC_WAKE);
}

int smp_stop_others(void)
{
    int sent = 0;
    unsigned me = idle_lapic_id();
    for (int k = 0; k < TOTAL; k++)
        if (CPU[k].pc.apic_id != me && CPU[k].online && idle_send_ipi(CPU[k].pc.apic_id, 0x4400) == 0) /* delivery mode NMI, level assert */
            sent++;
    return sent;
}

int smp_selftest_tlb(void)
{
    int n = 0;
    for (int k = 1; k < TOTAL; k++)
        if (CPU[k].online)
            n++;
    if (!n)
        return 1;
    u64 before = __atomic_load_n(&smp_tlb_acks, __ATOMIC_ACQUIRE);
    smp_tlb_flush_others();
    u64 t0 = timer_us();
    bkl_unlock(); /* a processor that waits for the lock has interrupts off and could not answer; let them in while we wait */
    while (__atomic_load_n(&smp_tlb_acks, __ATOMIC_ACQUIRE) - before < (u64)n && timer_us() - t0 < 1000000)
        __asm__ volatile("pause");
    bkl_lock();
    return __atomic_load_n(&smp_tlb_acks, __ATOMIC_ACQUIRE) - before == (u64)n ? 0 : -1;
}

#endif
