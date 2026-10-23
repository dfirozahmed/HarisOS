/* HarisOS reader-writer lock: see include/rwlock.h. */
#include "rwlock.h"
#include "irq.h"
#include "task.h"

/* Interrupts must be off in all of these helpers. */

static int alive(int id)
{
    TaskState st = task_state(id);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* Drop the threads that ended or were killed while waiting, and a writer that ended while holding the lock: neither may keep others
 * out. (Only the thread id is kept, so a new thread that reuses a slot looks like the old one.) */
static void clean(RWLock *l)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (((l->rwaiters | l->wwaiters) & (1u << i)) && !alive(i))
        {
            l->rwaiters &= ~(1u << i);
            l->wwaiters &= ~(1u << i);
        }
    if (l->writer >= 0 && !alive(l->writer))
        l->writer = -1;
}

/* The waiter of `mask` with the highest priority (lowest id among equals) is woken and removed from it. */
static void wake_best(unsigned *mask)
{
    int best = -1, best_prio = -1;
    for (int i = 0; i < TASK_MAX; i++)
        if ((*mask & (1u << i)) && task_state(i) == TASK_BLOCKED)
        {
            int p = task_effective_priority(i);
            if (p > best_prio)
            {
                best = i;
                best_prio = p;
            }
        }
    if (best >= 0)
    {
        *mask &= ~(1u << best);
        task_wake(best);
    }
}

static void wake_all(unsigned *mask)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (*mask & (1u << i))
            task_wake(i);
    *mask = 0;
}

void rw_init(RWLock *l)
{
    l->readers = 0;
    l->writer = -1;
    l->rwaiters = l->wwaiters = 0;
}

/* A reader may enter when no writer holds the lock and none waits for it (writers first). */
static int reader_may_enter(RWLock *l)
{
    clean(l);
    return l->writer < 0 && l->wwaiters == 0;
}

static int writer_may_enter(RWLock *l)
{
    clean(l);
    return l->writer < 0 && l->readers == 0;
}

int rw_rdlock(RWLock *l)
{
    int me = task_current();
    unsigned long long irq = irq_save();
    while (!reader_may_enter(l))
    {
        l->rwaiters |= 1u << me;
        task_block(); /* interrupts stay off from the check to the switch: the unlock cannot slip in between */
    }
    l->rwaiters &= ~(1u << me);
    l->readers++;
    irq_restore(irq);
    return 0;
}

int rw_tryrdlock(RWLock *l)
{
    unsigned long long irq = irq_save();
    int ok = reader_may_enter(l);
    if (ok)
        l->readers++;
    irq_restore(irq);
    return ok ? 0 : -1;
}

/* After a reader or the writer let go, decide who may try next: waiting writers first (when no reader is left), otherwise all readers. */
static void hand_over(RWLock *l)
{
    clean(l);
    if (l->writer >= 0)
        return;
    if (l->wwaiters)
    {
        if (l->readers == 0)
            wake_best(&l->wwaiters);
    }
    else
        wake_all(&l->rwaiters);
}

int rw_rdunlock(RWLock *l)
{
    unsigned long long irq = irq_save();
    if (l->readers <= 0)
    {
        irq_restore(irq);
        return -1;
    }
    l->readers--;
    hand_over(l);
    irq_restore(irq);
    return 0;
}

int rw_wrlock(RWLock *l)
{
    int me = task_current();
    unsigned long long irq = irq_save();
    if (l->writer == me)
    {
        irq_restore(irq);
        return -1; /* it would wait for itself for ever */
    }
    while (!writer_may_enter(l))
    {
        l->wwaiters |= 1u << me;
        task_block();
    }
    l->wwaiters &= ~(1u << me);
    l->writer = me;
    irq_restore(irq);
    return 0;
}

int rw_trywrlock(RWLock *l)
{
    unsigned long long irq = irq_save();
    int ok = writer_may_enter(l);
    if (ok)
        l->writer = task_current();
    irq_restore(irq);
    return ok ? 0 : -1;
}

int rw_wrunlock(RWLock *l)
{
    unsigned long long irq = irq_save();
    if (l->writer != task_current())
    {
        irq_restore(irq);
        return -1;
    }
    l->writer = -1;
    /* writers that wait go first; readers that were held back by them follow when none is left */
    if (l->wwaiters)
        wake_best(&l->wwaiters);
    else
        wake_all(&l->rwaiters);
    irq_restore(irq);
    return 0;
}
