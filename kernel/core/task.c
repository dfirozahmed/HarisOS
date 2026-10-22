/* HarisOS kernel threads (cooperative): see include/task.h. */
#include "task.h"
#include "heap.h"
#include "idle.h"
#include "irq.h"
#include "preempt.h"
#include "timer.h"

typedef struct
{
    unsigned long long rsp; /* saved stack pointer while the thread is not running */
    unsigned char *stack;   /* kmalloc block, NULL for task 0 (it runs on the stack the firmware gave us) */
    TaskState state;
    unsigned flags;         /* TASK_PREEMPTIBLE */
    unsigned long long wake_us; /* TASK_SLEEPING: when to become ready */
    TaskFn fn;
    void *arg;
    char name[TASK_NAME_MAX];
} Task;

/* Written at the lowest address of every stack. The stack grows down towards it, so an overflow damages it first;
 * schedule() checks it whenever a thread leaves the CPU. Not as good as an unmapped guard page (that needs our own
 * page tables), but it turns "random corruption later" into a report at the next switch. */
#define STACK_CANARY 0x48617269734F5321ULL /* "HarisOS!" */

/* Interrupt rules. TASKS, CUR and SLICE_LEFT are changed by the scheduler, which the timer interrupt can also start
 * (task_tick). So every path into schedule() runs with interrupts off (irq_save), and so does everything that edits
 * the table (create, reap). A thread that is switched out keeps that state on its own stack and gets it back when
 * it is switched in again: it continues inside schedule(), and its caller then restores the flags it saved. */
static Task TASKS[TASK_MAX];
static int CUR;
static unsigned long long SWITCHES;
static unsigned SLICE_LEFT = TASK_SLICE_TICKS;
volatile int preempt_count; /* see preempt.h */

/* What task_ctx_switch pops, lowest address first: the saved xmm6-15, the eight pushed registers (the first one
 * popped is r15) and the return address it finally "returns" to. */
typedef struct
{
    unsigned long long xmm[20];
    unsigned long long r15, r14, r13, r12, rsi, rdi, rbx, rbp;
    unsigned long long ret;
} TaskInitFrame;

static void fill_zero(void *p, unsigned long long bytes)
{
    volatile unsigned char *b = (volatile unsigned char *)p; /* volatile: no call to memset in a freestanding build */
    for (unsigned long long i = 0; i < bytes; i++)
        b[i] = 0;
}

/* Frees the stacks of finished threads. Only safe from another thread's stack, never from the dying one: the
 * current thread is skipped (it cannot be DEAD here anyway). Interrupts must be off. */
static void reap(void)
{
    for (int i = 0; i < TASK_MAX; i++)
    {
        Task *t = &TASKS[i];
        if (t->state == TASK_DEAD && i != CUR)
        {
            kfree(t->stack);
            t->stack = 0;
            t->state = TASK_UNUSED;
        }
    }
}

/* Choose the next ready thread after the current one (round robin) and switch to it. Returns when the calling
 * thread is chosen again. Interrupts must be off. */
static void schedule(void)
{
    Task *old = &TASKS[CUR];
    unsigned long long now = timer_us();

    for (int i = 0; i < TASK_MAX; i++)
        if (TASKS[i].state == TASK_SLEEPING && (long long)(now - TASKS[i].wake_us) >= 0)
            TASKS[i].state = TASK_READY;
    if (old->state == TASK_RUNNING)
        old->state = TASK_READY;

    int next = -1;
    for (int i = 1; i <= TASK_MAX; i++) /* i == TASK_MAX comes back to the current thread */
    {
        int k = (CUR + i) % TASK_MAX;
        if (TASKS[k].state == TASK_READY)
        {
            next = k;
            break;
        }
    }
    if (next < 0)
        __builtin_trap(); /* cannot happen: task 0 never sleeps or ends, so it is always ready or running */

    if (next == CUR)
    {
        old->state = TASK_RUNNING;
        reap(); /* nobody else to run, but a thread that just ended can be cleaned up */
        return;
    }
    if (old->stack && *(unsigned long long *)old->stack != STACK_CANARY)
        __builtin_trap(); /* the leaving thread overflowed its stack: shows up as #UD on the crash screen */

    TASKS[next].state = TASK_RUNNING;
    CUR = next;
    SWITCHES++;
    SLICE_LEFT = TASK_SLICE_TICKS; /* the thread that comes in gets a whole slice */
    task_ctx_switch(&old->rsp, TASKS[next].rsp);
    reap(); /* we are on the stack of whichever thread was chosen to continue */
}

