/* Host test for the kernel mutex and condition variable (kernel/core/mutex.c) on the real scheduler (task.c, task_ctx.c):
 * the threads really block, are woken and switch stacks. The platform pieces task.c needs are replaced by stubs, like in
 * test_task.c. */
#include "mutex.h"
#include "task.h"
#include "aspace.h"
#include "fpu.h"
#include "irq.h"
#include "syscall.h"
#include "pmm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <malloc.h>
#define ALIGNED_ALLOC(a, s) _aligned_malloc((s), (a))
#define ALIGNED_FREE(p) _aligned_free(p)
#else
#define ALIGNED_ALLOC(a, s) aligned_alloc((a), (s))
#define ALIGNED_FREE(p) free(p)
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* the threads of a set as a bit mask (these tests only use small ids) */
static unsigned mask_of(const TaskSet *s)
{
    unsigned m = 0;
    for (int i = taskset_next(s, 0); i >= 0 && i < 32; i = taskset_next(s, i + 1))
        m |= 1u << i;
    return m;
}

/* The kernel heap (the table of threads and the sets of sleepers use it): host memory, counted, with a switch that makes it fail. */
static int HEAP_LIVE, HEAP_FAIL;
void *kmalloc(unsigned long long size)
{
    if (HEAP_FAIL)
        return 0;
    void *p = calloc(1, size ? (size_t)size : 1);
    if (p)
        HEAP_LIVE++;
    return p;
}
void kfree(void *p)
{
    if (p)
    {
        free(p);
        HEAP_LIVE--;
    }
}
void *krealloc(void *p, unsigned long long size)
{
    if (HEAP_FAIL)
        return 0;
    void *n = realloc(p, size ? (size_t)size : 1);
    if (n && !p)
        HEAP_LIVE++;
    return n;
}

volatile int host_if = 1;
static int BLOCKS_OUT;

unsigned long long pmm_alloc_contiguous_tag(unsigned long long count, PmmTag tag)
{
    (void)tag;
    BLOCKS_OUT++;
    return (unsigned long long)ALIGNED_ALLOC(4096, count * 4096);
}
void pmm_free_contiguous(unsigned long long phys, unsigned long long count)
{
    (void)count;
    BLOCKS_OUT--;
    ALIGNED_FREE((void *)phys);
}
void fpu_init(void) {}
void fpu_save(void *state) { (void)state; }
void fpu_restore(const void *state) { (void)state; }
void fpu_reset_control(void) {}
void fpu_default(void *state) { (void)state; }
void aspace_switch(AddressSpace *as) { (void)as; }
void aspace_destroy(AddressSpace *as) { (void)as; }
void syscall_set_kernel_stack(unsigned long long top) { (void)top; }
int paging_set_present(unsigned long long virt, int present)
{
    (void)virt;
    (void)present;
    return 0;
}
unsigned long long timer_us(void) { return 1000000; }
int timer_ready(void) { return 1; }
int idle_ready(void) { return 1; }

/* workers must not call printf: only plain code runs on their small stacks */
static int LOG[64];
static int LOGN;
static void logv(int v)
{
    if (LOGN < 64)
        LOG[LOGN++] = v;
}

static Mutex M = MUTEX_INIT;
static CondVar CV = COND_INIT;
static volatile int FLAG;

/* lock, note the turn, unlock */
static void lock_worker(void *arg)
{
    mutex_lock(&M);
    logv((int)(long long)arg);
    mutex_unlock(&M);
}

/* the textbook condition wait: re-check in a loop */
static void wait_worker(void *arg)
{
    mutex_lock(&M);
    while (!FLAG)
        cond_wait(&CV, &M);
    logv((int)(long long)arg);
    mutex_unlock(&M);
}

static volatile int UNLOCK_RESULT;
static void foreign_unlock_worker(void *arg)
{
    (void)arg;
    UNLOCK_RESULT = mutex_unlock(&M);
}

