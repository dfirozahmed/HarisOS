/* ============================================================
 * HarisOS kernel threads (cooperative)
 *
 *   task_init();                          once, after the heap: the running code becomes task 0 ("MAIN")
 *   int id = task_create("NAME", fn, arg); a new thread that runs fn(arg); -1 if there is no slot or memory
 *   task_yield();                         give the CPU to the next ready thread (round robin)
 *   task_sleep_ms(10);                    other threads run for at least that long
 *   task_exit();                          end the calling thread (also happens when fn returns)
 *
 * Two kinds of threads:
 *   normal       (task_create)            changes only when it calls task_yield / task_sleep_ms / task_exit. Runs
 *                                         with interrupts off, like the main loop. Needs no locks.
 *   preemptible  (TASK_PREEMPTIBLE flag)  the 1 ms timer interrupt (task_tick) takes the CPU away after
 *                                         TASK_SLICE_TICKS ticks, so a thread that never yields cannot hang the
 *                                         system. It may be stopped anywhere, so it may only use code that is
 *                                         safe for that: its own data, the kernel heap (it brackets itself with
 *                                         preempt_disable, see preempt.h), and its own locks. NOT the window
 *                                         manager, graphics or the drivers, which belong to the main loop.
 *
 * Task 0 is the main loop. It is never preempted, never put to sleep and never ended, so the GUI code needs no
 * locks and some thread can always run (there is no idle thread: the main loop halts the CPU itself, see idle.h).
 * Preemptible threads therefore get the CPU when the main loop calls task_yield (it does that while
 * task_others_ready() says someone wants to run), and give it back by the timer or by yielding.
 *
 * How a switch works (task_ctx.c): the callee-saved registers of the thread that leaves are pushed on its own stack,
 * the stack pointer is saved in its Task, the other thread's stack pointer is loaded and its registers are popped.
 * A new thread gets a made-up stack that looks exactly like a thread that was switched out, so it is started by the
 * same code.
 * ============================================================ */
#ifndef HARIS_TASK_H
#define HARIS_TASK_H

#define TASK_MAX 16
#define TASK_STACK_SIZE (32 * 1024) /* usable bytes; a 4 KiB guard page (unmapped) sits below, see paging.h */
#define TASK_NAME_MAX 16

#define TASK_PREEMPTIBLE 1  /* flag for task_create_flags */
#define TASK_SLICE_TICKS 5  /* a preemptible thread runs this many 1 ms timer ticks before the next thread gets a turn */

/* Priorities: the scheduler gives the CPU to the READY thread with the highest priority; equal ones take turns (round robin).
 * A thread that is READY but passed over TASK_AGE_PASSES times gets one level of boost, so a busy high-priority thread cannot
 * starve the others for ever (the boost is lost when it runs). TASK_AGE_PASSES >= TASK_MAX keeps plain round robin exact among
 * threads of equal priority: none of them waits that long. */
#define TASK_PRIO_MIN 0
#define TASK_PRIO_DEFAULT 2 /* every new thread, and the main loop */
#define TASK_PRIO_MAX 4
#define TASK_AGE_PASSES 16

struct AddressSpace; /* aspace.h */

typedef void (*TaskFn)(void *arg);
typedef void (*TaskFreeFn)(void *data);

typedef enum
{
    TASK_UNUSED = 0,
    TASK_READY,    /* could run, waits for its turn */
    TASK_RUNNING,  /* the one running now */
    TASK_SLEEPING, /* waits for wake_us */
    TASK_DEAD,     /* finished; its stack is freed by the next thread that runs */
    TASK_BLOCKED   /* waits for an event: not scheduled until task_wake() (appended last: other values keep their numbers) */
} TaskState;

int task_init(void);
int task_create(const char *name, TaskFn fn, void *arg);
int task_create_flags(const char *name, TaskFn fn, void *arg, unsigned flags);
/* A thread that belongs to a user process: it runs (and its ring 3 code runs) in the address space `as`, which the
 * scheduler loads on every switch to it, and its kernel stack becomes the one the CPU uses when that code traps. It is
 * preemptible. The space is freed together with the thread (reap). The thread's function normally ends by entering ring
 * 3 (process.c); it never returns there. -1 if as is NULL or there is no slot or memory (as is then still the caller's). */
