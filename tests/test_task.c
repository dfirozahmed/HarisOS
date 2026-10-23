/* Host test for the kernel threads (kernel/core/task.c + task_ctx.c): the real context switch runs here, on stacks
 * made by task_create(). The heap and the clock are replaced by host versions so the test can find leaks and move time. */
#include "task.h"
#include "aspace.h"
#include "irq.h"
#include "syscall.h"
#include "pmm.h"
#include "preempt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the Windows C runtime has no aligned_alloc */
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

/* ---- heap, clock, interrupt flag ---- */
volatile int host_if = 1; /* irq.h (HARIS_HOST): the interrupt flag of this pretend CPU */

static int BLOCKS_OUT;
static int ALLOC_FAIL;
static unsigned long long NOW_US = 1000000;
static unsigned long long TIME_STEP; /* the clock moves this much on every timer_us() call */
static int TICK_EVERY;               /* > 0: every N-th timer_us() call acts like a timer interrupt (task_tick) */
static int TICK_CALLS, IN_TICK;

/* The page allocator: stacks are runs of whole pages. */
unsigned long long pmm_alloc_contiguous_tag(unsigned long long count, PmmTag tag)
{
    (void)tag;
    if (ALLOC_FAIL)
        return 0;
    BLOCKS_OUT++;
    return (unsigned long long)ALIGNED_ALLOC(4096, count * 4096);
}

void pmm_free_contiguous(unsigned long long phys, unsigned long long count)
{
    (void)count;
    BLOCKS_OUT--;
    ALIGNED_FREE((void *)phys);
}

/* Address spaces and the kernel stack of ring 3 traps: only recorded (nothing here runs in ring 3). */
static AddressSpace *LOADED;
static int AS_SWITCHES, AS_DESTROYED, KSTACK_SETS;
static unsigned long long LAST_KSTACK;

void aspace_switch(AddressSpace *as)
{
    LOADED = as;
    AS_SWITCHES++;
}

void aspace_destroy(AddressSpace *as)
{
    (void)as;
    AS_DESTROYED++;
}

void syscall_set_kernel_stack(unsigned long long top)
{
    LAST_KSTACK = top;
    KSTACK_SETS++;
}

/* Page tables: the test only records which pages were made not present (the host memory stays usable). */
static unsigned long long GUARDS[64];
static int GUARDS_OUT, PAGING_FAIL, WRONG_UNGUARD;
static unsigned long long LAST_GUARD;

int paging_set_present(unsigned long long virt, int present)
{
    if (PAGING_FAIL)
        return -1;
    if (!present)
    {
        GUARDS[GUARDS_OUT++] = LAST_GUARD = virt;
        return 0;
    }
    for (int i = 0; i < GUARDS_OUT; i++)
        if (GUARDS[i] == virt)
        {
            GUARDS[i] = GUARDS[--GUARDS_OUT];
            return 0;
        }
    WRONG_UNGUARD++; /* mapping a page that was never unmapped */
    return 0;
}

/* A spinning thread calls this over and over, so a "tick" injected here hits it at an arbitrary point, like a real
 * interrupt. IN_TICK stops the scheduler's own timer_us() call from ticking again. */
unsigned long long timer_us(void)
{
    NOW_US += TIME_STEP;
    if (TICK_EVERY && !IN_TICK && ++TICK_CALLS % TICK_EVERY == 0)
    {
        IN_TICK = 1;
        task_tick();
        IN_TICK = 0;
    }
    return NOW_US;
}

int timer_ready(void) { return 1; }
int idle_ready(void) { return 1; }

/* ---- workers (they must not call printf: only plain code runs on these small stacks) ---- */
static int LOG[256];
static int LOGN;
static void logv(int v)
{
    if (LOGN < 256)
        LOG[LOGN++] = v;
}

static void turn_worker(void *arg)
{
    int id = (int)(long long)arg;
    for (int i = 0; i < 3; i++)
    {
        logv(id);
        task_yield();
    }
}

/* many values alive across the yield: the compiler keeps them in callee-saved registers (rbx, rsi, rdi, r12-r15,
 * xmm6-15), so a switch that loses one of them gives a wrong sum */
static int REG_RESULT[4];
static void reg_worker(void *arg)
{
    int id = (int)(long long)arg;
    long long a = id * 3 + 1, b = id * 5 + 2, c = id * 7 + 3, d = id * 11 + 4, e = id * 13 + 5, f = id * 17 + 6, g = id * 19 + 7;
    double x = id + 0.5, y = id * 2 + 0.25, z = id * 3 + 0.125, w = id * 4 + 0.0625, u = id * 5 + 0.5, v = id * 6 + 0.75;
    for (int i = 0; i < 5; i++)
    {
        task_yield();
        a += b;
        b += c;
        c += d;
        d += e;
        e += f;
        f += g;
        g += a;
        x += y;
        y += z;
        z += w;
        w += u;
        u += v;
        v += x;
    }
    REG_RESULT[id] = (int)((a + b + c + d + e + f + g) % 1000003) + (int)(x + y + z + w + u + v);
}

