/* Kernel threads: boot self test. */
#include "task_internal.h"

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

/* A thread that never yields. Only the timer interrupt can take the CPU from it. It stops when the test tells it to
 * (SPIN_STOP), or at a generous deadline, so a broken tick makes the test fail instead of hanging the boot. The test
 * does not depend on how fast the machine is: it waits for a number of preemptions, not for a number of milliseconds
 * (an emulator on a busy host can be very slow). */
#define SPIN_NEEDED 3        /* preemptions that prove the timer works */
#define SPIN_DEADLINE_US 500000
static volatile unsigned long long SPIN_COUNT;
static volatile int SPIN_DONE, SPIN_STOP;
static unsigned long long SPIN_DEADLINE;

static void spin_worker(void *arg)
{
    (void)arg;
    while (!SPIN_STOP && (long long)(timer_us() - SPIN_DEADLINE) < 0)
        SPIN_COUNT++;
    SPIN_DONE = 1;
}

int task_selftest_preempt(void)
{
    if (!idle_ready() || !timer_ready())
        return 0; /* no tick, so there is nothing to preempt with: threads stay cooperative */

    SPIN_COUNT = 0;
    SPIN_DONE = 0;
    SPIN_STOP = 0;
    SPIN_DEADLINE = timer_us() + SPIN_DEADLINE_US;
    if (task_create_flags("SPIN", spin_worker, 0, TASK_PREEMPTIBLE) < 0)
        return -1;

    int preempted = 0;
    for (int i = 0; i < 400 && !SPIN_DONE; i++)
    {
        task_yield(); /* the spinner runs; we are back because its slice ended (timer) or because it finished */
        if (!SPIN_DONE && ++preempted >= SPIN_NEEDED)
            SPIN_STOP = 1; /* enough proof: let it end at its next turn */
    }
    task_yield(); /* frees the stack of the finished spinner */
    if (!SPIN_DONE)
        return -2;
    if (preempted < SPIN_NEEDED) /* it ran to its deadline: the timer never took the CPU from it */
        return -3;
    if (SPIN_COUNT == 0)
        return -4;
    return 0;
}