int task_create_user(const char *name, TaskFn fn, void *arg, struct AddressSpace *as, void *data, TaskFreeFn free_data);
struct AddressSpace *task_address_space(void); /* the running thread's user space, NULL for a kernel thread */
/* `data` is the process's own record (process.h): task_user_data() gives it back to code running in that thread (the
 * system calls), and free_data(data) is called when the thread has been cleaned up. NULL for kernel threads. */
void *task_user_data(void);

/* Wait for an event without being scheduled: task_block() marks the running thread BLOCKED and switches away; task_wake(id)
 * (from anywhere: the main loop, another thread) makes it READY again. The caller checks its condition first and calls
 * task_block() in a loop, because a wake-up can be spurious. Interrupts must stay off between the check and task_block()
 * (system calls run that way), or the event could slip in between and the wake-up be lost. The main loop (thread 0) never
 * blocks: task_block() there only yields. */
void task_block(void);
void task_wake(int id);
struct AddressSpace *task_space_of(int id);    /* the user space of thread id; NULL for kernel threads and ended ones */
void *task_data_of(int id);                     /* the process record (see task_user_data) of thread id; NULL for kernel threads, ended and unused ones */
/* End another thread at once (a signal, see process_signal): it is marked DEAD and the next reap frees its stack, pages and
 * record. Only for a thread that is not running (not the caller) and is not the main thread. 0, or -1. Whatever the thread was
 * doing is thrown away: it may only be in ring 3, or blocked in a system call, never inside a kernel lock (system calls run
 * with interrupts off and nothing is held while a thread waits). */
int task_kill(int id);

/* exec: the running process thread gets a new address space (already built, with the new program in it) and the name of the new
 * program; its old space is freed and the floating point state starts clean. The caller then enters ring 3 in the new space
 * and never returns to the old code. Interrupts must be off (a system call). */
void task_exec_replace(struct AddressSpace *as, const char *name);
void task_yield(void);

/* Change / read the priority (TASK_PRIO_MIN..TASK_PRIO_MAX, higher runs first). It takes effect at the next scheduling decision:
 * a running thread is not cut short when a better one becomes ready, it keeps the CPU until it yields, blocks or its slice ends.
 * task_set_priority: 0, or -1 for a bad id / value or a slot that is unused or finished. task_priority: -1 for those ids. */
int task_set_priority(int id, int prio);
int task_priority(int id);
/* Priority inheritance (mutex.c uses it): a thread that holds a lock somebody better waits for runs at that waiter's priority until it
 * lets go. task_set_inherited sets the borrowed priority (TASK_PRIO_MIN..TASK_PRIO_MAX), or -1 for none; the scheduler uses the higher of
 * it and the thread's own. 0, or -1 for a bad id / value or a slot that is unused or finished. task_effective_priority is that higher
 * value (-1 for those ids); task_priority stays the thread's own. */
int task_set_inherited(int id, int prio);
int task_effective_priority(int id);

/* Called by the timer interrupt (idle.c) on every tick, with interrupts off: switches away from a preemptible thread
 * whose slice is over, unless preempt_disable() is in force. Does nothing for normal threads and the main loop. */
void task_tick(void);
/* 1 if some thread other than the running one could run now (the main loop yields instead of halting then). */
int task_others_ready(void);
void task_sleep_ms(unsigned ms);
__attribute__((noreturn)) void task_exit(void);

int task_current(void); /* id of the running thread */
int task_count(void);   /* threads that are not finished */
TaskState task_state(int id);
const char *task_name(int id); /* NULL for an unused slot */
unsigned long long task_switches(void);
unsigned long long task_ticks(void); /* how many timer interrupts task_tick() has seen (a boot log diagnostic) */
/* The name of the thread whose stack guard page contains addr (a page fault with CR2 = addr means that thread's stack
 * overflowed), or NULL. For the crash screen. */
const char *task_guard_hit(unsigned long long addr);

/* Quick check at boot: starts two threads that take turns, 0 = they did. Needs task_init and a working heap. */
int task_selftest(void);
/* Starts a preemptible thread that never yields and checks that the timer interrupt takes the CPU away from it.
 * 0 = it did (three times). Needs the LAPIC tick (idle_ready()). Never hangs: the thread stops by itself after
 * half a second if the timer does not work, and the result is then -3. */
int task_selftest_preempt(void);

/* ---- for task.c and task_ctx.c ---- */
/* Saves the callee-saved registers on the current stack, stores the stack pointer in *save_rsp, continues on
 * new_rsp. Returns (much later) when somebody switches back to the saved stack. */
void task_ctx_switch(unsigned long long *save_rsp, unsigned long long new_rsp);

#endif
