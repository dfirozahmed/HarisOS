/* HarisOS user program: checks SYS_SPAWN and SYS_WAIT with the helper programs HELLO.ELF (exits with 42), ARGTEST.ELF (checks
 * its arguments) and CRASH.ELF (faults). Exits with 46 if every check holds, otherwise with the number of the first one that
 * did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0;
    int st = 12345;

    long long a = sys_spawn("HELLO.ELF", 0);
    CHECK(1, a > 0);
    CHECK(2, sys_wait(a, &st) == a && st == 42);
    CHECK(3, sys_wait(a, &st) == -1); /* once only */

    const char *args[] = {"argtest", "hello", "world wide", 0};
    long long b = sys_spawn("ARGTEST.ELF", args);
    CHECK(4, b > a); /* pids go up */
    CHECK(5, sys_wait(b, &st) == b && st == 77);
    long long c = sys_spawn("argtest.elf", 0); /* no argv: the path is argv[0], as written here */
    CHECK(6, c > b);
    CHECK(7, sys_wait(c, 0) == c); /* no place for the code */
    c = sys_spawn("ARGTEST.ELF", 0);
    CHECK(8, c > 0 && sys_wait(c, &st) == c && st == 55);

    /* things that must be refused */
    CHECK(9, sys_spawn("NO-SUCH-FILE.ELF", 0) == -1);
    CHECK(10, sys_spawn("", 0) == -1);
    CHECK(11, sys_spawn((const char *)0x1000, 0) == -1);
    CHECK(12, sys_spawn("ARGTEST.ELF", (const char *const *)0x1000) == -1);
    const char *badarg[] = {"x", (const char *)0x1000, 0};
    CHECK(13, sys_spawn("ARGTEST.ELF", badarg) == -1);
    const char *many[40];
    for (int i = 0; i < 39; i++)
        many[i] = "a";
    many[39] = 0;
    CHECK(14, sys_spawn("ARGTEST.ELF", many) == -1); /* more than PROCESS_ARGS_MAX */
    static char longarg[3000];
    memset(longarg, 'z', sizeof(longarg) - 1);
    const char *toolong[] = {longarg, 0};
    CHECK(15, sys_spawn("ARGTEST.ELF", toolong) == -1); /* more than PROCESS_ARGS_BYTES */
    long long left = sys_spawn("HELLO.ELF", 0);
    CHECK(17, left > 0);

    /* wait on things that are not outstanding children */
    CHECK(18, sys_wait(0, &st) == -1 && sys_wait(-1, &st) == -1 && sys_wait(999999, &st) == -1);
    CHECK(19, sys_wait(1, &st) == -1); /* a pid, but not ours */
    CHECK(20, sys_wait(left, (int *)0x1000) == -1);    /* bad pointer: refused ... */
    CHECK(21, sys_wait(left, &st) == left && st == 42); /* ... and the child can still be waited for */

    /* a child that crashes ends alone, with -1 */
    long long d = sys_spawn("CRASH.ELF", 0);
    CHECK(22, d > 0 && sys_wait(d, &st) == d && st == -1);

    /* many children at once: the list of children and the table of threads grow (there used to be a limit of 8 children) */
    long long kids[16];
    int n = 0;
    while (n < 12 && (kids[n] = sys_spawn("HELLO.ELF", 0)) > 0)
        n++;
    CHECK(23, n == 12); /* all twelve were started: nothing refused them */
    int good = 1;
    for (int i = 0; i < n; i++)
    {
        st = 0;
        good = good && sys_wait(kids[i], &st) == kids[i] && st == 42;
    }
    CHECK(25, good);
    long long again = sys_spawn("HELLO.ELF", 0); /* room again */
    CHECK(26, again > 0 && sys_wait(again, &st) == again && st == 42);

    if (fail == 0)
        puts("proctest: all checks passed");
    else
        printf("proctest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 46 : fail);
}
