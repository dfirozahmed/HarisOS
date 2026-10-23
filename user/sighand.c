/* HarisOS user program: checks signal handlers (signal() in libc.h): a handler that runs when a process signals itself, one that is
 * not nested, a blocked signal, a handler that interrupts a loop that never makes a system call (a timer tick delivers it), registers
 * and floating point that survive many handlers, a sleeping read that is interrupted, fork and exec and default actions.
 * Exits with 53 if every check holds, otherwise with the number of the first one that did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

/* the compiler for this target wants this symbol whenever a program uses floating point */
int _fltused = 1;

static volatile int hits, last_sig, depth, max_depth;

static void h(int sig)
{
    hits++;
    last_sig = sig;
}

static void h_nested(int sig)
{
    (void)sig;
    hits++;
    depth++;
    if (depth > max_depth)
        max_depth = depth;
    if (hits == 1)
        sys_kill(getpid(), SIGINT); /* the same signal inside its handler: it waits until the handler is done */
    depth--;
}

/* A handler that uses a lot of registers: if the kernel did not put them back, the interrupted loop would see it. */
static void h_heavy(int sig)
{
    (void)sig;
    volatile double a = 1.25, b = 7.5, c = 0.125, d = 3.0, e = 9.5, f = 2.0;
    for (int i = 0; i < 50; i++)
    {
        a = a * b + c;
        c = d / e + f;
        d = a - c;
        e = e * 1.001 + a;
        f = f + b / 3.0;
    }
    hits++;
}

/* deterministic work with doubles and integers, so that a lost register would change the result */
static unsigned long long work(void)
{
    volatile double acc = 1.0;
    unsigned long long h1 = 1469598103934665603ULL, h2 = 7;
    for (int i = 0; i < 4000; i++)
    {
        acc = acc * 1.0000001 + (double)(i % 7);
        h1 = (h1 ^ (unsigned long long)(i * 31 + 5)) * 1099511628211ULL;
        h2 = h2 * 6364136223846793005ULL + h1;
    }
    return h1 ^ h2 ^ (unsigned long long)(long long)acc;
}

