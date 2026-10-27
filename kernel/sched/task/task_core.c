/* Kernel threads: the table, the per processor state and the scheduler lock. */
#include "task_internal.h"

/* HarisOS kernel threads (cooperative): see include/task.h. */
TaskChunk *DIR[TASK_LIMIT / TASK_CHUNK];
int CAP;
/* Processors (smp.h). All of this is kernel code under the big kernel lock (bkl.h), so one scheduler with one table serves every processor;
 * what belongs to a processor is kept per processor: the running thread, its slice, the address space loaded in its CR3 and whether its
 * floating point unit holds a process's registers. CUR & co. below are those, for the processor this code runs on. A thread can move to
 * another processor whenever it is switched out, so these must be read again after task_ctx_switch returns (the macros do, being
 * re-evaluated at every use). Processor 0 is the boot processor and runs the main loop (thread 0). An application processor runs only
 * user threads and, when there is none, its own idle context (task_register_idle). */
int CURS[SMP_MAX_CPUS];
unsigned SLICES[SMP_MAX_CPUS];
AddressSpace *ACTIVE[SMP_MAX_CPUS];
int FPU_HOLDS[SMP_MAX_CPUS];
int IDLE_ID[SMP_MAX_CPUS]; /* the idle context of application processor n, 0 = none yet */
int AP_READY;      /* application processors that have registered their idle context (task_register_idle) */
int BOOT_RESERVED; /* 1 = the boot processor runs no user thread while an application processor can (task_reserve_boot_cpu) */
unsigned long long SWITCHES;
unsigned long long CPU_TICKS_ALL[SMP_MAX_CPUS], CPU_TICKS_BUSY[SMP_MAX_CPUS]; /* per processor, for task_cpu_ticks */
unsigned long long TICKS; /* timer interrupts seen by task_tick, for the boot log */
volatile int preempt_count; /* see preempt.h */

/* The scheduler lock. It protects the table of threads (every field the scheduler or a waker reads or writes: state, priorities, wake time,
 * address space, the slot allocation, CAP / DIR), CURS, IDLE_ID and the counters, so that code that runs OUTSIDE the big kernel lock can
 * wake a thread, change a priority, create or kill one while the processor that holds the big kernel lock is in the scheduler.
 *
 *   - Interrupts are off whenever it is held (every path into it already runs with them off), so an interrupt handler of the same processor
 *     never finds it taken by the code it interrupted. A processor that finds it taken by itself traps, like the spinlock.
 *   - It is held ACROSS the context switch: schedule_locked() takes it before it looks at the table and the thread that comes out of
 *     task_ctx_switch (a resumed one, in schedule_locked, or a new one, in task_main) lets go. Until then the thread that left has not yet saved its
 *     stack pointer, and nobody else may pick it. The lock belongs to the processor, like the big kernel lock (bkl.h).
 *   - Nothing that can call back into the task functions runs under it: the thread's free_data, aspace_destroy and the like run in reap()
 *     after the slot has been detached.
 *   - Order of locks: big kernel lock, then this one, then the page allocator's and the heap's. This one is never held while waiting for the
 *     big kernel lock.
 *   - Only the code that does not switch threads may run without the big kernel lock: task_wake, task_set_priority, task_set_inherited,
 *     task_kill, task_create and the like. schedule() and everything that ends in it (block, sleep, yield, exit, the tick) still need it:
 *     the thread that comes in expects to be in the kernel with the big kernel lock held. */
volatile int SCHED_LOCKED;
volatile unsigned SCHED_OWNER; /* processor number + 1 of the holder, 0 when free */
