/* HarisOS user program: checks pipes (SYS_PIPE and the descriptors SYS_SPAWN passes on), with UPPER.ELF as the other end.
 * Exits with 48 if every check holds, otherwise with the number of the first one that did not. */
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
    static char buf[4096]; /* (a big stack frame would need __chkstk, which a program has no use for) */
    int a[2], b[2], st = 0;

    /* inside one process */
    CHECK(1, sys_pipe(a) == 0 && a[0] >= 3 && a[1] >= 3 && a[0] != a[1]);
    CHECK(2, sys_write(a[1], "abc", 3) == 3 && sys_read(a[0], buf, 10) == 3 && memcmp(buf, "abc", 3) == 0);
    CHECK(3, sys_write(a[0], "x", 1) == -1 && sys_read(a[1], buf, 1) == -1); /* the wrong end */
    CHECK(4, sys_seek(a[0], 0, 0) == -1);
    CHECK(5, sys_write(a[1], "tail", 4) == 4 && sys_close(a[1]) == 0);       /* the writer ends: what is left, then end */
    CHECK(6, sys_read(a[0], buf, 10) == 4 && sys_read(a[0], buf, 10) == 0 && sys_read(a[0], buf, 10) == 0);
    CHECK(7, sys_close(a[0]) == 0 && sys_read(a[0], buf, 1) == -1);          /* closed: not a descriptor any more */
    CHECK(8, sys_pipe(a) == 0 && sys_close(a[0]) == 0 && sys_write(a[1], "x", 1) == -1); /* nobody can read it */
    sys_close(a[1]);

    /* a filter: UPPER reads pipe A and writes pipe B; this process feeds A and reads B */
    CHECK(9, sys_pipe(a) == 0 && sys_pipe(b) == 0);
    int io[3] = {a[0], b[1], -1};
    long long pid = sys_spawn_io("UPPER.ELF", 0, io);
    CHECK(10, pid > 0);
    sys_close(a[0]); /* the child has its own copies: keeping ours would stop the end of input from ever coming */
    sys_close(b[1]);
    static char text[3000], want[3000], got[3100];
    for (int i = 0; i < (int)sizeof(text); i++)
    {
        text[i] = (char)('a' + (i * 7) % 26);
        want[i] = (char)toupper(text[i]);
    }
    CHECK(11, sys_write(a[1], text, sizeof(text)) == (long long)sizeof(text));
    CHECK(12, sys_close(a[1]) == 0);
    int total = 0;
    long long n;
    while ((n = sys_read(b[0], got + total, (usize)(sizeof(got) - (usize)total))) > 0)
        total += (int)n;
    CHECK(13, n == 0 && total == (int)sizeof(want) && memcmp(got, want, sizeof(want)) == 0);
    CHECK(14, sys_wait(pid, &st) == pid && st == 0);
    sys_close(b[0]);

    /* a child that outlives its reader: UPPER's write fails, it exits 1 instead of hanging */
    CHECK(15, sys_pipe(a) == 0 && sys_pipe(b) == 0);
    int io2[3] = {a[0], b[1], -1};
    pid = sys_spawn_io("UPPER.ELF", 0, io2);
    CHECK(16, pid > 0);
    sys_close(a[0]);
    sys_close(b[1]);
    sys_close(b[0]); /* nobody will read the output */
    CHECK(17, sys_write(a[1], "data", 4) == 4);
    sys_close(a[1]);
    CHECK(18, sys_wait(pid, &st) == pid && st == 1);

    /* the wrong end for a child's input / output is refused, and nothing leaks */
    CHECK(19, sys_pipe(a) == 0);
    int bad[3] = {a[1], -1, -1};
    CHECK(20, sys_spawn_io("UPPER.ELF", 0, bad) == -1);
    int bad2[3] = {-1, a[0], -1};
    CHECK(21, sys_spawn_io("UPPER.ELF", 0, bad2) == -1);
    sys_close(a[0]);
    sys_close(a[1]);

    /* every free descriptor taken: no pipe, and what failed leaves nothing behind */
    int fds[8], used = 0;
    for (; used < 8; used++)
        if ((fds[used] = (int)sys_open("HELLO.ELF")) < 0)
            break;
    CHECK(22, used >= 6 && sys_pipe(a) == -1);
    for (int i = 0; i < used; i++)
        sys_close(fds[i]);
    CHECK(23, sys_pipe(a) == 0);
    sys_close(a[0]);
    sys_close(a[1]);

    if (fail == 0)
        puts("pipetest: all checks passed");
    else
        printf("pipetest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 48 : fail);
}
