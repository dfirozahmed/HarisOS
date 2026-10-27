/* Kernel threads: block, wake, yield, priorities, sleep, exit, the timer tick. */
#include "task_internal.h"

void task_block(void)
{
    if (CUR == 0) /* the main loop never blocks: it is what runs when everybody else does */
    {
        task_yield();
        return;
    }
    unsigned long long irq = irq_save();
    sched_lock();
    tk(CUR)->state = TASK_BLOCKED; /* under the lock: a waker never sees us READY before we have left */
    schedule_locked();
    irq_restore(irq);
}

/* A thread became READY: a processor asleep in HLT should look for it now (smp.h), not at its next timer tick. */
void kick_idle_cpus(void)
{
#ifndef HARIS_HOST
    smp_wake_idle();
#endif
}

void task_wake(int id)
{
    if (id < 0 || id >= CAP)
        return;
    unsigned long long irq = irq_save();
    sched_lock();
    int woke = tk(id)->state == TASK_BLOCKED;
    if (woke)
        tk(id)->state = TASK_READY;
    sched_unlock();
    if (woke)
        kick_idle_cpus();
    irq_restore(irq);
}

AddressSpace *task_space_of(int id)
{
    return id >= 0 && id < CAP && tk(id)->state != TASK_UNUSED && tk(id)->state != TASK_DEAD && !tk(id)->kill_pending ? tk(id)->as : 0;
}

void task_yield(void)
{
    unsigned long long irq = irq_save();
    schedule();
    bkl_yield(); /* code that waits by yielding in a loop must not keep the other processors out of the kernel (bkl.h) */
    irq_restore(irq);
}

int task_set_priority(int id, int prio)
{
    if (id < 0 || id >= CAP || prio < TASK_PRIO_MIN || prio > TASK_PRIO_MAX)
        return -1;
    unsigned long long irq = irq_save();
    sched_lock();
    if (tk(id)->state == TASK_UNUSED || tk(id)->state == TASK_DEAD)
    {
        sched_unlock();
        irq_restore(irq);
        return -1;
    }
    tk(id)->prio = prio;
    tk(id)->waited = 0; /* an old boost belonged to the old level */
    sched_unlock();
    irq_restore(irq);
    return 0;
}

int task_priority(int id)
{
    if (id < 0 || id >= CAP)
        return -1;
    unsigned long long irq = irq_save();
    sched_lock();
    int p = tk(id)->state != TASK_UNUSED && tk(id)->state != TASK_DEAD ? tk(id)->prio : -1;
    sched_unlock();
    irq_restore(irq);
    return p;
}

int task_set_inherited(int id, int prio)
{
    if (id < 0 || id >= CAP || prio < -1 || prio > TASK_PRIO_MAX)
        return -1;
    unsigned long long irq = irq_save();
    sched_lock();
    if (tk(id)->state == TASK_UNUSED || tk(id)->state == TASK_DEAD)
    {
        sched_unlock();
        irq_restore(irq);
        return -1;
    }
    tk(id)->inherit = prio;
    sched_unlock();
    irq_restore(irq);
    return 0;
}

int task_effective_priority(int id)
{
    if (id < 0 || id >= CAP)
        return -1;
    unsigned long long irq = irq_save();
    sched_lock();
    int p = -1;
    if (tk(id)->state != TASK_UNUSED && tk(id)->state != TASK_DEAD)
        p = tk(id)->inherit > tk(id)->prio ? tk(id)->inherit : tk(id)->prio;
    sched_unlock();
    irq_restore(irq);
    return p;
}

void task_sleep_ms(unsigned ms)
{
    if (CUR == 0) /* the main loop never sleeps: it is what runs when everybody else does */
    {
        task_yield();
        return;
    }
    unsigned long long irq = irq_save();
    sched_lock();
    tk(CUR)->wake_us = timer_us() + (unsigned long long)ms * 1000;
    tk(CUR)->state = TASK_SLEEPING;
    schedule_locked();
    irq_restore(irq);
}