/* the same arithmetic without any switching, to compare with */
static int reg_expected(int id)
{
    long long a = id * 3 + 1, b = id * 5 + 2, c = id * 7 + 3, d = id * 11 + 4, e = id * 13 + 5, f = id * 17 + 6, g = id * 19 + 7;
    double x = id + 0.5, y = id * 2 + 0.25, z = id * 3 + 0.125, w = id * 4 + 0.0625, u = id * 5 + 0.5, v = id * 6 + 0.75;
    for (int i = 0; i < 5; i++)
    {
        a += b;
        b += c;
        c += d;
        d += e;
        e += f;
        f += g;
        g += a;
        x += y;
        y += z;
        z += w;
        w += u;
        u += v;
        v += x;
    }
    return (int)((a + b + c + d + e + f + g) % 1000003) + (int)(x + y + z + w + u + v);
}

/* The compiler decides which registers hold the values above, so this assembly routine fills ALL callee-saved
 * registers (rbx rbp rdi rsi r12-r15 and xmm6-15) with seed-dependent values, yields, and checks every one of them
 * afterwards. Another thread doing the same with a different seed runs in between. */
int regs_probe(long long seed);
__asm__(".text\n"
        ".globl regs_probe\n"
        "regs_probe:\n"
        "    pushq %rbx\n"
        "    pushq %rbp\n"
        "    pushq %rdi\n"
        "    pushq %rsi\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $40, %rsp\n" /* 32 bytes shadow space for the call + the seed's slot; keeps RSP 16-aligned */
        "    movq %rcx, 32(%rsp)\n"
        ".irp k,6,7,8,9,10,11,12,13,14,15\n"
        "    leaq \\k(%rcx), %rax\n"
        "    movq %rax, %xmm\\k\n"
        ".endr\n"
        "    leaq 1(%rcx), %rbx\n"
        "    leaq 2(%rcx), %rbp\n"
        "    leaq 3(%rcx), %rdi\n"
        "    leaq 4(%rcx), %rsi\n"
        "    leaq 5(%rcx), %r12\n"
        "    leaq 6(%rcx), %r13\n"
        "    leaq 7(%rcx), %r14\n"
        "    leaq 8(%rcx), %r15\n"
        "    call task_yield\n"
        "    movq 32(%rsp), %rcx\n"
        ".irp k,6,7,8,9,10,11,12,13,14,15\n"
        "    movq %xmm\\k, %rax\n"
        "    leaq \\k(%rcx), %rdx\n"
        "    cmpq %rax, %rdx\n"
        "    jne 2f\n"
        ".endr\n"
        "    leaq 1(%rcx), %rdx\n    cmpq %rbx, %rdx\n    jne 2f\n"
        "    leaq 2(%rcx), %rdx\n    cmpq %rbp, %rdx\n    jne 2f\n"
        "    leaq 3(%rcx), %rdx\n    cmpq %rdi, %rdx\n    jne 2f\n"
        "    leaq 4(%rcx), %rdx\n    cmpq %rsi, %rdx\n    jne 2f\n"
        "    leaq 5(%rcx), %rdx\n    cmpq %r12, %rdx\n    jne 2f\n"
        "    leaq 6(%rcx), %rdx\n    cmpq %r13, %rdx\n    jne 2f\n"
        "    leaq 7(%rcx), %rdx\n    cmpq %r14, %rdx\n    jne 2f\n"
        "    leaq 8(%rcx), %rdx\n    cmpq %r15, %rdx\n    jne 2f\n"
        "    movl $1, %eax\n"
        "    jmp 3f\n"
        "2:  xorl %eax, %eax\n"
        "3:  addq $40, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rbp\n"
        "    popq %rbx\n"
        "    ret\n");

static int PROBE_OK[4];
static void probe_worker(void *arg)
{
    int id = (int)(long long)arg;
    PROBE_OK[id] = regs_probe(1000 * (id + 1));
}

/* aligned locals make the compiler use aligned SSE moves: a misaligned stack would crash here */
static int ALIGN_OK;
static void align_worker(void *arg)
{
    (void)arg;
    _Alignas(16) double v[4] = {1, 2, 3, 4};
    for (int i = 0; i < 3; i++)
    {
        task_yield();
        for (int k = 0; k < 4; k++)
            v[k] = v[k] * 2 + 1;
    }
    ALIGN_OK = (v[0] == 15 && v[3] == 39);
}

