/* HarisOS kernel mutex and condition variable: see include/mutex.h. */
#include "mutex.h"
#include "irq.h"
#include "task.h"

/* Interrupts must be off in all of these helpers. */

/* The waiter in `mask` with the highest priority (lowest id among equals), or -1. Threads that are no longer blocked (ended or
 * killed while waiting) are dropped from the mask, so a wake-up is never spent on a thread that cannot use it. */
static int best_waiter(unsigned *mask)
{
    int best = -1, best_prio = -1;
    for (int i = 0; i < TASK_MAX; i++)
    {
        if (!(*mask & (1u << i)))
            continue;
        if (task_state(i) != TASK_BLOCKED)
        {
            *mask &= ~(1u << i);
            continue;
        }
        int p = task_priority(i);
        if (p > best_prio)
        {
            best = i;
            best_prio = p;
        }
    }
    return best;
}

static void wake_best(unsigned *mask)
{
    int id = best_waiter(mask);
    if (id >= 0)
    {
        *mask &= ~(1u << id);
        task_wake(id);
    }
}

void mutex_init(Mutex *m)
{
    m->owner = -1;
    m->waiters = 0;
}

/* 1 if somebody holds it. A holder that has ended or was killed (its slot is unused or dead) does not count: the lock is taken
 * back. (A new thread that got the same slot in the meantime would look like the old holder: the id is all that is kept.)
 * Interrupts must be off. */
static int is_locked(Mutex *m)
{
    if (m->owner >= 0)
    {
        TaskState st = task_state(m->owner);
        if (st == TASK_UNUSED || st == TASK_DEAD)
            m->owner = -1;
    }
    return m->owner >= 0;
}

int mutex_lock(Mutex *m)
{
    int me = task_current();
    unsigned long long irq = irq_save();
    if (m->owner == me)
    {
        irq_restore(irq);
        return -1; /* it would wait for itself for ever */
    }
    while (is_locked(m))
    {
        m->waiters |= 1u << me;
        task_block(); /* interrupts stay off from the check above to the switch: the unlock cannot slip in between */
    }
    m->waiters &= ~(1u << me); /* the main loop's task_block only yields: its bit may still be set */
    m->owner = me;
    irq_restore(irq);
    return 0;
}

int mutex_trylock(Mutex *m)
{
    unsigned long long irq = irq_save();
    int ok = !is_locked(m);
    if (ok)
        m->owner = task_current();
    irq_restore(irq);
    return ok ? 0 : -1;
}

/* The unlock itself, for mutex_unlock and cond_wait. Interrupts off, caller is the holder. */
static void release(Mutex *m)
{
    m->owner = -1;
    wake_best(&m->waiters);
}

int mutex_unlock(Mutex *m)
{
    unsigned long long irq = irq_save();
    if (m->owner != task_current())
    {
        irq_restore(irq);
        return -1;
    }
    release(m);
    irq_restore(irq);
    return 0;
}

int mutex_held(const Mutex *m) { return m->owner == task_current(); }

void cond_init(CondVar *c) { c->waiters = 0; }

int cond_wait(CondVar *c, Mutex *m)
{
    int me = task_current();
    unsigned long long irq = irq_save();
    if (m->owner != me)
    {
        irq_restore(irq);
        return -1;
    }
    c->waiters |= 1u << me; /* registered before the mutex is let go: a signal sent after the unlock cannot be missed */
    release(m);
    while (c->waiters & (1u << me)) /* signal / broadcast clear the bit; the main loop's yield-only block just goes round */
        task_block();
    irq_restore(irq);
    return mutex_lock(m);
}

/* The pieces of the waits above for the system call layer, which sleeps in its own loop so that a signal can end the wait. */
void mutex_wait_for(Mutex *m, int task)
{
    if (task >= 0 && task < TASK_MAX)
        m->waiters |= 1u << task;
}

void mutex_cancel_wait(Mutex *m, int task)
{
    if (task >= 0 && task < TASK_MAX)
        m->waiters &= ~(1u << task);
}

void cond_wait_for(CondVar *c, int task)
{
    if (task >= 0 && task < TASK_MAX)
        c->waiters |= 1u << task;
}

void cond_cancel_wait(CondVar *c, int task)
{
    if (task >= 0 && task < TASK_MAX)
        c->waiters &= ~(1u << task);
}

int cond_waiting(const CondVar *c, int task) { return task >= 0 && task < TASK_MAX && (c->waiters & (1u << task)) != 0; }

void cond_signal(CondVar *c)
{
    unsigned long long irq = irq_save();
    wake_best(&c->waiters);
    irq_restore(irq);
}

void cond_broadcast(CondVar *c)
{
    unsigned long long irq = irq_save();
    for (int i = 0; i < TASK_MAX; i++)
        if (c->waiters & (1u << i))
            task_wake(i);
    c->waiters = 0;
    irq_restore(irq);
}
