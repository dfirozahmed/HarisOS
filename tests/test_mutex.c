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
    CHECK(M.waiters == (1u << w));
    CHECK(mutex_unlock(&M) == 0);
    CHECK(task_state(w) == TASK_READY && M.waiters == 0);
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
    for (int i = 0; i < 200 && M.waiters != ((1u << t1) | (1u << t2) | (1u << t3)); i++)
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
    task_yield();
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
    CHECK(CV.waiters == 0);

    LOGN = 0;
    FLAG = 0;
    int c1 = task_create("C1", wait_worker, (void *)1);
    task_yield();
    CHECK(task_state(c1) == TASK_BLOCKED && M.owner == -1); /* waiting releases the mutex */
    CHECK(CV.waiters == (1u << c1));
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
    CHECK(CV.waiters == 0);
    drain();
    CHECK(LOGN == 3 && task_count() == 1 && M.owner == -1);

    CHECK(BLOCKS_OUT == 0);
    CHECK(host_if == 1);
    if (fails == 0)
        printf("mutex tests passed\n");
    return fails ? 1 : 0;
}