__attribute__((noreturn)) void task_exit(void)
{
    if (CUR == 0)
        __builtin_trap(); /* ending the main loop would leave nothing to run */
    irq_save(); /* never restored: this thread does not continue */
    sched_lock();
    tk(CUR)->state = TASK_DEAD;
    schedule_locked(); /* never comes back: a dead thread is never chosen */
    for (;;)
        __builtin_trap();
}

void task_tick(void)
{
    sched_lock();
    TICKS++;
    Task *t = tk(CUR);
    int cpu = (int)percpu_index();
    CPU_TICKS_ALL[cpu]++;
    if (!t->idle_cpu)
        t->run_ticks++; /* (thread 0 also counts the ticks that end its sleep in HLT: the Task Manager shows the main loop's own figure for it) */
    /* busy = running a thread other than the processor's idle: on an application processor every thread but its idle context; on the boot
     * processor every thread but the main loop (thread 0), whose own busy share is measured in the loop (cpuload.h, cpuload_boot_percent) */
    if (cpu > 0 ? !t->idle_cpu : CUR != 0)
        CPU_TICKS_BUSY[cpu]++;
    if (t->kill_pending && CUR != 0)
    {
        /* task_kill found this thread running on this processor. It was in ring 3 then, so it holds nothing: end it here. The interrupt
         * frame on its stack is thrown away with the rest of the thread. */
        t->kill_pending = 0;
        t->state = TASK_DEAD;
        schedule_locked(); /* never comes back: a dead thread is never chosen */
        return;
    }
    if (!(t->flags & TASK_PREEMPTIBLE) || t->state != TASK_RUNNING || preempt_count > 0)
    {
        sched_unlock();
        return;
    }
    /* A thread that matters more became ready (task_wake, a sleep that ended, a priority that was raised): it takes the CPU now, not
     * when the slice ends. This is the only safe place for it: task_wake can be called from code that must not be switched away from
     * (it holds interrupts off, a pipe, the console), so it only makes the thread READY and this tick, at most one millisecond later,
     * does the switch. A running thread that is not preemptible never gets here (the return above). */
    if (!better_waiting() && --SLICE_LEFT > 0)
    {
        sched_unlock();
        return;
    }
    SLICE_LEFT = slice_of(CUR);
    schedule_locked(); /* we are in the interrupt: interrupts are off */
}

int task_others_ready(void)
{
    unsigned long long irq = irq_save();
    sched_lock();
    unsigned long long now = timer_us();
    int found = 0;
    int me = (int)percpu_index();
    for (int i = 0; i < CAP && !found; i++)
    {
        if (i == CUR || !eligible(i, me))
            continue;
        if (tk(i)->state == TASK_READY ||
            (tk(i)->state == TASK_SLEEPING && (long long)(now - tk(i)->wake_us) >= 0))
            found = 1;
    }
    sched_unlock();
    irq_restore(irq);
    return found;
}

int task_current(void) { return CUR; }

int task_slots(void) { return CAP; }

void **task_held_slot(int id) { return id >= 0 && id < CAP ? &tk(id)->held : 0; }

void **task_blocked_slot(int id) { return id >= 0 && id < CAP ? &tk(id)->blocked : 0; }

void task_set_exit_code(int id, int code)
{
    if (id >= 0 && id < CAP)
        tk(id)->exit_code = code;
}

int task_exit_code(int id) { return id >= 0 && id < CAP ? tk(id)->exit_code : 0; }

int task_count(void)
{
    int n = 0;
    for (int i = 0; i < CAP; i++)
        if (tk(i)->state != TASK_UNUSED && tk(i)->state != TASK_DEAD && !tk(i)->idle_cpu)
            n++;
    return n;
}

/* The idle contexts of the application processors are not threads to the outside: they do not show up in the lists. */
TaskState task_state(int id)
{
    if (id < 0 || id >= CAP || tk(id)->idle_cpu)
        return TASK_UNUSED;
    return tk(id)->kill_pending ? TASK_DEAD : tk(id)->state; /* a thread task_kill found running on another processor is already gone to the outside */
}

const char *task_name(int id)
{
    return id >= 0 && id < CAP && tk(id)->state != TASK_UNUSED && !tk(id)->idle_cpu ? tk(id)->name : 0;
}