/* ---- priority inheritance ---- */
static Mutex M1 = MUTEX_INIT, M2 = MUTEX_INIT;

/* holds M, sleeps until the test wakes it, then unlocks and notes the priority it runs at afterwards */
static void sleeper_holder(void *arg)
{
    (void)arg;
    mutex_lock(&M);
    task_block();
    mutex_unlock(&M);
    logv(task_effective_priority(task_current()));
}

/* the same with two mutexes: lock M1, lock M2, sleep, unlock M1 (note), unlock M2 (note) */
static void two_holder(void *arg)
{
    (void)arg;
    mutex_lock(&M1);
    mutex_lock(&M2);
    task_block();
    mutex_unlock(&M1);
    logv(task_effective_priority(task_current()));
    mutex_unlock(&M2);
    logv(task_effective_priority(task_current()));
}

static void lock1_worker(void *arg)
{
    mutex_lock(&M1);
    logv((int)(long long)arg);
    mutex_unlock(&M1);
}
static void lock2_worker(void *arg)
{
    mutex_lock(&M2);
    logv((int)(long long)arg);
    mutex_unlock(&M2);
}

/* holds M2, then waits for M1 (a link in a chain) */
static void chain_worker(void *arg)
{
    (void)arg;
    mutex_lock(&M2);
    mutex_lock(&M1);
    mutex_unlock(&M1);
    mutex_unlock(&M2);
}

static void plain_worker(void *arg) { logv((int)(long long)arg); }
static void hold1_worker(void *arg)
{
    (void)arg;
    mutex_lock(&M1);
    task_block();
    mutex_unlock(&M1);
}
static void idle_blocker(void *arg)
{
    (void)arg;
    task_block();
}

/* deadlock detection: A holds M1 and later wants M2, B holds M2 and wants M1 */
static volatile int DL_RESULT = 99;
static void dl_a(void *arg)
{
    (void)arg;
    mutex_lock(&M1);
    task_block();
    DL_RESULT = mutex_lock(&M2); /* B holds M2 and sleeps for M1, which we hold: waiting would be for ever */
    if (DL_RESULT == 0)
        mutex_unlock(&M2);
    mutex_unlock(&M1);
}
static void dl_b(void *arg)
{
    (void)arg;
    mutex_lock(&M2);
    mutex_lock(&M1);
    logv(2);
    mutex_unlock(&M1);
    mutex_unlock(&M2);
}

static void drain(void)
{
    for (int i = 0; i < 100 && task_count() > 1; i++)
        task_yield();
    task_yield();
}

