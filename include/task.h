/* ============================================================
 * HarisOS kernel threads (cooperative)
 *
 *   task_init();                          once, after the heap: the running code becomes task 0 ("MAIN")
 *   int id = task_create("NAME", fn, arg); a new thread that runs fn(arg); -1 if there is no slot or memory
 *   task_yield();                         give the CPU to the next ready thread (round robin)
 *   task_sleep_ms(10);                    other threads run for at least that long
 *   task_exit();                          end the calling thread (also happens when fn returns)
 *
 * "Cooperative" means a thread only changes when it calls task_yield / task_sleep_ms / task_exit; nothing takes
 * the CPU away (that is the next step: a timer interrupt that calls the scheduler). So the heap, the drivers
 * and the window manager need no locks yet, as long as no thread yields in the middle of using them.
 *
 * Task 0 is the main loop. It is never put to sleep or ended, so some thread can always run: there is no
 * idle thread yet (the main loop still halts the CPU itself, see idle.h).
 *
 * How a switch works (task_ctx.c): the callee-saved registers of the thread that leaves are pushed on its own stack,
 * the stack pointer is saved in its Task, the other thread's stack pointer is loaded and its registers are popped.
 * A new thread gets a made-up stack that looks exactly like a thread that was switched out, so it is started by the
 * same code.
 * ============================================================ */
#ifndef HARIS_TASK_H
#define HARIS_TASK_H

#define TASK_MAX 16
#define TASK_STACK_SIZE (32 * 1024)
#define TASK_NAME_MAX 16

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
void task_yield(void);
void task_sleep_ms(unsigned ms);
__attribute__((noreturn)) void task_exit(void);

int task_current(void); /* id of the running thread */
int task_count(void);   /* threads that are not finished */
TaskState task_state(int id);
const char *task_name(int id); /* NULL for an unused slot */
unsigned long long task_switches(void);

/* Quick check at boot: starts two threads that take turns, 0 = they did. Needs task_init and a working heap. */
int task_selftest(void);

/* ---- for task.c and task_ctx.c ---- */
/* Saves the callee-saved registers on the current stack, stores the stack pointer in *save_rsp, continues on
 * new_rsp. Returns (much later) when somebody switches back to the saved stack. */
void task_ctx_switch(unsigned long long *save_rsp, unsigned long long new_rsp);

#endif
