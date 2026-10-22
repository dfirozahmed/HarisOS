/* HarisOS kernel threads (cooperative): see include/task.h. */
#include "task.h"
#include "heap.h"
#include "timer.h"

typedef struct
{
    unsigned long long rsp; /* saved stack pointer while the thread is not running */
    unsigned char *stack;   /* kmalloc block, NULL for task 0 (it runs on the stack the firmware gave us) */
    TaskState state;
    unsigned long long wake_us; /* TASK_SLEEPING: when to become ready */
    TaskFn fn;
    void *arg;
    char name[TASK_NAME_MAX];
} Task;

/* Written at the lowest address of every stack. The stack grows down towards it, so an overflow damages it first;
 * schedule() checks it whenever a thread leaves the CPU. Not as good as an unmapped guard page (that needs our own
 * page tables), but it turns "random corruption later" into a report at the next switch. */
#define STACK_CANARY 0x48617269734F5321ULL /* "HarisOS!" */

static Task TASKS[TASK_MAX];
static int CUR;
static unsigned long long SWITCHES;

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
 * current thread is skipped (it cannot be DEAD here anyway). */
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
 * thread is chosen again. */
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
    task_ctx_switch(&old->rsp, TASKS[next].rsp);
    reap(); /* we are on the stack of whichever thread was chosen to continue */
}

/* The first code of every new thread: task_ctx_switch "returns" here. */
static void task_main(void)
{
    reap(); /* a thread that came here instead of returning from schedule() must free dead threads too */
    TASKS[CUR].fn(TASKS[CUR].arg);
    task_exit();
}

int task_init(void)
{
    for (int i = 0; i < TASK_MAX; i++)
    {
        TASKS[i].state = TASK_UNUSED;
        TASKS[i].stack = 0;
    }
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

int task_create(const char *name, TaskFn fn, void *arg)
{
    if (!fn)
        return -1;
    reap();
    int id = -1;
    for (int i = 1; i < TASK_MAX; i++) /* slot 0 is the main thread */
        if (TASKS[i].state == TASK_UNUSED)
        {
            id = i;
            break;
        }
    if (id < 0)
        return -1;

    unsigned char *stack = kmalloc(TASK_STACK_SIZE);
    if (!stack)
        return -1;
    *(unsigned long long *)stack = STACK_CANARY;

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
    t->wake_us = 0;
    int n = 0;
    for (; name && name[n] && n < TASK_NAME_MAX - 1; n++)
        t->name[n] = name[n];
    t->name[n] = 0;
    t->state = TASK_READY;
    return id;
}

void task_yield(void) { schedule(); }

void task_sleep_ms(unsigned ms)
{
    if (CUR == 0) /* the main loop never sleeps: it is what runs when everybody else does */
    {
        schedule();
        return;
    }
    TASKS[CUR].wake_us = timer_us() + (unsigned long long)ms * 1000;
    TASKS[CUR].state = TASK_SLEEPING;
    schedule();
}

__attribute__((noreturn)) void task_exit(void)
{
    if (CUR == 0)
        __builtin_trap(); /* ending the main loop would leave nothing to run */
    TASKS[CUR].state = TASK_DEAD;
    schedule(); /* never comes back: a dead thread is never chosen */
    for (;;)
        __builtin_trap();
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