static void yields(int n)
{
    for (int i = 0; i < n; i++)
        sys_yield();
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0, st = 0;

    /* a process signals itself: the handler has run when the call returns, and the call's own result is intact */
    CHECK(1, signal(SIGINT, h) == 0);
    long long r = sys_kill(getpid(), SIGINT);
    CHECK(2, r == 0 && hits == 1 && last_sig == SIGINT);
    sys_kill(getpid(), SIGINT);
    sys_kill(getpid(), SIGINT);
    CHECK(3, hits == 3);

    /* not nested: the second signal waits until the first handler is finished, then runs */
    hits = 0;
    CHECK(4, signal(SIGINT, h_nested) == 0);
    sys_kill(getpid(), SIGINT);
    yields(2);
    CHECK(5, hits == 2 && max_depth == 1);

    /* blocked: it does not run until it is unblocked, and then runs at once */
    signal(SIGINT, h);
    hits = 0;
    unsigned old = sigblock(1u << SIGINT);
    CHECK(6, old == 0);
    sys_kill(getpid(), SIGINT);
    yields(3);
    CHECK(7, hits == 0);
    CHECK(8, sigblock(0) == (1u << SIGINT));
    CHECK(9, hits == 1);

    /* "ignore" ends when a handler is installed; a handler can be removed again */
    sys_sigignore(1u << SIGINT);
    sys_kill(getpid(), SIGINT);
    CHECK(10, hits == 1); /* ignored: nothing ran, the process lives */
    signal(SIGINT, h);
    sys_kill(getpid(), SIGINT);
    CHECK(11, hits == 2);

    /* what cannot be done */
    CHECK(12, signal(SIGKILL, h) == -1 && signal(1, h) == -1 && signal(40, h) == -1);
    CHECK(13, sys_sigaction(SIGINT, (void *)0x1000, (void *)0x2000) == -1);       /* addresses in the kernel's half */
    CHECK(14, sys_sigaction(SIGINT, (void *)h, (void *)0x2000) == -1);            /* the stub is not in the user half */

    /* a loop that makes no system call is reached by a timer tick */
    long long pid = fork();
    if (pid == 0)
    {
        hits = 0;
        signal(SIGTERM, h);
        volatile unsigned long long spin = 0;
        while (hits == 0)
            spin++; /* only the handler can end this */
        sys_exit(hits == 1 && last_sig == SIGTERM ? 7 : 8);
    }
    yields(30);
    CHECK(15, pid > 0 && sys_kill(pid, SIGTERM) == 0 && sys_wait(pid, &st) == pid && st == 7);

    /* many handlers in the middle of a computation: every register and xmm is as it was */
    unsigned long long want = work();
    pid = fork();
    if (pid == 0)
    {
        hits = 0;
        signal(SIGINT, h_heavy);
        int ok = 1;
        do
        {
            ok = ok && work() == want;
        } while (hits < 12);
        sys_exit(ok ? 0 : 1);
    }
    for (int i = 0; i < 12; i++)
    {
        yields(2);
        sys_kill(pid, SIGINT);
    }
    for (int i = 0; i < 40 && sys_kill(pid, 0) == 0; i++) /* keep nudging until it is done, in case some signals came too early */
    {
        sys_kill(pid, SIGINT);
        yields(3);
    }
    CHECK(16, sys_wait(pid, &st) == pid && st == 0);

    /* a sleeping read is interrupted: -1, and the handler has run */
    int p[2];
    CHECK(17, sys_pipe(p) == 0);
    pid = fork();
    if (pid == 0)
    {
        hits = 0;
        signal(SIGTERM, h);
        char c;
        long long n = sys_read(p[0], &c, 1); /* nobody writes: it sleeps */
        sys_exit(n == -1 && hits == 1 ? 7 : 8);
    }
    yields(30);
    CHECK(18, sys_kill(pid, SIGTERM) == 0 && sys_wait(pid, &st) == pid && st == 7);
    sys_close(p[0]);
    sys_close(p[1]);

    /* fork keeps handlers; exec drops them; the default can be restored */
    hits = 0;
    signal(SIGINT, h);
    pid = fork();
    if (pid == 0)
    {
        sys_kill(getpid(), SIGINT);
        sys_exit(hits == 1 ? 0 : 1); /* the handler was inherited */
    }
    CHECK(19, sys_wait(pid, &st) == pid && st == 0 && hits == 0);

    pid = fork();
    if (pid == 0)
    {
        signal(SIGTERM, h);
        const char *argv[] = {"spin", 0};
        execv("SPIN.ELF", argv);
        sys_exit(9);
    }
    yields(30);
    CHECK(20, sys_kill(pid, SIGTERM) == 0 && sys_wait(pid, &st) == pid && st == 128 + SIGTERM); /* no handler left: the default */

    pid = fork();
    if (pid == 0)
    {
        signal(SIGINT, h);
        signal(SIGINT, 0);
        for (;;)
            sys_yield();
    }
    yields(30);
    CHECK(21, sys_kill(pid, SIGINT) == 0 && sys_wait(pid, &st) == pid && st == 128 + SIGINT);

    /* the handler can be replaced, and a process that is ended with SIGKILL while a handler is installed still ends */
    pid = fork();
    if (pid == 0)
    {
        signal(SIGTERM, h);
        signal(SIGINT, h);
        for (;;)
            sys_yield();
    }
    yields(30);
    CHECK(22, sys_kill(pid, SIGKILL) == 0 && sys_wait(pid, &st) == pid && st == 128 + SIGKILL);

    if (fail == 0)
        puts("sighand: all checks passed");
    else
        printf("sighand: check %d failed\n", fail);
    sys_exit(fail == 0 ? 53 : fail);
}
