/* HarisOS user program: checks signals with SPIN.ELF as the victim: kill on a process that computes, one that waits for input,
 * one that waits on a pipe; SIGINT and SIGTERM that are ignored; SIGKILL that cannot be; the pipes of a killed process; ps.
 * Exits with 49 if every check holds, otherwise with the number of the first one that did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

static long long start(const char *mode)
{
    const char *argv[] = {"spin", mode, 0};
    return sys_spawn("SPIN.ELF", argv);
}

/* give the child the CPU for a while so it is really running / waiting when the signal comes */
static void settle(void)
{
    for (int i = 0; i < 20; i++)
        sys_yield();
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0, st = 0;

    /* a process that computes forever: SIGKILL */
    long long a = start("");
    CHECK(1, a > 0);
    settle();
    CHECK(2, sys_kill(a, 0) == 0);                       /* it is there */
    CHECK(3, sys_kill(a, SIGKILL) == 0);
    CHECK(4, sys_wait(a, &st) == a && st == 128 + SIGKILL);
    CHECK(5, sys_kill(a, 0) == -1 && sys_kill(a, SIGKILL) == -1); /* and now it is gone */

    /* waiting for input: SIGTERM */
    a = start("read");
    CHECK(6, a > 0);
    settle();
    CHECK(7, sys_kill(a, SIGTERM) == 0 && sys_wait(a, &st) == a && st == 128 + SIGTERM);

    /* waiting on a pipe */
    a = start("pipe");
    settle();
    CHECK(8, sys_kill(a, SIGINT) == 0 && sys_wait(a, &st) == a && st == 128 + SIGINT);

    /* ignored signals do nothing; SIGKILL cannot be ignored */
    a = start("int");
    settle();
    CHECK(9, sys_kill(a, SIGINT) == 0);                  /* accepted ... */
    settle();
    CHECK(10, sys_kill(a, 0) == 0);                      /* ... but the process lives on */
    CHECK(11, sys_kill(a, SIGTERM) == 0 && sys_wait(a, &st) == a && st == 128 + SIGTERM); /* TERM is not ignored here */
    a = start("all");
    settle();
    CHECK(12, sys_kill(a, SIGINT) == 0 && sys_kill(a, SIGTERM) == 0);
    settle();
    CHECK(13, sys_kill(a, 0) == 0);
    CHECK(14, sys_kill(a, SIGKILL) == 0 && sys_wait(a, &st) == a && st == 128 + SIGKILL);

    /* what a killed process held is let go: a reader of its pipe sees the end of input */
    int p[2];
    CHECK(15, sys_pipe(p) == 0);
    int io[3] = {-1, p[1], -1};
    a = sys_spawn_io("SPIN.ELF", 0, io);
    CHECK(16, a > 0);
    sys_close(p[1]); /* the child holds the only write end now */
    settle();
    CHECK(17, sys_kill(a, SIGKILL) == 0);
    char c;
    CHECK(18, sys_read(p[0], &c, 1) == 0);               /* end of input, not a hang */
    CHECK(19, sys_wait(a, &st) == a && st == 128 + SIGKILL);
    sys_close(p[0]);

    /* ps lists the running processes: this one is there, with its children */
    long long b = start("");
    PsInfo info;
    int found_self = 0, found_child = 0, count = 0;
    for (unsigned i = 0; sys_psinfo(i, &info) == 1; i++)
    {
        count++;
        if (strcmp(info.name, "SIGTEST.ELF") == 0)
            found_self = 1;
        if (info.pid == b)
        {
            found_child = 1;
            CHECK(20, info.ppid > 0);
        }
    }
    CHECK(21, count >= 2 && found_self && found_child);
    CHECK(22, sys_kill(b, SIGKILL) == 0 && sys_wait(b, &st) == b);

    /* things that must be refused */
    CHECK(23, sys_kill(0, SIGKILL) == -1 && sys_kill(-5, SIGKILL) == -1 && sys_kill(999999, SIGKILL) == -1);
    CHECK(24, sys_kill(1, 1) == -1 && sys_kill(1, 40) == -1);   /* no such signal */

    if (fail == 0)
        puts("sigtest: all checks passed");
    else
        printf("sigtest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 49 : fail);
}