static void nested_worker(void *arg)
{
    (void)arg;
    logv(100);
    int child = task_create("CHILD", turn_worker, (void *)7);
    logv(child > 0 ? 101 : -1);
    task_yield();
}

static int SLEEP_DONE;
static void sleep_worker(void *arg)
{
    (void)arg;
    task_sleep_ms(5);
    SLEEP_DONE = 1;
}

static void quick_worker(void *arg) { (*(volatile int *)arg)++; }

/* ---- process threads ---- */
static AddressSpace *SEEN_AS;
static int SEEN_LOADED, SEEN_IF;
static void user_worker(void *arg)
{
    (void)arg;
    SEEN_AS = task_address_space();
    SEEN_LOADED = LOADED == SEEN_AS;
    SEEN_IF = host_if;
}

static int SEEN_N, SEEN_BAD;
static void user_pair_worker(void *arg)
{
    for (int i = 0; i < 2; i++)
    {
        SEEN_N++;
        if (task_address_space() != arg || LOADED != arg)
            SEEN_BAD++;
        task_yield();
    }
}

/* ---- timer tick behaviour (task_tick called by hand) ---- */
static volatile int TK_IF_NORMAL = -1, TK_IF_PREEMPT = -1;
static volatile int TK_NORMAL_OK, TK_P1, TK_P2, TK_P3;

static void tick_normal_worker(void *arg)
{
    (void)arg;
    TK_IF_NORMAL = host_if;
    unsigned long long s0 = task_switches();
    for (int i = 0; i < 50; i++)
        task_tick();
    TK_NORMAL_OK = task_switches() == s0; /* a normal thread is never taken off the CPU by a tick */
}

static void tick_preempt_worker(void *arg)
{
    (void)arg;
    TK_IF_PREEMPT = host_if;
    unsigned long long s0 = task_switches();
    for (int i = 0; i < TASK_SLICE_TICKS - 1; i++)
        task_tick();
    TK_P1 = task_switches() == s0; /* the slice is not over yet */
    preempt_disable();
    for (int i = 0; i < 20; i++)
        task_tick();
    TK_P2 = task_switches() == s0; /* ... and while the lock is held, no number of ticks switches */
    preempt_enable();
    task_tick(); /* the last tick of the slice: the main thread gets the CPU here */
    TK_P3 = 1;   /* reached only after we were switched back in */
}

static void drain(void)
{
    for (int i = 0; i < 50 && task_count() > 1; i++)
        task_yield();
    task_yield(); /* frees the stack of the last finished thread */
}

