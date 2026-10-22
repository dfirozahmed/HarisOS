/* Host test for the kernel threads (kernel/core/task.c + task_ctx.c): the real context switch runs here, on stacks
 * made by task_create(). The heap and the clock are replaced by host versions so the test can find leaks and move time. */
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- heap and clock ---- */
static int BLOCKS_OUT;
static int ALLOC_FAIL;
static unsigned long long NOW_US = 1000000;

void *kmalloc(unsigned long long size)
{
    if (ALLOC_FAIL)
        return 0;
    BLOCKS_OUT++;
    return malloc((size_t)size);
}

void kfree(void *p)
{
    if (!p)
        return;
    BLOCKS_OUT--;
    free(p);
}

unsigned long long timer_us(void) { return NOW_US; }

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

    /* the same self test the kernel runs at boot */
    CHECK(task_selftest() == 0);
    CHECK(BLOCKS_OUT == 0);
    CHECK(task_switches() > 0);

    if (fails == 0)
        printf("task tests passed\n");
    return fails ? 1 : 0;
}
