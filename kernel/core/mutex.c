/* HarisOS kernel mutex and condition variable: see include/mutex.h. */
#include "mutex.h"
#include "irq.h"
#include "task.h"

/* Interrupts must be off in all of these helpers. */

/* Priority inheritance. Every thread has the list of mutexes it holds (Mutex.next_held) and the mutex it sleeps on (if any). Whenever a
 * thread starts or stops waiting for a mutex, or a mutex is let go, the holder's borrowed priority is worked out again: the best
 * effective priority among all the threads that wait for any mutex it still holds (task_set_inherited). A holder that is itself
 * waiting for another mutex passes the priority on down the chain (so the borrowing is transitive). */
static Mutex *HELD[TASK_MAX];
static Mutex *BLOCKED_ON[TASK_MAX];

static int alive(int id)
{
    TaskState st = task_state(id);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

static void unlink_held(Mutex *m, int owner)
{
    if (owner >= 0 && owner < TASK_MAX)
        for (Mutex **pp = &HELD[owner]; *pp; pp = &(*pp)->next_held)
            if (*pp == m)
            {
                *pp = m->next_held;
                break;
            }
    m->next_held = 0;
}

static void take(Mutex *m, int me)
{
    m->owner = me;
    m->next_held = HELD[me];
    HELD[me] = m;
}

static void drop(Mutex *m)
{
    unlink_held(m, m->owner);
    m->owner = -1;
}

static void recompute(int owner)
{
    int eff = -1;
    for (const Mutex *h = HELD[owner]; h; h = h->next_held)
        for (int i = 0; i < TASK_MAX; i++)
            if ((h->waiters & (1u << i)) && i != owner && alive(i))
            {
                int p = task_effective_priority(i);
                if (p > eff)
                    eff = p;
            }
    task_set_inherited(owner, eff);
}

/* The waiters of m changed: refresh its holder, then the holder of whatever that holder waits for, and so on (bounded: a cycle of
 * threads waiting for each other is a deadlock, which this does not try to solve, but it must not loop for ever). */
static void propagate(Mutex *m)
{
    for (int depth = 0; m && depth < TASK_MAX; depth++)
    {
        int o = m->owner;
        if (o < 0 || o >= TASK_MAX)
            break;
        recompute(o);
        m = BLOCKED_ON[o];
    }
}

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
        int p = task_effective_priority(i);
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
    unlink_held(m, m->owner); /* storage that is reused while its old holder still has it in its list */
    m->owner = -1;
    m->waiters = 0;
    m->next_held = 0;
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
            drop(m);
    }
    return m->owner >= 0;
}

/* Deadlock detection: would `task` wait for ever if it started to wait for m? Follow the chain "m's holder waits for that mutex, whose
 * holder waits for ...": if it comes back to `task`, the last wait would close a cycle. A link counts only while the holder really sleeps
 * on that mutex (its waiter bit is set; an unlock clears it) and is alive, so a thread that was just woken does not give a false alarm.
 * Interrupts off. */
static int deadlock_via(Mutex *m, int task)
{
    for (int depth = 0; m && depth < TASK_MAX; depth++)
    {
        int o = m->owner;
        if (o < 0 || o >= TASK_MAX || !alive(o))
            return 0;
        if (o == task)
            return 1;
        Mutex *next = BLOCKED_ON[o];
        if (!next || !(next->waiters & (1u << o)))
            return 0;
        m = next;
    }
    return 0;
}

int mutex_would_deadlock(Mutex *m, int task)
{
    unsigned long long irq = irq_save();
    int r = task >= 0 && task < TASK_MAX && deadlock_via(m, task);
    irq_restore(irq);
    return r;
}

static int lock_impl(Mutex *m, int detect)
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
        if (detect && deadlock_via(m, me))
        {
            mutex_cancel_wait(m, me);
            irq_restore(irq);
            return -1; /* waiting would close a cycle of threads that wait for each other */
        }
        mutex_wait_for(m, me); /* the holder borrows our priority from here on */
        task_block(); /* interrupts stay off from the check above to the switch: the unlock cannot slip in between */
    }
    take(m, me);
    mutex_cancel_wait(m, me); /* the main loop's task_block only yields: its bit may still be set */
    irq_restore(irq);
    return 0;
}

int mutex_lock(Mutex *m) { return lock_impl(m, 1); }

int mutex_relock(Mutex *m) { return lock_impl(m, 0); }

int mutex_trylock(Mutex *m)
{
    unsigned long long irq = irq_save();
    int ok = !is_locked(m);
    if (ok)
        take(m, task_current());
    irq_restore(irq);
    return ok ? 0 : -1;
}

/* The unlock itself, for mutex_unlock and cond_wait. Interrupts off, caller is the holder. The holder gives back what it borrowed,
 * except for what the waiters of the other mutexes it still holds ask for. */
static void release(Mutex *m)
{
    int o = m->owner;
    drop(m);
    wake_best(&m->waiters);
    if (o >= 0 && o < TASK_MAX)
        recompute(o);
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
    return mutex_relock(m);
}

/* The pieces of the waits above for the system call layer, which sleeps in its own loop so that a signal can end the wait. */
void mutex_wait_for(Mutex *m, int task)
{
    if (task < 0 || task >= TASK_MAX)
        return;
    m->waiters |= 1u << task;
    BLOCKED_ON[task] = m;
    propagate(m);
}

void mutex_cancel_wait(Mutex *m, int task)
{
    if (task < 0 || task >= TASK_MAX)
        return;
    m->waiters &= ~(1u << task);
    if (BLOCKED_ON[task] == m)
        BLOCKED_ON[task] = 0;
    propagate(m);
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
