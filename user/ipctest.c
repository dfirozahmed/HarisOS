/* HarisOS user program: checks shared memory and message queues, between a parent and its forked children and with IPCCHILD.ELF,
 * a program that shares nothing with it but the names. Exits with 51 if every check holds, otherwise with the number of the first
 * one that did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

static long long spawn1(const char *mode)
{
    const char *argv[] = {"ipcchild", mode, 0};
    return sys_spawn("IPCCHILD.ELF", argv);
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0, st = 0;

    /* ---- shared memory ---- */
    volatile unsigned char *m = sys_shm_map("ipctest.shm", 8192, SYS_CREATE);
    CHECK(1, (long long)m != -1 && ((usize)m & 4095) == 0);
    CHECK(2, sys_shm_size((void *)m) == 8192);
    int zero = 1;
    for (int i = 0; i < 8192; i++)
        zero = zero && m[i] == 0;
    CHECK(3, zero);                                                 /* a new segment is zeros */
    CHECK(4, (long long)sys_shm_map("ipctest.shm", 8192, SYS_CREATE | SYS_EXCL) == -1);  /* it exists */
    CHECK(5, (long long)sys_shm_map("no.such.segment", 4096, 0) == -1);
    m[0] = 'P';
    m[4095] = 'Q';
    m[4096] = 'R';

    /* the same segment mapped twice in one process: two addresses, one set of bytes */
    volatile unsigned char *m2 = sys_shm_map("ipctest.shm", 0, 0);
    CHECK(6, (long long)m2 != -1 && m2 != m && m2[0] == 'P' && m2[4096] == 'R');
    m2[10] = 0x5A;
    CHECK(7, m[10] == 0x5A);

    /* fork: unlike ordinary memory, the segment is NOT copied: parent and child see each other's writes */
    static volatile int ordinary = 1;
    long long pid = fork();
    if (pid == 0)
    {
        int ok = m[0] == 'P' && m[10] == 0x5A;
        m[20] = 77;      /* shared: the parent will see it */
        ordinary = 99;   /* copied: the parent will not */
        sys_exit(ok ? 0 : 1);
    }
    CHECK(8, pid > 0 && sys_wait(pid, &st) == pid && st == 0);
    CHECK(9, m[20] == 77 && ordinary == 1);

    /* a program that was started separately finds it by name */
    pid = spawn1("shm");
    CHECK(10, pid > 0 && sys_wait(pid, &st) == pid && st == 0);
    CHECK(11, m[1] == 'C' && m[8191] == 'Z');

    /* unmap one of the mappings: the other still works; unlink: the name goes, the memory stays while mapped */
    CHECK(12, sys_shm_unmap((void *)m2) == 0 && sys_shm_unmap((void *)m2) == -1 && sys_shm_size((void *)m2) == -1);
    CHECK(13, m[0] == 'P');
    CHECK(14, sys_shm_unlink("ipctest.shm") == 0 && sys_shm_unlink("ipctest.shm") == -1);
    CHECK(15, (long long)sys_shm_map("ipctest.shm", 0, 0) == -1);   /* no name any more */
    CHECK(16, m[0] == 'P' && m[4096] == 'R');                         /* but our mapping is intact */
    CHECK(17, sys_shm_unmap((void *)m) == 0);

    /* limits and bad arguments */
    CHECK(18, (long long)sys_shm_map("big", 1024 * 1024 + 1, SYS_CREATE) == -1);
    CHECK(19, (long long)sys_shm_map("bad/name", 4096, SYS_CREATE) == -1 && (long long)sys_shm_map("", 4096, SYS_CREATE) == -1);
    volatile unsigned char *mb = sys_shm_map("megabyte", 1024 * 1024, SYS_CREATE);
    CHECK(20, (long long)mb != -1 && sys_shm_size((void *)mb) == 1024 * 1024);
    mb[0] = 1;
    mb[1024 * 1024 - 1] = 2;
    CHECK(21, mb[0] == 1 && mb[1024 * 1024 - 1] == 2);
    sys_shm_unlink("megabyte");
    sys_shm_unmap((void *)mb);

    /* ---- message queues ---- */
    long long q = sys_mq_open("ipctest.q", SYS_CREATE), r = sys_mq_open("ipctest.r", SYS_CREATE);
    CHECK(22, q >= 3 && r >= 3 && q != r);
    CHECK(23, sys_mq_open("ipctest.q", SYS_CREATE | SYS_EXCL) == -1 && sys_mq_open("missing.q", 0) == -1);
    static char buf[300];

    /* boundaries and order */
    CHECK(24, sys_write((int)q, "a", 1) == 1 && sys_write((int)q, "bb", 2) == 2 && sys_write((int)q, "ccc", 3) == 3);
    CHECK(25, sys_read((int)q, buf, sizeof(buf)) == 1 && buf[0] == 'a');
    CHECK(26, sys_read((int)q, buf, sizeof(buf)) == 2 && memcmp(buf, "bb", 2) == 0);
    CHECK(27, sys_read((int)q, buf, sizeof(buf)) == 3 && memcmp(buf, "ccc", 3) == 0);

    /* a buffer that is too small refuses and keeps the message; sizes: 1 .. 256 bytes */
    CHECK(28, sys_write((int)q, "0123456789", 10) == 10 && sys_read((int)q, buf, 4) == -1);
    CHECK(29, sys_read((int)q, buf, sizeof(buf)) == 10 && memcmp(buf, "0123456789", 10) == 0);
    static char max[257];
    memset(max, 'm', sizeof(max));
    CHECK(30, sys_write((int)q, max, 256) == 256 && sys_write((int)q, max, 257) == -1 && sys_write((int)q, max, 0) == 0);
    CHECK(31, sys_read((int)q, buf, sizeof(buf)) == 256 && memcmp(buf, max, 256) == 0);   /* the empty write sent nothing */

    /* a child sends, the parent waits in read (the queue is empty at first); the descriptor is inherited */
    pid = fork();
    if (pid == 0)
    {
        for (int i = 0; i < 3; i++)
        {
            for (int k = 0; k < 20; k++)
                sys_yield();
            char msg[4] = {'m', (char)('0' + i), 0, 0};
            if (sys_write((int)q, msg, 2) != 2)
                sys_exit(1);
        }
        sys_exit(0);
    }
    int ordered = 1;
    for (int i = 0; i < 3; i++)
        ordered = ordered && sys_read((int)q, buf, sizeof(buf)) == 2 && buf[0] == 'm' && buf[1] == '0' + i;
    CHECK(32, pid > 0 && ordered && sys_wait(pid, &st) == pid && st == 0);

    /* more messages than the queue holds (16): the sender waits for the receiver to make room, and nothing is lost or reordered */
    pid = fork();
    if (pid == 0)
    {
        int ok = 1;
        for (int i = 0; i < 40; i++)
        {
            char msg[8];
            long long n = sys_read((int)q, msg, sizeof(msg));
            ok = ok && n == 3 && msg[0] == 'n' && msg[1] == '0' + i / 10 && msg[2] == '0' + i % 10;
        }
        sys_exit(ok ? 0 : 1);
    }
    int sent = 1;
    for (int i = 0; i < 40; i++)
    {
        char msg[3] = {'n', (char)('0' + i / 10), (char)('0' + i % 10)};
        sent = sent && sys_write((int)q, msg, 3) == 3;
    }
    CHECK(33, pid > 0 && sent && sys_wait(pid, &st) == pid && st == 0);

    /* ping-pong with a program that was started separately: it finds both queues by name */
    pid = spawn1("mq");
    CHECK(34, pid > 0);
    int pong = 1;
    for (int i = 0; i < 5; i++)
    {
        char msg[16];
        int len = snprintf(msg, sizeof(msg), "ping %d", i);
        pong = pong && sys_write((int)q, msg, (usize)len) == len;
        char want[16];
        snprintf(want, sizeof(want), "pong %d", i);
        long long n = sys_read((int)r, buf, sizeof(buf));
        pong = pong && n == len && memcmp(buf, want, (usize)len) == 0;
    }
    CHECK(35, pong && sys_wait(pid, &st) == pid && st == 0);

    /* unlink: the name goes, the descriptors keep working; closing the last one frees the queue */
    CHECK(36, sys_mq_unlink("ipctest.q") == 0 && sys_mq_unlink("ipctest.q") == -1 && sys_mq_open("ipctest.q", 0) == -1);
    CHECK(37, sys_write((int)q, "last", 4) == 4 && sys_read((int)q, buf, sizeof(buf)) == 4);
    CHECK(38, sys_mq_unlink("ipctest.r") == 0);
    CHECK(39, sys_close((int)q) == 0 && sys_close((int)r) == 0 && sys_read((int)q, buf, 4) == -1);
    CHECK(40, sys_seek((int)sys_mq_open("ipctest.s", SYS_CREATE), 0, 0) == -1);   /* a queue has no position */
    sys_mq_unlink("ipctest.s");

    /* exec drops the mappings of the old program (the kernel self test checks that no segment is left afterwards) */
    pid = fork();
    if (pid == 0)
    {
        volatile unsigned char *e = sys_shm_map("ipctest.exec", 4096, SYS_CREATE);
        if ((long long)e == -1)
            sys_exit(1);
        sys_shm_unlink("ipctest.exec"); /* only the mapping keeps it alive now */
        execv("ARGTEST.ELF", 0);
        sys_exit(2);
    }
    CHECK(41, pid > 0 && sys_wait(pid, &st) == pid && st == 55);

    if (fail == 0)
        puts("ipctest: all checks passed");
    else
        printf("ipctest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 51 : fail);
}
