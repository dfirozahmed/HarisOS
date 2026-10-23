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

typedef void (*TaskFn)(void *arg);

typedef enum
{
    TASK_UNUSED = 0,
    TASK_READY,    /* could run, waits for its turn */
    TASK_RUNNING,  /* the one running now */
    TASK_SLEEPING, /* waits for wake_us */
    TASK_DEAD      /* finished; its stack is freed by the next thread that runs */
} TaskState;

int task_init(void);
int task_create(const char *name, TaskFn fn, void *arg);
int task_create_flags(const char *name, TaskFn fn, void *arg, unsigned flags);
void task_yield(void);

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
