/* Host test for the spinlock (include/spinlock.h) and the reader-writer lock (kernel/sync/rwlock.c) on the real scheduler (task.c,
 * task_ctx.c): the threads really block, are woken and switch stacks. The platform pieces task.c needs are stubs, like in test_mutex.c. */
#include "rwlock.h"
#include "spinlock.h"
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

static RWLock RW = RWLOCK_INIT;

/* read-lock, note arg, sleep until woken, unlock, note arg + 100 */
static void rd_hold(void *arg)
{
    rw_rdlock(&RW);
    logv((int)(long long)arg);
    task_block();
    rw_rdunlock(&RW);
    logv((int)(long long)arg + 100);
}
/* the same for writing */
static void wr_hold(void *arg)
{
    rw_wrlock(&RW);
    logv((int)(long long)arg);
    task_block();
    rw_wrunlock(&RW);
    logv((int)(long long)arg + 100);
}
/* lock, note, unlock at once */
static void rd_once(void *arg)
{
    rw_rdlock(&RW);
    logv((int)(long long)arg);
    rw_rdunlock(&RW);
}
static void wr_once(void *arg)
{
    rw_wrlock(&RW);
    logv((int)(long long)arg);
    rw_wrunlock(&RW);
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

    /* ---- spinlock ---- */
    {
        static Spinlock a = SPINLOCK_INIT, b = SPINLOCK_INIT;
        CHECK(host_if == 1 && !spin_is_locked(&a));
        unsigned long long fa = spin_lock_irqsave(&a);
        CHECK(host_if == 0 && spin_is_locked(&a)); /* interrupts off while it is held */
        unsigned long long f2;
        CHECK(spin_trylock_irqsave(&a, &f2) == -1 && host_if == 0 && spin_is_locked(&a)); /* busy: nothing changed */
        unsigned long long fb = spin_lock_irqsave(&b); /* locks nest; the inner one saw interrupts already off */
        CHECK(spin_is_locked(&b) && host_if == 0);
        spin_unlock_irqrestore(&b, fb);
        CHECK(!spin_is_locked(&b) && host_if == 0); /* still inside the outer one */
        spin_unlock_irqrestore(&a, fa);
        CHECK(!spin_is_locked(&a) && host_if == 1); /* back to what it was */
        CHECK(spin_trylock_irqsave(&a, &f2) == 0 && host_if == 0 && spin_is_locked(&a));
        spin_unlock_irqrestore(&a, f2);
        CHECK(host_if == 1);
        spin_init(&a);
        CHECK(!spin_is_locked(&a));
    }

    /* ---- reader-writer lock without contention ---- */
    CHECK(RW.readers == 0 && RW.writer == -1);
    CHECK(rw_rdunlock(&RW) == -1 && rw_wrunlock(&RW) == -1); /* nothing to let go */
    CHECK(rw_rdlock(&RW) == 0 && rw_rdlock(&RW) == 0 && RW.readers == 2 && rw_tryrdlock(&RW) == 0 && RW.readers == 3);
    CHECK(rw_trywrlock(&RW) == -1);                          /* readers keep a writer out */
    CHECK(rw_rdunlock(&RW) == 0 && rw_rdunlock(&RW) == 0 && rw_rdunlock(&RW) == 0 && rw_rdunlock(&RW) == -1);
    CHECK(rw_wrlock(&RW) == 0 && RW.writer == 0);
    CHECK(rw_wrlock(&RW) == -1 && rw_trywrlock(&RW) == -1 && rw_tryrdlock(&RW) == -1); /* a writer keeps everybody out, itself too */
    CHECK(rw_wrunlock(&RW) == 0 && RW.writer == -1 && rw_wrunlock(&RW) == -1);
    CHECK(rw_trywrlock(&RW) == 0 && rw_wrunlock(&RW) == 0);
    CHECK(host_if == 1);

    /* ---- readers share the lock, a writer has to wait for all of them ---- */
    LOGN = 0;
    int r1 = task_create("R1", rd_hold, (void *)1);
    int r2 = task_create("R2", rd_hold, (void *)2);
    task_yield();
    CHECK(r1 > 0 && r2 > 0 && RW.readers == 2 && LOGN == 2 && LOG[0] == 1 && LOG[1] == 2); /* both inside at once */
    CHECK(task_state(r1) == TASK_BLOCKED && task_state(r2) == TASK_BLOCKED);
    CHECK(rw_trywrlock(&RW) == -1 && rw_tryrdlock(&RW) == 0 && rw_rdunlock(&RW) == 0);
    task_wake(r1);
    task_yield();
    CHECK(RW.readers == 1 && LOGN == 3 && LOG[2] == 101);
    CHECK(rw_trywrlock(&RW) == -1); /* one reader is still in */
    task_wake(r2);
    drain();
    CHECK(RW.readers == 0 && LOGN == 4 && LOG[3] == 102 && task_count() == 1);
    CHECK(rw_trywrlock(&RW) == 0 && rw_wrunlock(&RW) == 0);

    /* ---- a writer keeps readers out; when it lets go they come in ---- */
    LOGN = 0;
    CHECK(rw_wrlock(&RW) == 0);
    int rd = task_create("RD", rd_once, (void *)5);
    task_yield();
    CHECK(task_state(rd) == TASK_BLOCKED && LOGN == 0 && mask_of(&RW.rwaiters) == (1u << rd));
    CHECK(!task_others_ready()); /* asleep, not spinning */
    CHECK(rw_wrunlock(&RW) == 0);
    CHECK(task_state(rd) == TASK_READY && mask_of(&RW.rwaiters) == 0);
    drain();
    CHECK(LOGN == 1 && LOG[0] == 5 && RW.readers == 0 && task_count() == 1);

    /* ---- writer preference: a reader that arrives while a writer waits goes behind it ---- */
    LOGN = 0;
    int ra = task_create("RA", rd_hold, (void *)1);
    task_yield(); /* RA is inside, asleep */
    int wa = task_create("WA", wr_once, (void *)10);
    task_yield(); /* WA waits for RA */
    CHECK(task_state(wa) == TASK_BLOCKED && mask_of(&RW.wwaiters) == (1u << wa) && RW.readers == 1);
    int rb = task_create("RB", rd_once, (void *)2);
    task_yield(); /* RB would share with RA, but a writer is waiting: it waits too */
    CHECK(task_state(rb) == TASK_BLOCKED && mask_of(&RW.rwaiters) == (1u << rb) && RW.readers == 1 && LOGN == 1);
    CHECK(rw_tryrdlock(&RW) == -1); /* the same for a try */
    task_wake(ra);
    drain();
    CHECK(LOGN == 4 && LOG[0] == 1 && LOG[1] == 101 && LOG[2] == 10 && LOG[3] == 2); /* RA, RA out, then the writer, then the reader */
    CHECK(RW.readers == 0 && RW.writer == -1 && mask_of(&RW.wwaiters) == 0 && mask_of(&RW.rwaiters) == 0 && task_count() == 1);
    (void)rb;

    /* ---- writers go in turn, the best waiting one first ---- */
    LOGN = 0;
    CHECK(rw_wrlock(&RW) == 0);
    int wl = task_create("WLOW", wr_once, (void *)1);
    int wh = task_create("WHIGH", wr_once, (void *)2);
    CHECK(task_set_priority(wl, 0) == 0 && task_set_priority(wh, 4) == 0);
    for (int i = 0; i < 200 && mask_of(&RW.wwaiters) != ((1u << wl) | (1u << wh)); i++)
        task_yield(); /* the low one only gets to run (and wait) after aging */
    CHECK(rw_wrunlock(&RW) == 0);
    for (int i = 0; i < 2000 && task_count() > 1; i++)
        task_yield();
    task_yield();
    CHECK(LOGN == 2 && LOG[0] == 2 && LOG[1] == 1);
    CHECK(task_count() == 1);

    /* ---- a writer that is killed while it holds the lock does not keep it ---- */
    LOGN = 0;
    int wk = task_create("WKILL", wr_hold, (void *)7);
    task_yield();
    CHECK(task_state(wk) == TASK_BLOCKED && RW.writer == wk && rw_trywrlock(&RW) == -1);
    CHECK(task_kill(wk) == 0);
    drain();
    CHECK(rw_trywrlock(&RW) == 0 && rw_wrunlock(&RW) == 0); /* the dead writer's lock was taken back */
    CHECK(rw_tryrdlock(&RW) == 0 && rw_rdunlock(&RW) == 0);

    /* ---- a waiting writer that is killed does not hold readers back ---- */
    LOGN = 0;
    int rk = task_create("RHOLD", rd_hold, (void *)1);
    task_yield();
    int wq = task_create("WQ", wr_once, (void *)3);
    task_yield(); /* the writer waits for the reader */
    CHECK(task_state(wq) == TASK_BLOCKED && mask_of(&RW.wwaiters) == (1u << wq));
    CHECK(rw_tryrdlock(&RW) == -1);
    CHECK(task_kill(wq) == 0);
    drain();
    CHECK(rw_tryrdlock(&RW) == 0 && rw_rdunlock(&RW) == 0); /* it does not stand in the way any more */
    task_wake(rk);
    drain();
    CHECK(RW.readers == 0 && task_count() == 1);

    CHECK(BLOCKS_OUT == 0 && host_if == 1);
    if (fails == 0)
        printf("rwlock tests passed\n");
    return fails ? 1 : 0;
}