int main(void)
{
    CHECK(task_init() == 0);

    /* ---- plain lock / unlock ---- */
    CHECK(M.owner == -1 && !mutex_held(&M));
    CHECK(mutex_unlock(&M) == -1); /* not locked */
    CHECK(mutex_lock(&M) == 0 && mutex_held(&M) && M.owner == 0);
    CHECK(mutex_lock(&M) == -1);    /* already ours: refused instead of waiting for itself */
    CHECK(mutex_trylock(&M) == -1); /* busy, even for the holder */
    CHECK(mutex_unlock(&M) == 0 && M.owner == -1 && !mutex_held(&M));
    CHECK(mutex_trylock(&M) == 0 && M.owner == 0);
    CHECK(mutex_unlock(&M) == 0);
    CHECK(host_if == 1);

    /* ---- a thread that finds it locked sleeps until the unlock ---- */
    LOGN = 0;
    CHECK(mutex_lock(&M) == 0);
    int w = task_create("W", lock_worker, (void *)5);
    CHECK(w > 0);
    task_yield(); /* W runs and blocks in mutex_lock */
    CHECK(task_state(w) == TASK_BLOCKED && LOGN == 0);
    CHECK(!task_others_ready()); /* asleep, not spinning */
    task_yield();
    CHECK(task_state(w) == TASK_BLOCKED && LOGN == 0);
    CHECK(mask_of(&M.waiters) == (1u << w));
    CHECK(mutex_unlock(&M) == 0);
    CHECK(task_state(w) == TASK_READY && mask_of(&M.waiters) == 0);
    drain();
    CHECK(LOGN == 1 && LOG[0] == 5 && M.owner == -1 && task_count() == 1);

    /* ---- the waiter with the highest priority gets the lock first ---- */
    LOGN = 0;
    CHECK(mutex_lock(&M) == 0);
    int t1 = task_create("LOW", lock_worker, (void *)1);
    int t2 = task_create("HIGH", lock_worker, (void *)2);
    int t3 = task_create("MID", lock_worker, (void *)3);
    CHECK(t1 > 0 && t2 > 0 && t3 > 0);
    CHECK(task_set_priority(t1, 0) == 0 && task_set_priority(t2, 4) == 0 && task_set_priority(t3, 3) == 0);
    for (int i = 0; i < 200 && mask_of(&M.waiters) != ((1u << t1) | (1u << t2) | (1u << t3)); i++)
        task_yield(); /* the lowest one only gets to run (and block) after aging */
    CHECK(task_state(t1) == TASK_BLOCKED && task_state(t2) == TASK_BLOCKED && task_state(t3) == TASK_BLOCKED);
    CHECK(mutex_unlock(&M) == 0);
    for (int i = 0; i < 2000 && task_count() > 1; i++) /* the lowest one is only picked after aging */
        task_yield();
    task_yield();
    CHECK(LOGN == 3 && LOG[0] == 2 && LOG[1] == 3 && LOG[2] == 1);

    /* ---- a waiter that was killed is skipped, the wake-up goes to the next one ---- */
    LOGN = 0;
    CHECK(mutex_lock(&M) == 0);
    int k1 = task_create("KILLED", lock_worker, (void *)1);
    int k2 = task_create("LIVE", lock_worker, (void *)2);
    CHECK(task_set_priority(k1, 4) == 0);
    for (int i = 0; i < 200 && mask_of(&M.waiters) != ((1u << k1) | (1u << k2)); i++)
        task_yield(); /* k1 blocks first and lends its priority to us: k2 only runs after aging */
    CHECK(task_state(k1) == TASK_BLOCKED && task_state(k2) == TASK_BLOCKED);
    CHECK(task_kill(k1) == 0);
    CHECK(mutex_unlock(&M) == 0);
    CHECK(task_state(k2) == TASK_READY); /* not wasted on the dead thread */
    drain();
    CHECK(LOGN == 1 && LOG[0] == 2 && M.owner == -1 && task_count() == 1);

    /* ---- only the holder can unlock ---- */
    CHECK(mutex_lock(&M) == 0);
    UNLOCK_RESULT = 99;
    task_create("OTHER", foreign_unlock_worker, 0);
    task_yield();
    CHECK(UNLOCK_RESULT == -1 && M.owner == 0); /* a thread that does not hold it cannot release it */
    CHECK(mutex_unlock(&M) == 0);
    drain();
    CHECK(M.owner == -1);

    /* ---- condition variable ---- */
    CHECK(cond_wait(&CV, &M) == -1); /* the mutex is not held */
    cond_signal(&CV);                /* nobody waits: nothing happens, nothing is remembered */
    cond_broadcast(&CV);
    CHECK(mask_of(&CV.waiters) == 0);

    LOGN = 0;
    FLAG = 0;
    int c1 = task_create("C1", wait_worker, (void *)1);
    task_yield();
    CHECK(task_state(c1) == TASK_BLOCKED && M.owner == -1); /* waiting releases the mutex */
    CHECK(mask_of(&CV.waiters) == (1u << c1));
    task_yield();
    CHECK(task_state(c1) == TASK_BLOCKED && LOGN == 0);
    cond_signal(&CV); /* the flag is still 0: C1 wakes, checks, waits again */
    task_yield();
    CHECK(LOGN == 0 && task_state(c1) == TASK_BLOCKED);
    CHECK(mutex_lock(&M) == 0);
    FLAG = 1;
    cond_signal(&CV);
    CHECK(task_state(c1) == TASK_READY);
    task_yield();
    CHECK(task_state(c1) == TASK_BLOCKED && LOGN == 0); /* it woke but the mutex is still ours: it waits for the unlock */
    CHECK(mutex_unlock(&M) == 0);
    drain();
    CHECK(LOGN == 1 && LOG[0] == 1 && M.owner == -1 && task_count() == 1);

    /* signal wakes one waiter, broadcast wakes all */
    LOGN = 0;
    FLAG = 0;
    int d[3];
    for (int i = 0; i < 3; i++)
        d[i] = task_create("D", wait_worker, (void *)(long long)(i + 1));
    task_yield();
    for (int i = 0; i < 3; i++)
        CHECK(task_state(d[i]) == TASK_BLOCKED);
    CHECK(mutex_lock(&M) == 0);
    FLAG = 1;
    cond_signal(&CV);
    CHECK(mutex_unlock(&M) == 0);
    for (int i = 0; i < 20; i++)
        task_yield();
    CHECK(LOGN == 1); /* only one went on; the others still wait (the flag stays 1, but nobody woke them) */
    int still = 0;
    for (int i = 0; i < 3; i++)
        still += task_state(d[i]) == TASK_BLOCKED;
    CHECK(still == 2);
    cond_broadcast(&CV);
    CHECK(mask_of(&CV.waiters) == 0);
    drain();
    CHECK(LOGN == 3 && task_count() == 1 && M.owner == -1);

    /* ---- priority inheritance ---- */
    CHECK(task_set_priority(0, 0) == 0); /* the main thread is the lowest, so the order below is the priorities' doing */
    {
        /* the holder (priority 1) gets the priority of the waiter (4), so it runs before a thread of priority 3 and unlocks in time */
        LOGN = 0;
        int lo = task_create("LO", sleeper_holder, 0);
        task_set_priority(lo, 1);
        task_yield(); /* LO takes M and sleeps */
        CHECK(task_state(lo) == TASK_BLOCKED && M.owner == lo);
        CHECK(task_effective_priority(lo) == 1);
        int hi = task_create("HI", lock_worker, (void *)7);
        task_set_priority(hi, 4);
        task_yield(); /* HI wants M and sleeps */
        CHECK(task_state(hi) == TASK_BLOCKED);
        CHECK(task_priority(lo) == 1 && task_effective_priority(lo) == 4); /* its own priority is unchanged; it runs at 4 */
        int mid = task_create("MID", plain_worker, (void *)5);
        task_set_priority(mid, 3);
        task_wake(lo);
        drain();
        CHECK(LOGN == 3 && LOG[0] == 1 && LOG[1] == 7 && LOG[2] == 5); /* LO (back to 1 after its unlock), HI, then MID */
        CHECK(task_effective_priority(lo) == -1 && task_count() == 1);

        /* a chain: C (4) waits for B's M2, B (2) waits for A's M1, A (1) holds M1: A and B both run at 4 */
        LOGN = 0;
        int a = task_create("A", hold1_worker, 0);
        task_set_priority(a, 1);
        task_yield();
        int b = task_create("B", chain_worker, 0);
        task_set_priority(b, 2);
        task_yield(); /* B has M2 and waits for M1 */
        CHECK(task_state(b) == TASK_BLOCKED && task_effective_priority(a) == 2);
        int c = task_create("C", lock2_worker, (void *)9);
        task_set_priority(c, 4);
        task_yield(); /* C waits for M2 */
        CHECK(task_state(c) == TASK_BLOCKED);
        CHECK(task_effective_priority(b) == 4 && task_effective_priority(a) == 4);
        CHECK(task_priority(a) == 1 && task_priority(b) == 2);
        task_wake(a);
        drain();
        CHECK(LOGN == 1 && LOG[0] == 9 && task_count() == 1 && M1.owner == -1 && M2.owner == -1);

        /* two mutexes: after the first unlock the holder keeps what the waiter of the other one asks for, after the second it is back */
        LOGN = 0;
        int t = task_create("TWO", two_holder, 0);
        task_set_priority(t, 1);
        task_yield(); /* holds M1 and M2 */
        int h = task_create("H", lock1_worker, (void *)7);
        task_set_priority(h, 4);
        task_yield();
        CHECK(task_effective_priority(t) == 4);
        int m = task_create("M", lock2_worker, (void *)5);
        task_set_priority(m, 2);
        task_yield();
        CHECK(task_state(h) == TASK_BLOCKED && task_state(m) == TASK_BLOCKED && task_effective_priority(t) == 4);
        task_wake(t);
        drain();
        CHECK(LOGN == 4 && LOG[0] == 2 && LOG[1] == 1 && LOG[2] == 7 && LOG[3] == 5); /* 4 -> 2 (M's wait) -> 1 */
        CHECK(task_count() == 1);

        /* the pieces the system calls use: a registered waiter lends its priority, a cancelled one takes it back */
        CHECK(mutex_lock(&M1) == 0);
        int x = task_create("X", idle_blocker, 0);
        task_set_priority(x, 4);
        task_yield();
        CHECK(task_effective_priority(0) == 0);
        mutex_wait_for(&M1, x);
        CHECK(task_effective_priority(0) == 4 && task_priority(0) == 0);
        mutex_cancel_wait(&M1, x);
        CHECK(task_effective_priority(0) == 0);
        CHECK(mutex_unlock(&M1) == 0);
        task_wake(x);
        drain();

        /* deadlock detection: the thread that would close the cycle is told (-1), the other one still gets its lock afterwards */
        LOGN = 0;
        int da = task_create("DLA", dl_a, 0);
        task_set_priority(da, 1);
        task_yield(); /* A holds M1 and sleeps */
        int db = task_create("DLB", dl_b, 0);
        task_set_priority(db, 2);
        task_yield(); /* B holds M2 and sleeps for M1 */
        CHECK(task_state(da) == TASK_BLOCKED && task_state(db) == TASK_BLOCKED && M1.owner == da && M2.owner == db);
        CHECK(mutex_would_deadlock(&M2, da) == 1);       /* A waiting for M2: B waits for M1, A holds it */
        CHECK(mutex_would_deadlock(&M2, 0) == 0);        /* the main thread holds nothing B waits for */
        CHECK(mutex_would_deadlock(&M1, db) == 0);       /* B already waits there: its holder A waits for nothing yet */
        CHECK(mutex_would_deadlock(&M2, -1) == 0 && mutex_would_deadlock(&M2, task_slots()) == 0);
        task_wake(da);
        drain();
        CHECK(DL_RESULT == -1);                          /* told, not put to sleep for ever */
        CHECK(LOGN == 1 && LOG[0] == 2);                 /* B got M1 once A let it go */
        CHECK(M1.owner == -1 && M2.owner == -1 && task_count() == 1);
        /* a chain without a cycle is fine: the earlier tests wait for each other in a line and all finish */

        /* a holder that ended while waiters existed: the lock is taken back and no stale loan is left */
        CHECK(task_set_inherited(0, TASK_PRIO_MAX + 1) == -1 && task_set_inherited(5, 1) == -1 && task_set_inherited(0, -1) == 0);
    }
    CHECK(task_set_priority(0, TASK_PRIO_DEFAULT) == 0 && task_effective_priority(0) == TASK_PRIO_DEFAULT);

    CHECK(BLOCKS_OUT == 0);
    CHECK(host_if == 1);
    if (fails == 0)
        printf("mutex tests passed\n");
    return fails ? 1 : 0;
}
