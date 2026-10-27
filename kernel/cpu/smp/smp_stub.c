/* SMP: stubs for builds that are not x86-64 or are host tests. */
#include "smp_internal.h"

#if !(defined(__x86_64__) && !defined(HARIS_HOST))

int smp_install(u64 code, u64 data)
{
    (void)code;
    (void)data;
    return -1;
}
int smp_start(u64 code, u64 data)
{
    (void)code;
    (void)data;
    return 0;
}
void smp_bsp_init(void) {}
int smp_bsp_gs_check(void) { return -1; }
void smp_set_kernel_stack(u64 rsp0) { (void)rsp0; }
int smp_run_on(int cpu, void (*fn)(void))
{
    (void)cpu;
    (void)fn;
    return -1;
}
int smp_job_done(int cpu)
{
    (void)cpu;
    return 0;
}
int smp_selftest_spinlock(void) { return 1; }
int smp_selftest_alloc(void) { return 1; }
int smp_selftest_tlb(void) { return 1; }
int smp_selftest_sched(void) { return 1; }
void smp_tlb_flush_others(void) {}
int smp_stop_others(void) { return 0; }
void smp_wake_idle(void) {}
unsigned smp_irq_target(void) { return 0; }
int smp_cpu_total(void) { return 1; }
int smp_cpu_online(int i) { return i == 0; }
unsigned smp_cpu_apic_id(int i)
{
    (void)i;
    return 0;
}
u64 smp_cpu_lazy_ticks(int i)
{
    (void)i;
    return 0;
}
u64 smp_cpu_ticks(int i)
{
    (void)i;
    return 0;
}

#endif
