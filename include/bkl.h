/* ============================================================
 * HarisOS big kernel lock (BKL): one lock that serialises all kernel code, so the kernel keeps working as if it had one processor.
 *
 * The kernel (scheduler, heap, page allocator, drivers, GUI ...) was written for a single processor: "interrupts off" meant "I am
 * alone". Instead of rewriting every module with its own lock, the processors take turns in the kernel:
 *
 *   state KERNEL   the processor runs kernel code and HOLDS the lock           (percpu idle = 0)
 *   state USER     the processor runs ring 3 code and does not hold it
 *   state IDLE     the processor waits in HLT and does not hold it             (percpu idle = 1)
 *
 * Every way from ring 3 into the kernel takes the lock and every way back releases it: the SYSCALL entry and SYSRET / IRETQ exits
 * (syscall.c), every interrupt and exception that arrives in ring 3 (idle.c, fault.c) and the first entry of a new process
 * (process.c). The lock belongs to the processor, not to a thread: a thread that is switched out inside the kernel and another one
 * that is switched in simply go on in the KERNEL state. A processor that sleeps (idle_wait, the idle loop of an application
 * processor) lets go before HLT; the interrupt that wakes it takes the lock again in its entry code and leaves it taken, because
 * the code it returns to is kernel code (see BKL_ISR_ENTER_IRQ). Interrupts are off whenever a processor waits for the lock.
 *
 * The cost: only one processor is in the kernel at a time, so user programs run in parallel but system calls do not. The gain: no
 * module needs to know about other processors. Individual locks can replace it one module at a time later.
 * ============================================================ */
#ifndef HARIS_BKL_H
#define HARIS_BKL_H

/* ---- for assembly (string literals to paste into an __asm__ block; they use the local labels 5 to 9, and no register) ---- */
#define BKL_ASM_LOCK                            \
    "    lock btsl $0, bkl_word(%rip)\n"        \
    "    jnc 9f\n"                              \
    "    lock incl bkl_waiters(%rip)\n"         \
    "8:  pause\n"                               \
    "    lock btsl $0, bkl_word(%rip)\n"        \
    "    jc 8b\n"                               \
    "    lock decl bkl_waiters(%rip)\n"         \
    "9:\n"
#define BKL_ASM_UNLOCK "    movl $0, bkl_word(%rip)\n"

/* An interrupt gate's entry, `cs` = where the saved CS is relative to RSP ("8" for an interrupt, "24" after fault_common's vector and error
 * code). From ring 3: swap in the kernel's GS and take the lock. From ring 0 (IRQ variant): if the processor was IDLE, take the lock and
 * become KERNEL; otherwise it holds the lock already. Before the per-CPU records exist (percpu_ready == 0) there is nothing to do. */
#define BKL_ISR_ENTER_FAULT(cs)    \
    "    testb $3, " cs "(%rsp)\n" \
    "    jz 7f\n"                  \
    "    swapgs\n" BKL_ASM_LOCK    \
    "7:\n"
#define BKL_ISR_ENTER_IRQ(cs)      \
    "    testb $3, " cs "(%rsp)\n" \
    "    jz 6f\n"                  \
    "    swapgs\n" BKL_ASM_LOCK    \
    "    jmp 7f\n"                 \
    "6:  cmpl $0, percpu_ready(%rip)\n" \
    "    je 7f\n"                  \
    "    cmpq $0, %gs:64\n"        \
    "    je 7f\n"                  \
    "    movq $0, %gs:64\n" BKL_ASM_LOCK \
    "7:\n"
/* The way out, back to the interrupted code: if that was ring 3, let go and swap the user's GS back. */
#define BKL_ISR_EXIT(cs)           \
    "    testb $3, " cs "(%rsp)\n" \
    "    jz 5f\n" BKL_ASM_UNLOCK   \
    "    swapgs\n"                 \
    "5:\n"

/* ---- for C ---- */
#if defined(__x86_64__) && !defined(HARIS_HOST)
extern volatile unsigned bkl_word;    /* fault.c: 1 while a processor is in the kernel */
extern volatile unsigned bkl_waiters; /* fault.c: processors waiting for the lock */

static inline void bkl_lock(void)
{
    if (!__atomic_exchange_n(&bkl_word, 1, __ATOMIC_ACQUIRE))
        return;
    __atomic_fetch_add(&bkl_waiters, 1, __ATOMIC_RELAXED);
    for (;;)
    {
        while (bkl_word)
            __asm__ volatile("pause");
        if (!__atomic_exchange_n(&bkl_word, 1, __ATOMIC_ACQUIRE))
            break;
    }
    __atomic_fetch_sub(&bkl_waiters, 1, __ATOMIC_RELAXED);
}

static inline void bkl_unlock(void) { __atomic_store_n(&bkl_word, 0, __ATOMIC_RELEASE); }

/* A yield point for kernel code that waits by looping (the main loop, task_yield in a loop): the lock is not fair, and a processor that
 * never leaves the kernel would keep the others out for ever. If somebody waits, let go until one of them has taken it. In the kernel,
 * interrupts off. */
static inline void bkl_yield(void)
{
    if (!bkl_waiters)
        return;
    bkl_unlock();
    for (int i = 0; i < 200000 && !bkl_word; i++)
        __asm__ volatile("pause");
    bkl_lock();
}
#else
static inline void bkl_yield(void) {}
#endif

#endif
