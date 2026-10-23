/* HarisOS user program: checks fork() and execv() (with ARGTEST.ELF and UPPER.ELF as the programs to become). Exits with 50 if
 * every check holds, otherwise with the number of the first one that did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

static volatile int G = 1; /* a global: it lives in the program's data pages */

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0, st = 0;
    static char buf[256];

    /* a copy: the child sees what the parent had, and what it changes stays its own */
    volatile int local = 2;
    volatile int *heap = malloc(sizeof(int));
    *heap = 3;
    int pp[2];
    CHECK(1, sys_pipe(pp) == 0);
    long long me = getpid();
    long long pid = fork();
    CHECK(2, pid >= 0);
    if (pid == 0) /* the child */
    {
        int ok = G == 1 && local == 2 && *heap == 3 && getppid() == me && getpid() != me;
        G = 10;
        local = 20;
        *heap = 30;
        const char *msg = ok ? "child ok" : "child BAD";
        sys_write(pp[1], msg, strlen(msg));
        sys_exit(7);
    }
    CHECK(3, pid > 0 && pid != me);
    sys_close(pp[1]); /* the child holds its own copy of the write end: end of input comes when it is gone */
    CHECK(4, sys_wait(pid, &st) == pid && st == 7);
    CHECK(5, G == 1 && local == 2 && *heap == 3); /* nothing the child did reached us */
    long long n = sys_read(pp[0], buf, sizeof(buf) - 1);
    CHECK(6, n == 8 && memcmp(buf, "child ok", 8) == 0);
    CHECK(7, sys_read(pp[0], buf, sizeof(buf)) == 0); /* the child ended with its end of the pipe: end of input */
    sys_close(pp[0]);

    /* registers that live across fork() are right in both: a loop counter and a running sum */
    int sum = 0;
    long long kids[4];
    for (int i = 0; i < 4; i++)
    {
        long long c = fork();
        if (c == 0)
            sys_exit(10 + i);
        kids[i] = c;
    }
    for (int i = 0; i < 4; i++)
    {
        CHECK(8, kids[i] > 0 && sys_wait(kids[i], &st) == kids[i]);
        sum += st;
    }
    CHECK(9, sum == 10 + 11 + 12 + 13);

    /* a lot of memory: one megabyte of pattern, checked and changed by the child */
    unsigned char *big = malloc(1024 * 1024);
    CHECK(10, big != 0);
    for (int i = 0; i < 1024 * 1024; i++)
        big[i] = (unsigned char)(i * 31 + 7);
    pid = fork();
    if (pid == 0)
    {
        int ok = 1;
        for (int i = 0; i < 1024 * 1024; i += 4097)
            ok = ok && big[i] == (unsigned char)(i * 31 + 7);
        for (int i = 0; i < 1024 * 1024; i += 4096)
            big[i] = 0xEE;
        sys_exit(ok ? 0 : 1);
    }
    CHECK(11, pid > 0 && sys_wait(pid, &st) == pid && st == 0);
    int intact = 1;
    for (int i = 0; i < 1024 * 1024; i += 4096)
        intact = intact && big[i] == (unsigned char)(i * 31 + 7);
    CHECK(12, intact);
    free(big);

    /* fork, then become another program: the pid stays, and wait still finds it */
    pid = fork();
    if (pid == 0)
    {
        const char *argv[] = {"argtest", "hello", "world wide", 0};
        execv("ARGTEST.ELF", argv);
        sys_exit(1); /* only if exec failed */
    }
    CHECK(13, pid > 0 && sys_wait(pid, &st) == pid && st == 77);

    /* an exec that fails leaves the process as it was */
    pid = fork();
    if (pid == 0)
    {
        int r1 = execv("NO-SUCH-FILE.ELF", 0);
        int r2 = execv("KERNEL.EFI", 0); /* a file, but not an ELF program */
        int r3 = execv("/RAM Disk", 0);  /* not a file */
        sys_exit(r1 == -1 && r2 == -1 && r3 == -1 && G == 1 && local == 2 && *heap == 3 ? 5 : 6);
    }
    CHECK(14, pid > 0 && sys_wait(pid, &st) == pid && st == 5);

    /* the usual shell recipe: fork, point fd 0 and 1 at pipes, exec a filter */
    int a[2], b[2];
    CHECK(15, sys_pipe(a) == 0 && sys_pipe(b) == 0);
    pid = fork();
    if (pid == 0)
    {
        if (dup2(a[0], 0) != 0 || dup2(b[1], 1) != 1)
            sys_exit(2);
        sys_close(a[0]);
        sys_close(a[1]);
        sys_close(b[0]);
        sys_close(b[1]);
        execv("UPPER.ELF", 0);
        sys_exit(3);
    }
    CHECK(16, pid > 0);
    sys_close(a[0]);
    sys_close(b[1]);
    static char text[2000], want[2000], got[2100];
    for (int i = 0; i < (int)sizeof(text); i++)
    {
        text[i] = (char)('a' + (i * 5) % 26);
        want[i] = (char)toupper(text[i]);
    }
    CHECK(17, sys_write(a[1], text, sizeof(text)) == (long long)sizeof(text));
    sys_close(a[1]);
    int total = 0;
    while ((n = sys_read(b[0], got + total, sizeof(got) - (usize)total)) > 0)
        total += (int)n;
    CHECK(18, n == 0 && total == (int)sizeof(want) && memcmp(got, want, sizeof(want)) == 0);
    CHECK(19, sys_wait(pid, &st) == pid && st == 0);
    sys_close(b[0]);

    /* the limit on children, and every one of them can be waited for */
    long long many[12];
    int count = 0;
    while (count < 12)
    {
        long long c = fork();
        if (c == 0)
            sys_exit(0);
        if (c < 0)
            break;
        many[count++] = c;
    }
    CHECK(20, count >= 3 && count <= 8);
    int all = 1;
    for (int i = 0; i < count; i++)
        all = all && sys_wait(many[i], &st) == many[i] && st == 0;
    CHECK(21, all);
    pid = fork(); /* room again */
    if (pid == 0)
        sys_exit(0);
    CHECK(22, pid > 0 && sys_wait(pid, &st) == pid);

    /* a grandchild: a child that forks too, and its parent waits for it, not us */
    pid = fork();
    if (pid == 0)
    {
        long long g = fork();
        if (g == 0)
            sys_exit(21);
        int gst = 0;
        sys_exit(g > 0 && sys_wait(g, &gst) == g && gst == 21 ? 0 : 1);
    }
    CHECK(23, pid > 0 && sys_wait(pid, &st) == pid && st == 0);
    CHECK(24, sys_wait(pid, &st) == -1); /* once only */

    free((void *)heap);
    if (fail == 0)
        puts("forktest: all checks passed");
    else
        printf("forktest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 50 : fail);
}
