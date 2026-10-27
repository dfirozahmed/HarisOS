/* Kernel threads: per processor time, affinity, the idle contexts of the other processors. */
#include "task_internal.h"

void task_cpu_ticks(int cpu, unsigned long long *busy, unsigned long long *all)
{
    int ok = cpu >= 0 && cpu < SMP_MAX_CPUS;
    *busy = ok ? CPU_TICKS_BUSY[cpu] : 0;
    /* An application processor that sleeps answers its timer ticks without calling task_tick (idle.c), so its own count of interrupts is the
     * total; the ticks that found it busy all went through task_tick. */
    *all = !ok ? 0 : CPU_TICKS_ALL[cpu];
#ifndef HARIS_HOST
    if (ok && cpu > 0)
        *all = smp_cpu_ticks(cpu);
#endif
}

unsigned long long task_run_ticks(int id) { return id >= 0 && id < CAP && !tk(id)->idle_cpu ? tk(id)->run_ticks : 0; }

int task_last_cpu(int id) { return id >= 0 && id < CAP && !tk(id)->idle_cpu ? tk(id)->last_cpu : -1; }

unsigned task_cpu_mask(int id) { return id >= 0 && id < CAP ? tk(id)->cpus_ran : 0; }

int task_set_affinity(int id, int cpu)
{
    if (id <= 0 || id >= CAP || cpu < -1 || cpu >= SMP_MAX_CPUS)
        return -1;
    unsigned long long irq = irq_save();
    sched_lock();
    Task *t = tk(id);
    int bad = t->state == TASK_UNUSED || t->state == TASK_DEAD || t->idle_cpu || (cpu > 0 && (!t->as || !IDLE_ID[cpu]));
    if (!bad)
        t->affinity = cpu;
    sched_unlock();
    if (!bad)
        kick_idle_cpus(); /* the processor it is pinned to may sleep in HLT while the thread is READY */
    irq_restore(irq);
    return bad ? -1 : 0;
}

int task_affinity(int id)
{
    if (id < 0 || id >= CAP)
        return -1;
    unsigned long long irq = irq_save();
    sched_lock();
    int a = tk(id)->state != TASK_UNUSED && tk(id)->state != TASK_DEAD && !tk(id)->idle_cpu ? tk(id)->affinity : -1;
    sched_unlock();
    irq_restore(irq);
    return a;
}

void task_reserve_boot_cpu(int on)
{
    unsigned long long irq = irq_save();
    sched_lock();
    BOOT_RESERVED = on != 0;
    sched_unlock();
    irq_restore(irq);
}

/* The calling application processor (its idle loop in smp.c, in the kernel with the big kernel lock, interrupts off) becomes a context the
 * scheduler can switch to and from: a slot of its own that is never a candidate for the others. 0, or -1 (not an application processor,
 * already done, no slot). */
int task_register_idle(void)
{
    int cpu = (int)percpu_index();
    if (cpu < 1 || cpu >= SMP_MAX_CPUS || IDLE_ID[cpu])
        return -1;
    for (;;)
    {
        unsigned long long irq = irq_save();
        sched_lock();
        for (int i = 1; i < CAP; i++)
            if (tk(i)->state == TASK_UNUSED)
            {
                Task *t = tk(i);
                t->stack = 0;
                t->guarded = 0;
                t->as = 0;
                t->data = 0;
                t->free_data = 0;
                t->flags = 0;
                t->prio = TASK_PRIO_DEFAULT;
                t->inherit = -1;
                t->waited = 0;
                t->held = 0;
                t->blocked = 0;
                t->exit_code = 0;
                t->cpus_ran = 1u << cpu;
                t->run_ticks = 0;
                t->last_cpu = cpu;
                t->kill_pending = 0;
                t->idle_cpu = cpu;
                t->affinity = -1;
                t->name[0] = 'I';
                t->name[1] = 'D';
                t->name[2] = 'L';
                t->name[3] = 'E';
                t->name[4] = (char)('0' + cpu % 10);
                t->name[5] = 0;
                t->state = TASK_RUNNING; /* the calling processor is running it right now */
                IDLE_ID[cpu] = i;
                AP_READY++;
                CURS[cpu] = i;
                sched_unlock();
                irq_restore(irq);
                return 0;
            }
        sched_unlock();
        irq_restore(irq);
        if (!add_chunk())
            return -1;
    }
}

/* 1 if an application processor's idle loop should go into the kernel (take the big kernel lock, task_cpu_idle): a user thread it may run is
 * READY, or asleep with its time come. It looks with the scheduler lock alone, interrupts off, so a processor with nothing to do never
 * touches the big kernel lock. A processor that has not registered its idle context yet always says yes. A wrong "yes" costs one visit to
 * the kernel; a wrong "no" cannot happen for a thread that was READY before the call (the lock orders it after the wake-up), and one that
 * becomes READY later sends smp_wake_idle to every processor that sleeps. */
int task_cpu_has_work(void)
{
    int cpu = (int)percpu_index();
    if (cpu < 1 || !IDLE_ID[cpu])
        return 1;
    unsigned long long irq = irq_save();
    sched_lock();
    unsigned long long now = timer_us();
    int found = 0;
    for (int i = 0; i < CAP && !found; i++)
    {
        if (!eligible(i, cpu))
            continue;
        if (tk(i)->state == TASK_READY || (tk(i)->state == TASK_SLEEPING && (long long)(now - tk(i)->wake_us) >= 0))
            found = 1;
    }
    sched_unlock();
    irq_restore(irq);
    return found;
}

/* The idle loop of an application processor asks: is there a user thread for me? It runs it (schedule() switches away and comes back
 * here, in the idle context, when the processor has nothing to run again). Kernel, big kernel lock held, interrupts off. */
void task_cpu_idle(void)
{
    int cpu = (int)percpu_index();
    if (cpu < 1 || !IDLE_ID[cpu])
        return;
    schedule();
}

unsigned long long task_switches(void) { return SWITCHES; }

unsigned long long task_ticks(void) { return TICKS; }

const char *task_guard_hit(unsigned long long addr)
{
    for (int i = 0; i < CAP; i++)
    {
        const Task *t = tk(i);
        unsigned long long base = (unsigned long long)t->stack;
        if (t->guarded && t->state != TASK_UNUSED && addr >= base && addr < base + PMM_PAGE_SIZE)
            return t->name;
    }
    return 0;
}
