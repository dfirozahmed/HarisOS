/* ============================================================
 * HarisOS spinlock: protects data that an interrupt handler and a thread both touch
 *
 *   static Spinlock lock = SPINLOCK_INIT;
 *   unsigned long long f = spin_lock_irqsave(&lock);   interrupts off, then the lock is taken
 *   ... short code: no yield, no sleep, no task_block, no mutex ...
 *   spin_unlock_irqrestore(&lock, f);                  the lock is let go, interrupts back to what they were (calls nest)
 *   spin_trylock_irqsave(&lock, &f);                   0 and the lock is taken, or -1 (busy; interrupts are as before)
 *
 * HarisOS runs on one CPU. Interrupts off already keeps every other code from running, so the lock is held only by the code that
 * runs: finding it taken can only mean the caller (or code it interrupted before it turned interrupts off, which cannot happen
 * with this order) takes it twice, a deadlock that spinning would never end. spin_lock_irqsave therefore stops with a trap instead
 * of spinning (it shows up as #UD on the crash screen), exactly like the other "cannot happen" checks of the scheduler. The
 * test-and-set is a real atomic exchange, so the lock keeps its meaning if another CPU is ever started.
 *
 * Use a Mutex (mutex.h) where the code may sleep or runs long; a spinlock is only for short sections, and the only lock an
 * interrupt handler may take.
 * ============================================================ */
#ifndef HARIS_SPINLOCK_H
#define HARIS_SPINLOCK_H

#include "irq.h"

typedef struct
{
    volatile int locked;
} Spinlock;

#define SPINLOCK_INIT { 0 }

static inline void spin_init(Spinlock *l) { l->locked = 0; }

static inline int spin_trylock_irqsave(Spinlock *l, unsigned long long *flags)
{
    unsigned long long f = irq_save();
    if (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
    {
        irq_restore(f); /* it was taken: nothing changed */
        return -1;
    }
    *flags = f;
    return 0;
}

static inline unsigned long long spin_lock_irqsave(Spinlock *l)
{
    unsigned long long f;
    if (spin_trylock_irqsave(l, &f) != 0)
        __builtin_trap(); /* one CPU: the holder is the caller, spinning would be for ever */
    return f;
}

static inline void spin_unlock_irqrestore(Spinlock *l, unsigned long long flags)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
}

static inline int spin_is_locked(const Spinlock *l) { return l->locked != 0; }

#endif