/* The first code of every new thread: task_ctx_switch "returns" here, with interrupts off (the switch ran inside
 * schedule()). */
static void task_main(void)
{
    reap(); /* a thread that came here instead of returning from schedule() must free dead threads too */
    if (TASKS[CUR].flags & TASK_PREEMPTIBLE)
        irq_enable(); /* from here on the timer may take the CPU away */
    TASKS[CUR].fn(TASKS[CUR].arg);
    task_exit();
}

int task_init(void)
{
    for (int i = 0; i < TASK_MAX; i++)
    {
        TASKS[i].state = TASK_UNUSED;
        TASKS[i].stack = 0;
        TASKS[i].flags = 0;
    }
    SLICE_LEFT = TASK_SLICE_TICKS;
    preempt_count = 0;
    Task *t = &TASKS[0];
    t->state = TASK_RUNNING;
    t->name[0] = 'M';
    t->name[1] = 'A';
    t->name[2] = 'I';
    t->name[3] = 'N';
    t->name[4] = 0;
    CUR = 0;
    SWITCHES = 0;
    return 0;
}

int task_create(const char *name, TaskFn fn, void *arg) { return task_create_flags(name, fn, arg, 0); }

int task_create_flags(const char *name, TaskFn fn, void *arg, unsigned flags)
{
    if (!fn)
        return -1;
    unsigned char *stack = kmalloc(TASK_STACK_SIZE); /* before the table is touched: the heap has its own lock */
    if (!stack)
        return -1;
    *(unsigned long long *)stack = STACK_CANARY;

    unsigned long long irq = irq_save(); /* the slot search and the fill below must not be interleaved with another create */
    reap();
    int id = -1;
    for (int i = 1; i < TASK_MAX; i++) /* slot 0 is the main thread */
        if (TASKS[i].state == TASK_UNUSED)
        {
            id = i;
            break;
        }
    if (id < 0)
    {
        irq_restore(irq);
        kfree(stack);
        return -1;
    }

    /* Layout at the top of the stack (top is 16-aligned), highest address first:
     *   top-8 .. top-40  32 bytes of "shadow space" the Microsoft ABI gives every callee above its return address
     *   top-40           fake return address of task_main (it never returns: it ends in task_exit)
     *   top-48           ret slot of the frame below = task_main's address
     * task_main starts as if it had been called: RSP = top-40, which is 8 mod 16 like at any function entry. */
    unsigned long long top = ((unsigned long long)stack + TASK_STACK_SIZE) & ~15ULL;
    unsigned long long *ret_slot = (unsigned long long *)(top - 48);
    TaskInitFrame *fr = (TaskInitFrame *)((unsigned char *)ret_slot - __builtin_offsetof(TaskInitFrame, ret));
    fill_zero(fr, (unsigned long long)((unsigned char *)(top) - (unsigned char *)fr));
    fr->ret = (unsigned long long)task_main;

    Task *t = &TASKS[id];
    t->stack = stack;
    t->rsp = (unsigned long long)fr;
    t->fn = fn;
    t->arg = arg;
    t->flags = flags;
    t->wake_us = 0;
    int n = 0;
    for (; name && name[n] && n < TASK_NAME_MAX - 1; n++)
        t->name[n] = name[n];
    t->name[n] = 0;
    t->state = TASK_READY; /* last: from now on the scheduler may pick it */
    irq_restore(irq);
    return id;
}

void task_yield(void)
{
    unsigned long long irq = irq_save();
    schedule();
    irq_restore(irq);
}

void task_sleep_ms(unsigned ms)
{
    if (CUR == 0) /* the main loop never sleeps: it is what runs when everybody else does */
    {
        task_yield();
        return;
    }
    unsigned long long irq = irq_save();
    TASKS[CUR].wake_us = timer_us() + (unsigned long long)ms * 1000;
    TASKS[CUR].state = TASK_SLEEPING;
    schedule();
    irq_restore(irq);
}

