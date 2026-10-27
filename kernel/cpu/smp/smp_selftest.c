/* SMP: self tests (spinlock, allocators, scheduler lock) with all processors at once. */
#include "smp_internal.h"

#if defined(__x86_64__) && !defined(HARIS_HOST)

/* ---- spinlock self test: all processors fight for one lock ---- */
#define SPIN_TEST_ROUNDS 100000

static Spinlock TEST_LOCK = SPINLOCK_INIT;
static volatile u64 TEST_COUNT;
static volatile int TEST_INSIDE, TEST_OVERLAP;

static void spin_test_job(void)
{
    for (int i = 0; i < SPIN_TEST_ROUNDS; i++)
    {
        u64 f = spin_lock_irqsave(&TEST_LOCK);
        if (TEST_INSIDE)
            TEST_OVERLAP++; /* two processors inside at once: the lock does not exclude */
        TEST_INSIDE = 1;
        u64 v = TEST_COUNT; /* a read and a later write: updates are lost without the lock */
        TEST_COUNT = v + 1;
        TEST_INSIDE = 0;
        spin_unlock_irqrestore(&TEST_LOCK, f);
    }
}

/* ---- allocator self test: heap and page allocator used by all processors at once, WITHOUT the big kernel lock ---- */
#define ALLOC_TEST_ROUNDS 20000
static volatile u64 ALLOC_BAD;

static void alloc_test_job(void)
{
    unsigned me = percpu_index();
    u64 seed = 0x9E3779B97F4A7C15ULL * (me + 1);
    unsigned char *blk[16] = {0};
    unsigned size[16] = {0};
    for (int r = 0; r < ALLOC_TEST_ROUNDS; r++)
    {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        unsigned s = (unsigned)(seed & 15);
        unsigned char tag = (unsigned char)(me * 16 + s + 1);
        if (blk[s])
        {
            for (unsigned i = 0; i < size[s]; i++)
                if (blk[s][i] != tag)
                {
                    ALLOC_BAD++; /* somebody else wrote into our block: the allocator handed it out twice */
                    break;
                }
            kfree(blk[s]);
            blk[s] = 0;
        }
        else
        {
            size[s] = 16 + (unsigned)((seed >> 8) % 2000);
            blk[s] = kmalloc(size[s]);
            if (!blk[s])
            {
                ALLOC_BAD++;
                continue;
            }
            for (unsigned i = 0; i < size[s]; i++)
                blk[s][i] = tag;
        }
        if ((r & 31) == 0) /* and whole pages from the page allocator */
        {
            u64 pg = pmm_alloc_page();
            if (!pg)
            {
                ALLOC_BAD++;
                continue;
            }
            volatile u64 *w = (volatile u64 *)pg;
            w[0] = (u64)me + 1;
            w[511] = ~(u64)me;
            for (volatile int spin = 0; spin < 20; spin++)
                ;
            if (w[0] != (u64)me + 1 || w[511] != ~(u64)me)
                ALLOC_BAD++; /* the same page was given to two processors */
            pmm_free_page(pg);
        }
    }
    for (int s = 0; s < 16; s++)
        if (blk[s])
            kfree(blk[s]);
}

int smp_selftest_alloc(void)
{
    int n = 0;
    HeapStats before, after;
    heap_stats(&before);
    ALLOC_BAD = 0;
    for (int k = 1; k < TOTAL; k++)
        if (smp_run_on(k, alloc_test_job) == 0)
            n++;
    if (!n)
        return 1;
    alloc_test_job();
    u64 t0 = timer_us();
    int timeout = 0;
    bkl_unlock(); /* as in the spinlock test: the others run outside the lock anyway, but they must be able to get back in afterwards */
    for (int k = 1; k < TOTAL && !timeout; k++)
        while (CPU[k].online && !smp_job_done(k))
            if (timer_us() - t0 > 60000000)
            {
                timeout = 1;
                break;
            }
            else
                __asm__ volatile("pause");
    bkl_lock();
    if (timeout)
        return -2;
    heap_stats(&after);
    if (ALLOC_BAD)
        return -1;
    if (heap_check() != 0)
        return -3;
    return after.used_bytes == before.used_bytes && after.used_blocks == before.used_blocks ? 0 : -4;
}

