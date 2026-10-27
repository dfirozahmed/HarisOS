/* SMP: starting all the application processors, per processor figures. */
#include "smp_internal.h"

#if defined(__x86_64__) && !defined(HARIS_HOST)

int smp_start(u64 code, u64 data)
{
    if (!code || !data || !idle_ready() || !timer_ready())
        return 0;

    unsigned ids[ACPI_MAX_CPUS], aps[SMP_MAX_CPUS];
    int n = acpi_cpu_count();
    for (int i = 0; i < n; i++)
        ids[i] = acpi_cpu_apic_id(i);
    unsigned bsp = idle_lapic_id();
    int count = smp_pick_aps(ids, n, bsp, idle_is_x2apic() ? 0xFFFFFFFEu : 0xFFu, aps, SMP_MAX_CPUS - 1);

    CPU[0].pc.apic_id = bsp;
    CPU[0].lapic_id = bsp;
    CPU[0].online = 1;
    TOTAL = 1;

    volatile SmpParams *p = (volatile SmpParams *)data;
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    p->entry = (u64)ap_entry;
    p->cr3 = cr3;
    p->efer = rdmsr(MSR_EFER) & ~EFER_LMA;
    u64 cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    p->cr0 = cr0 & ~((1ULL << 30) | (1ULL << 29)); /* never CD / NW: caches off */
    p->cr4 = cr4;
    p->pat = rdmsr(MSR_PAT);
    PARAMS = p;

    int up = 0;
    for (int k = 1; k <= count; k++)
    {
        CPU[k].pc.apic_id = aps[k - 1];
        CPU[k].online = 0;
        TOTAL = k + 1;
        p->stack = (u64)(AP_STACK[k] + SMP_STACK_SIZE);
        p->index = (u64)k;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        /* INIT, then SIPI twice: the second is ignored by a processor that already started on the first. */
        if (idle_send_ipi(aps[k - 1], 0x4500) != 0)
            continue;
        wait_us(10000);
        unsigned sipi = 0x4600 | (unsigned)(code >> 12);
        idle_send_ipi(aps[k - 1], sipi);
        wait_us(200);
        idle_send_ipi(aps[k - 1], sipi);

        u64 t0 = timer_us();
        while (!__atomic_load_n(&CPU[k].online, __ATOMIC_ACQUIRE) && timer_us() - t0 < 200000)
            __asm__ volatile("pause");
        if (CPU[k].online)
            up++;
    }
    if (up)
        wait_us(20000); /* long enough for the first timer ticks of the processors started last (smp_cpu_ticks) */
    return up;
}

int smp_cpu_total(void) { return TOTAL; }
int smp_cpu_online(int i) { return i >= 0 && i < TOTAL && CPU[i].online; }
unsigned smp_cpu_apic_id(int i) { return (i >= 0 && i < TOTAL) ? CPU[i].pc.apic_id : 0; }
u64 smp_cpu_ticks(int i) { return (i > 0 && i < TOTAL) ? CPU[i].pc.ticks : 0; }
u64 smp_cpu_lazy_ticks(int i) { return (i > 0 && i < TOTAL) ? CPU[i].pc.lazy_ticks : 0; }

#endif
