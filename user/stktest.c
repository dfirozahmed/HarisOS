/* HarisOS user program: the stack grows by itself, and arguments of any size fit. Exits with 55 if every check passed (else the number of the first
 * check that failed). Started with no arguments it runs the checks; started as  STKTEST.ELF child ...  it only checks its own arguments
 * (exit 0 if they are right), and as  STKTEST.ELF overflow  it recurses without end (the kernel must end it with a CPU exception, not hang). */
#include "libc.h"

#define CHECK(n, cond) do { if (!(cond)) { printf("stktest: check %d failed\n", n); sys_exit(n); } } while (0)

/* Each call uses about 600 bytes of stack: 1500 deep is about 900 KB, far more than the 16 KiB the stack starts with. */
static int depth(int n)
{
    volatile char pad[512];
    pad[0] = (char)n;
    pad[511] = (char)(n >> 8);
    return n == 0 ? 0 : depth(n - 1) + 1 + (pad[0] == (char)n ? 0 : 1000000);
}

/* A frame of 300 KB: __chkstk walks the stack pointer down through it. */
static long long big_frame(void)
{
    volatile char buf[300 * 1024];
    for (usize i = 0; i < sizeof(buf); i += 4096)
        buf[i] = (char)(i >> 12);
    buf[sizeof(buf) - 1] = 7;
    long long sum = 0;
    for (usize i = 0; i < sizeof(buf); i += 4096)
        sum += buf[i];
    return sum + buf[sizeof(buf) - 1];
}

#pragma clang diagnostic ignored "-Winfinite-recursion"
static int forever(int n)
{
    volatile char pad[4000];
    pad[0] = (char)n;
    return forever(n + 1) + pad[0];
}

#define NARGS 300
#define ARGLEN 100 /* 300 arguments of 100 bytes: 30 KB, twice the initial stack */

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "overflow") == 0)
    {
        sys_exit(forever(0)); /* does not get here */
    }
    if (argc >= 2 && strcmp(argv[1], "child") == 0)
    {
        int ok = argc == NARGS + 2 && argv[argc] == 0;
        for (int i = 0; ok && i < NARGS; i++)
        {
            const char *a = argv[i + 2];
            ok = strlen(a) == ARGLEN && a[0] == (char)('A' + i % 26) && a[ARGLEN - 1] == (char)('a' + i % 26);
        }
        sys_exit(ok ? 0 : 1);
    }

    CHECK(1, depth(1500) == 1500);
    long long expect = 0;
    for (int i = 0; i < 300 * 1024; i += 4096)
        expect += (char)(i >> 12);
    CHECK(2, big_frame() == expect + 7);
    CHECK(3, depth(300) == 300); /* and again: the pages are there already */

    /* a program started with 300 long arguments: the kernel makes the stack big enough for them */
    static char text[NARGS][ARGLEN + 1];
    const char *av[NARGS + 3];
    av[0] = "STKTEST.ELF";
    av[1] = "child";
    for (int i = 0; i < NARGS; i++)
    {
        memset(text[i], 'x', ARGLEN);
        text[i][0] = (char)('A' + i % 26);
        text[i][ARGLEN - 1] = (char)('a' + i % 26);
        text[i][ARGLEN] = 0;
        av[i + 2] = text[i];
    }
    av[NARGS + 2] = 0;
    int st = -5;
    long long pid = sys_spawn("STKTEST.ELF", av);
    CHECK(4, pid > 0 && sys_wait(pid, &st) == pid);
    CHECK(5, st == 0);

    /* a stack that grows without end is stopped at the limit: the process ends by a CPU exception (code -1), the system goes on */
    const char *ov[] = {"STKTEST.ELF", "overflow", 0};
    pid = sys_spawn("STKTEST.ELF", ov);
    CHECK(6, pid > 0 && sys_wait(pid, &st) == pid);
    CHECK(7, st == -1);

    printf("stktest: all checks passed\n");
    sys_exit(55);
}
