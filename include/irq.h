/* ============================================================
 * HarisOS interrupt flag helpers
 *
 *   unsigned long long f = irq_save();   interrupts off, returns the old state
 *   ... code that the timer interrupt must not break in the middle ...
 *   irq_restore(f);                      back to what it was (stays off if it was off before: calls nest)
 *   irq_enable();                        interrupts on (a new preemptible thread starts with this)
 *
 * Host tests define HARIS_HOST: cli / sti are privileged and fault in a normal program, so the flag is a variable
 * that the test defines (volatile int host_if) and can look at.
 * ============================================================ */
#ifndef HARIS_IRQ_H
#define HARIS_IRQ_H

#define IRQ_FLAG_IF 0x200ULL /* the interrupt flag in RFLAGS */

#if defined(HARIS_HOST)

extern volatile int host_if;
static inline unsigned long long irq_save(void)
{
    unsigned long long f = host_if ? IRQ_FLAG_IF : 0;
    host_if = 0;
    return f;
}
static inline void irq_restore(unsigned long long f) { host_if = (f & IRQ_FLAG_IF) != 0; }
static inline void irq_enable(void) { host_if = 1; }

#else

static inline unsigned long long irq_save(void)
{
    unsigned long long f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(unsigned long long f)
{
    if (f & IRQ_FLAG_IF)
        __asm__ volatile("sti" : : : "memory");
}
static inline void irq_enable(void) { __asm__ volatile("sti" : : : "memory"); }

#endif

#endif