/* ---- scheduler lock self test: threads created and re-prioritised by all processors at once, WITHOUT the big kernel lock ---- */
#define SCHED_TEST_ROUNDS 24
static volatile u64 SCHED_SEEN[4096 / 64]; /* bit n: slot n was handed out in this test */
static volatile int SCHED_BAD;
static int SCHED_IDS[SMP_MAX_CPUS][SCHED_TEST_ROUNDS];

static void sched_test_thread(void *arg) { (void)arg; } /* never runs: nothing schedules while the test is on */

static void sched_test_job(void)
{
    unsigned me = percpu_index();
    for (int r = 0; r < SCHED_TEST_ROUNDS; r++)
    {
        int id = task_create("SCHEDTEST", sched_test_thread, 0);
        SCHED_IDS[me][r] = id;
        if (id < 0)
        {
            __atomic_fetch_add(&SCHED_BAD, 1, __ATOMIC_RELAXED);
            continue;
        }
        u64 bit = 1ULL << (id % 64);
        if (__atomic_fetch_or(&SCHED_SEEN[id / 64], bit, __ATOMIC_ACQ_REL) & bit)
            __atomic_fetch_add(&SCHED_BAD, 1, __ATOMIC_RELAXED); /* the same slot was given to two creators */
        if (task_set_priority(id, 1) != 0 || task_priority(id) != 1)
            __atomic_fetch_add(&SCHED_BAD, 1, __ATOMIC_RELAXED);
        task_wake(id); /* not blocked: must change nothing */
        if (task_state(id) != TASK_READY)
            __atomic_fetch_add(&SCHED_BAD, 1, __ATOMIC_RELAXED);
    }
}

int smp_selftest_sched(void)
{
    int n = 0, before = task_count();
    SCHED_BAD = 0;
    for (int w = 0; w < 4096 / 64; w++)
        SCHED_SEEN[w] = 0;
    for (int k = 1; k < TOTAL; k++)
    {
        for (int r = 0; r < SCHED_TEST_ROUNDS; r++)
            SCHED_IDS[k][r] = -1;
        if (smp_run_on(k, sched_test_job) == 0)
            n++;
    }
    if (!n)
        return 1;
    for (int r = 0; r < SCHED_TEST_ROUNDS; r++)
        SCHED_IDS[0][r] = -1;
    sched_test_job();
    u64 t0 = timer_us();
    int timeout = 0;
    bkl_unlock(); /* the others work outside the big kernel lock; they must be able to get back in afterwards */
    for (int k = 1; k < TOTAL && !timeout; k++)
        while (CPU[k].online && !smp_job_done(k))
            if (timer_us() - t0 > 20000000)
            {
                timeout = 1;
                break;
            }
            else
                __asm__ volatile("pause");
    bkl_lock();
    if (timeout)
        return -2;
    int created = 0;
    for (int k = 0; k < TOTAL; k++)
        for (int r = 0; r < SCHED_TEST_ROUNDS; r++)
            if (SCHED_IDS[k][r] >= 0)
            {
                created++;
                task_kill(SCHED_IDS[k][r]); /* not started yet: dead at once, and the next switch frees it */
            }
    task_yield();
    if (SCHED_BAD)
        return -1;
    if (task_count() != before)
        return -3; /* a thread was lost or is still there */
    return created == TOTAL * SCHED_TEST_ROUNDS ? 0 : -4;
}

int smp_selftest_spinlock(void)
{
    int n = 0;
    for (int k = 1; k < TOTAL; k++)
        if (smp_run_on(k, spin_test_job) == 0)
            n++;
    if (!n)
        return 1;
    spin_test_job(); /* the boot processor takes part */
    u64 t0 = timer_us();
    int timeout = 0;
    bkl_unlock(); /* the others must be able to enter the kernel while we wait (interrupts are off here, nothing needs the kernel) */
    for (int k = 1; k < TOTAL && !timeout; k++)
        while (CPU[k].online && !smp_job_done(k))
            if (timer_us() - t0 > 20000000)
            {
                timeout = 1;
                break;
            }
            else
                __asm__ volatile("pause");
    bkl_lock();
    if (timeout)
        return -2;
    if (TEST_OVERLAP)
        return -3;
    return TEST_COUNT == (u64)(n + 1) * SPIN_TEST_ROUNDS ? 0 : -1;
}

#endif
