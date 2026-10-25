/* ============================================================
 * HarisOS spinlock: protects data that an interrupt handler and a thread, or two processors, both touch
 *
 *   static Spinlock lock = SPINLOCK_INIT;
 *   unsigned long long f = spin_lock_irqsave(&lock);   interrupts off, then the lock is taken (waits while another processor holds it)
 *   ... short code: no yield, no sleep, no task_block, no mutex ...
 *   spin_unlock_irqrestore(&lock, f);                  the lock is let go, interrupts back to what they were (calls nest)
 *   spin_trylock_irqsave(&lock, &f);                   0 and the lock is taken, or -1 (busy; interrupts are as before)
 *
 * Interrupts off keep every other code of THIS processor from running while the lock is held, so a lock found taken by this very
 * processor means the caller takes it twice: a deadlock that spinning would never end. spin_lock_irqsave stops with a trap in that
 * case (it shows up as #UD on the crash screen), exactly like the other "cannot happen" checks of the scheduler. A lock held by
 * ANOTHER processor is just waited for (pause in a loop): that one lets go after its short section. The lock records who holds it
 * (the processor number, percpu.h) to tell the two cases apart.
 *
 * Use a Mutex (mutex.h) where the code may sleep or runs long; a spinlock is only for short sections, and the only lock an
 * interrupt handler may take.
 * ============================================================ */
#ifndef HARIS_SPINLOCK_H
#define HARIS_SPINLOCK_H

#include "irq.h"
#include "percpu.h"

typedef struct
{
    volatile int locked;
    volatile unsigned owner; /* processor number + 1 of the holder, 0 when free */
} Spinlock;

#define SPINLOCK_INIT { 0, 0 }

static inline void spin_init(Spinlock *l)
{
    l->owner = 0;
    l->locked = 0;
}

static inline int spin_trylock_irqsave(Spinlock *l, unsigned long long *flags)
{
    unsigned long long f = irq_save();
    if (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
    {
        irq_restore(f); /* it was taken: nothing changed */
        return -1;
    }
    l->owner = percpu_index() + 1;
    *flags = f;
    return 0;
}

static inline unsigned long long spin_lock_irqsave(Spinlock *l)
{
    unsigned long long f;
    while (spin_trylock_irqsave(l, &f) != 0)
    {
        if (l->owner == percpu_index() + 1)
            __builtin_trap(); /* the holder is this processor, which is the caller: spinning would be for ever */
#if defined(__x86_64__)
        __asm__ volatile("pause");
#endif
    }
    return f;
}

static inline void spin_unlock_irqrestore(Spinlock *l, unsigned long long flags)
{
    l->owner = 0;
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
}

static inline int spin_is_locked(const Spinlock *l) { return l->locked != 0; }

#endif
