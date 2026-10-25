/* ============================================================
 * HarisOS preemption lock
 *
 *   preempt_disable();    the timer interrupt will not switch threads until the matching preempt_enable()
 *   ... touch data that another thread might be using ...
 *   preempt_enable();
 *
 * A preemptible thread (see task.h) can be stopped by the timer between any two instructions. If that happens in the
 * middle of changing a shared structure (the heap's free list, say) and another thread then uses the same structure,
 * it is damaged. Code that must not be interrupted like that can be bracketed with this pair. The calls nest. Rules: keep the section short, and never yield or sleep inside it.
 *
 * Interrupts themselves stay on, so the clock still ticks and nothing is lost; the tick just does not switch.
 *
 * SMP: the counter is one global and keeps only this processor honest while the big kernel lock (bkl.h) is held; it does NOT stop
 * another processor. Shared data that is used without the big kernel lock needs a spinlock (spinlock.h) instead: the page allocator and
 * the heap have theirs, and no longer use this pair.
 * ============================================================ */
#ifndef HARIS_PREEMPT_H
#define HARIS_PREEMPT_H

extern volatile int preempt_count; /* defined in task.c; 0 = threads may be preempted */

static inline void preempt_disable(void)
{
    preempt_count++;
    __asm__ volatile("" : : : "memory"); /* the section's accesses must not move above this */
}

static inline void preempt_enable(void)
{
    __asm__ volatile("" : : : "memory"); /* ... or below this */
    preempt_count--;
}

#endif