int main(void)
{
    CHECK(task_init() == 0);
    CHECK(task_count() == 1);
    CHECK(task_current() == 0);
    CHECK(strcmp(task_name(0), "MAIN") == 0);
    CHECK(task_name(5) == 0);
    CHECK(task_create("X", 0, 0) == -1);

    /* a failed allocation leaves no slot behind */
    ALLOC_FAIL = 1;
    CHECK(task_create("X", turn_worker, 0) == -1);
    ALLOC_FAIL = 0;
    CHECK(task_count() == 1);

    /* with nobody else, yield returns at once */
    task_yield();
    CHECK(task_current() == 0 && task_state(0) == TASK_RUNNING);

    /* round robin: 1 2 3 1 2 3 1 2 3 */
    LOGN = 0;
    for (int i = 1; i <= 3; i++)
        CHECK(task_create("T", turn_worker, (void *)(long long)i) == i);
    CHECK(task_count() == 4);
    CHECK(strcmp(task_name(2), "T") == 0);
    CHECK(task_state(1) == TASK_READY);
    drain();
    CHECK(LOGN == 9);
    for (int i = 0; i < 9 && i < LOGN; i++)
        CHECK(LOG[i] == i % 3 + 1);
    CHECK(task_count() == 1);
    CHECK(BLOCKS_OUT == 0); /* all stacks freed */
    CHECK(task_state(1) == TASK_UNUSED);

    /* callee-saved registers survive the switches */
    for (int i = 0; i < 4; i++)
        task_create("REG", reg_worker, (void *)(long long)i);
    drain();
    for (int i = 0; i < 4; i++)
        CHECK(REG_RESULT[i] == reg_expected(i));

    /* every callee-saved register, general and xmm, is restored: three threads fill them differently and yield */
    for (int i = 0; i < 3; i++)
        task_create("PROBE", probe_worker, (void *)(long long)i);
    drain();
    for (int i = 0; i < 3; i++)
        CHECK(PROBE_OK[i] == 1);

    /* a new thread's stack is aligned like the stack of a called function */
    task_create("ALIGN", align_worker, 0);
    task_create("ALIGN", align_worker, 0);
    drain();
    CHECK(ALIGN_OK);

    /* a thread may start another one */
    LOGN = 0;
    task_create("NEST", nested_worker, 0);
    drain();
    CHECK(LOG[0] == 100 && LOG[1] == 101);
    CHECK(LOGN == 5); /* 100, 101, then the child logs 7 three times */
    CHECK(BLOCKS_OUT == 0);

    /* sleeping: not run before its time, run after */
    SLEEP_DONE = 0;
    int s = task_create("SLEEP", sleep_worker, 0);
    task_yield(); /* it goes to sleep */
    CHECK(task_state(s) == TASK_SLEEPING);
    task_yield();
    CHECK(!SLEEP_DONE && task_state(s) == TASK_SLEEPING);
    NOW_US += 4000;
    task_yield();
    CHECK(!SLEEP_DONE);
    NOW_US += 1000;
    task_yield(); /* wakes and ends */
    CHECK(SLEEP_DONE);
    drain();
    CHECK(task_count() == 1 && BLOCKS_OUT == 0);

    /* the main thread cannot sleep (it would leave nothing to run) */
    task_sleep_ms(50);
    CHECK(task_current() == 0 && task_state(0) == TASK_RUNNING);

    /* slots are reused: far more threads than TASK_MAX over time, and TASK_MAX - 1 at once */
    volatile int ran = 0;
    for (int round = 0; round < 10; round++)
    {
        for (int i = 0; i < TASK_MAX - 1; i++)
            CHECK(task_create("Q", quick_worker, (void *)&ran) > 0);
        CHECK(task_create("Q", quick_worker, (void *)&ran) == -1); /* full */
        drain();
    }
    CHECK(ran == 10 * (TASK_MAX - 1));
    CHECK(BLOCKS_OUT == 0);

    /* ---- the timer tick ---- */
    /* the main loop is never preempted, whatever the tick count */
    unsigned long long sw = task_switches();
    for (int i = 0; i < 100; i++)
        task_tick();
    CHECK(task_switches() == sw && task_current() == 0);

    /* a normal thread: ticks do nothing, and it starts with interrupts off like the main loop */
    task_create("TICKN", tick_normal_worker, 0);
    task_yield();
    CHECK(TK_IF_NORMAL == 0);
    CHECK(TK_NORMAL_OK);
    drain();

    /* a preemptible thread: starts with interrupts on, loses the CPU exactly at the end of its slice, not before,
     * and not while preempt_disable() holds */
    CHECK(host_if == 1);
    task_create_flags("TICKP", tick_preempt_worker, 0, TASK_PREEMPTIBLE);
    task_yield(); /* runs until its slice ends: the tick switches back to us */
    CHECK(host_if == 1); /* our own interrupt state came back */
    CHECK(TK_IF_PREEMPT == 1);
    CHECK(TK_P1 && TK_P2);
    CHECK(!TK_P3); /* it was stopped inside its last task_tick() */
    CHECK(task_current() == 0 && preempt_count == 0);
    CHECK(task_others_ready()); /* it is ready to continue */
    drain();
    CHECK(TK_P3);
    CHECK(!task_others_ready());

    /* a sleeper counts as "ready" only once its time has come */
    int sl = task_create("SLP", sleep_worker, 0);
    CHECK(sl > 0 && task_others_ready()); /* ready (not started yet) */
    SLEEP_DONE = 0;
    task_yield();
    CHECK(task_state(sl) == TASK_SLEEPING && !task_others_ready());
    NOW_US += 6000;
    CHECK(task_others_ready());
    drain();
    CHECK(SLEEP_DONE);

    /* the boot self test of the preemption: with a tick source it passes; without one it reports failure, not a hang */
    TIME_STEP = 5;
    TICK_EVERY = 0;
    CHECK(task_selftest_preempt() == -3);
    TICK_EVERY = 50;
    CHECK(task_selftest_preempt() == 0);
    TICK_EVERY = 0;
    TIME_STEP = 0;
    CHECK(BLOCKS_OUT == 0 && task_count() == 1);
    CHECK(preempt_count == 0 && host_if == 1);

    /* ---- process threads: their own address space ---- */
    static AddressSpace fake_as; /* only a pointer to compare: the scheduler hands it to aspace_switch, never reads it */
    CHECK(task_address_space() == 0);
    AS_SWITCHES = AS_DESTROYED = KSTACK_SETS = 0;
    LOADED = 0;
    CHECK(task_create_user("USER", user_worker, 0, 0) == -1); /* no space: not a process thread */
    CHECK(BLOCKS_OUT == 0);
    int u = task_create_user("USER", user_worker, 0, &fake_as);
    CHECK(u > 0);
    CHECK(task_state(u) == TASK_READY);
    CHECK(task_space_of(u) == &fake_as && task_space_of(0) == 0 && task_space_of(-1) == 0 && task_space_of(99) == 0);
    task_yield();   /* it runs in its space: the scheduler loaded it before the switch */
    CHECK(SEEN_AS == &fake_as && SEEN_LOADED);
    CHECK(SEEN_IF == 1); /* a process thread is preemptible: its ring 3 code runs with interrupts on */
    CHECK(task_address_space() == 0); /* ... and we are the main thread again, in the kernel's tables */
    CHECK(LOADED == 0 && AS_SWITCHES == 2); /* into the process's space, and back out */
    CHECK(KSTACK_SETS == 1 && LAST_KSTACK != 0 && LAST_KSTACK % 16 == 0); /* the thread's kernel stack, once */
    drain();
    CHECK(AS_DESTROYED == 1); /* freed with the thread */
    CHECK(task_space_of(u) == 0);
    CHECK(task_state(u) == TASK_UNUSED && BLOCKS_OUT == 0);

    /* kernel threads never change the address space or the trap stack */
    AS_SWITCHES = KSTACK_SETS = 0;
    ran = 0;
    task_create("K", quick_worker, (void *)&ran);
    drain();
    CHECK(ran == 1 && AS_SWITCHES == 0 && KSTACK_SETS == 0);

    /* two process threads: each switch loads the other's space, and the trap stack follows */
    static AddressSpace fake_as2;
    AS_SWITCHES = AS_DESTROYED = KSTACK_SETS = 0;
    SEEN_N = 0;
    task_create_user("U1", user_pair_worker, &fake_as, &fake_as);
    task_create_user("U2", user_pair_worker, &fake_as2, &fake_as2);
    drain();
    CHECK(SEEN_N == 4 && SEEN_BAD == 0); /* every time a thread ran, its own space was the loaded one */
    CHECK(AS_DESTROYED == 2 && LOADED == 0);
    CHECK(KSTACK_SETS >= 4);

    /* ---- guard pages ---- */
    CHECK(GUARDS_OUT == 0 && WRONG_UNGUARD == 0); /* everything so far was unmapped again when its stack was freed */
    int g = task_create("GUARDED", quick_worker, (void *)&ran);
    CHECK(g > 0 && GUARDS_OUT == 1);
    CHECK(LAST_GUARD % 4096 == 0);
    CHECK(task_guard_hit(LAST_GUARD) != 0 && strcmp(task_guard_hit(LAST_GUARD), "GUARDED") == 0);
    CHECK(task_guard_hit(LAST_GUARD + 4095) != 0);  /* the whole guard page */
    CHECK(task_guard_hit(LAST_GUARD + 4096) == 0);  /* the first usable stack page is not it */
    CHECK(task_guard_hit(LAST_GUARD - 1) == 0);
    drain();
    CHECK(GUARDS_OUT == 0 && WRONG_UNGUARD == 0); /* mapped again before the page went back to the allocator */
    CHECK(task_guard_hit(LAST_GUARD) == 0);       /* and no longer attributed to a thread */

    /* page tables that cannot be changed: the thread runs without a guard */
    PAGING_FAIL = 1;
    ran = 0;
    CHECK(task_create("NOGUARD", quick_worker, (void *)&ran) > 0);
    drain();
    PAGING_FAIL = 0;
    CHECK(ran == 1 && GUARDS_OUT == 0 && WRONG_UNGUARD == 0 && BLOCKS_OUT == 0);

    /* a full table gives the block back, guard not touched */
    for (int i = 0; i < TASK_MAX - 1; i++)
        task_create("Q", quick_worker, (void *)&ran);
    int guards_before = GUARDS_OUT;
    CHECK(task_create("Q", quick_worker, (void *)&ran) == -1);
    CHECK(GUARDS_OUT == guards_before);
    drain();
    CHECK(GUARDS_OUT == 0 && BLOCKS_OUT == 0);

    /* the same self test the kernel runs at boot */
    CHECK(task_selftest() == 0);
    CHECK(BLOCKS_OUT == 0);
    CHECK(task_switches() > 0);

    if (fails == 0)
        printf("task tests passed\n");
    return fails ? 1 : 0;
}