__attribute__((noreturn)) void task_exit(void)
{
    if (CUR == 0)
        __builtin_trap(); /* ending the main loop would leave nothing to run */
    irq_save(); /* never restored: this thread does not continue */
    TASKS[CUR].state = TASK_DEAD;
    schedule(); /* never comes back: a dead thread is never chosen */
    for (;;)
        __builtin_trap();
}

void task_tick(void)
{
    Task *t = &TASKS[CUR];
    if (!(t->flags & TASK_PREEMPTIBLE) || t->state != TASK_RUNNING || preempt_count > 0)
        return;
    if (--SLICE_LEFT > 0)
        return;
    SLICE_LEFT = TASK_SLICE_TICKS;
    schedule(); /* we are in the interrupt: interrupts are off */
}

int task_others_ready(void)
{
    unsigned long long irq = irq_save();
    unsigned long long now = timer_us();
    int found = 0;
    for (int i = 0; i < TASK_MAX && !found; i++)
    {
        if (i == CUR)
            continue;
        if (TASKS[i].state == TASK_READY ||
            (TASKS[i].state == TASK_SLEEPING && (long long)(now - TASKS[i].wake_us) >= 0))
            found = 1;
    }
    irq_restore(irq);
    return found;
}

int task_current(void) { return CUR; }

int task_count(void)
{
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (TASKS[i].state != TASK_UNUSED && TASKS[i].state != TASK_DEAD)
            n++;
    return n;
}

TaskState task_state(int id) { return id >= 0 && id < TASK_MAX ? TASKS[id].state : TASK_UNUSED; }

const char *task_name(int id)
{
    return id >= 0 && id < TASK_MAX && TASKS[id].state != TASK_UNUSED ? TASKS[id].name : 0;
}

unsigned long long task_switches(void) { return SWITCHES; }

/* ---- boot self test ----------------------------------------- */
static void selftest_worker(void *arg)
{
    volatile int *counter = arg;
    for (int i = 0; i < 3; i++)
    {
        (*counter)++;
        task_yield();
    }
}

int task_selftest(void)
{
    volatile int a = 0, b = 0;
    int ta = task_create("TEST A", selftest_worker, (void *)&a);
    int tb = task_create("TEST B", selftest_worker, (void *)&b);
    if (ta < 0 || tb < 0)
        return -1;

    task_yield(); /* A runs once and yields, B runs once and yields, back to us: they take turns */
    if (a != 1 || b != 1)
        return -2;

    for (int i = 0; i < 20 && task_count() > 1; i++)
        task_yield();
    if (a != 3 || b != 3 || task_count() != 1)
        return -3;
    task_yield(); /* the last dead thread's stack is freed by the next switch */
    if (task_state(ta) != TASK_UNUSED || task_state(tb) != TASK_UNUSED)
        return -4;
    return 0;
}

/* A thread that never yields. Only the timer interrupt can take the CPU from it. It stops by itself at its deadline, so a
 * broken tick makes the test fail instead of hanging the boot. */
static volatile unsigned long long SPIN_COUNT;
static volatile int SPIN_DONE;
static unsigned long long SPIN_DEADLINE;

static void spin_worker(void *arg)
{
    (void)arg;
    while ((long long)(timer_us() - SPIN_DEADLINE) < 0)
        SPIN_COUNT++;
    SPIN_DONE = 1;
}

int task_selftest_preempt(void)
{
    if (!idle_ready() || !timer_ready())
        return 0; /* no tick, so there is nothing to preempt with: threads stay cooperative */

    SPIN_COUNT = 0;
    SPIN_DONE = 0;
    SPIN_DEADLINE = timer_us() + 20000;
    if (task_create_flags("SPIN", spin_worker, 0, TASK_PREEMPTIBLE) < 0)
        return -1;

    int preempted = 0;
    for (int i = 0; i < 400 && !SPIN_DONE; i++)
    {
        task_yield(); /* the spinner runs; we are back because its slice ended (timer) or because it finished */
        if (!SPIN_DONE)
            preempted++;
    }
    task_yield(); /* frees the stack of the finished spinner */
    if (!SPIN_DONE)
        return -2;
    if (preempted < 2) /* 20 ms of spinning is several slices: coming back only at the end means no preemption */
        return -3;
    if (SPIN_COUNT == 0)
        return -4;
    return 0;
}
