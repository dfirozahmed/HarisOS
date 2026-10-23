/* HarisOS user program: checks named mutexes and condition variables: a mutex around a counter in shared memory (four forked processes),
 * try-lock, a bounded buffer with a producer and a consumer that wait on conditions, broadcast, a holder that is killed, and names.
 * Exits with 54 if every check holds, otherwise with the number of the first one that did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

#define RING 4

typedef struct
{
    volatile int counter;
    volatile int count, head, tail; /* the bounded buffer */
    volatile int ring[RING];
    volatile int go;
} Shared;

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0, st = 0;
    Shared *sh = sys_shm_map("muxtest.shm", sizeof(Shared), SYS_CREATE);
    CHECK(1, (long long)sh != -1);

    /* a mutex: only one process at a time does read - yield - write, so no increment is lost */
    long long mtx = sys_mutex_open("muxtest.m", SYS_CREATE);
    CHECK(2, mtx >= 3);
    long long kids[4];
    for (int i = 0; i < 4; i++)
    {
        long long c = fork();
        if (c == 0)
        {
            for (int k = 0; k < 50; k++)
            {
                sys_mutex_lock((int)mtx);
                int v = sh->counter;
                sys_yield(); /* the others run here: without the mutex they would overwrite each other */
                sh->counter = v + 1;
                sys_mutex_unlock((int)mtx);
            }
            sys_exit(0);
        }
        kids[i] = c;
    }
    int all = 1;
    for (int i = 0; i < 4; i++)
        all = all && kids[i] > 0 && sys_wait(kids[i], &st) == kids[i] && st == 0;
    CHECK(3, all);
    CHECK(4, sh->counter == 200);

    /* try-lock, and the rules of unlock */
    CHECK(5, sys_mutex_try_lock((int)mtx) == 0 && sys_mutex_try_lock((int)mtx) == -1); /* ours already */
    long long pid = fork();
    if (pid == 0)
        sys_exit(sys_mutex_try_lock((int)mtx) == 1 && sys_mutex_unlock((int)mtx) == -1 ? 41 : 42); /* held by the parent */
    CHECK(6, pid > 0 && sys_wait(pid, &st) == pid && st == 41);
    CHECK(7, sys_mutex_unlock((int)mtx) == 0 && sys_mutex_unlock((int)mtx) == -1);

    /* a bounded buffer: the producer waits while it is full, the consumer while it is empty */
    long long lock = sys_mutex_open("muxtest.buf", SYS_CREATE);
    long long not_full = sys_cond_open("muxtest.nf", SYS_CREATE), not_empty = sys_cond_open("muxtest.ne", SYS_CREATE);
    CHECK(8, lock >= 3 && not_full >= 3 && not_empty >= 3);
    sh->count = sh->head = sh->tail = 0;
    pid = fork();
    if (pid == 0)
    {
        for (int i = 0; i < 40; i++)
        {
            sys_mutex_lock((int)lock);
            while (sh->count == RING)
                sys_cond_wait((int)not_full, (int)lock);
            sh->ring[sh->head] = 1000 + i;
            sh->head = (sh->head + 1) % RING;
            sh->count++;
            sys_cond_signal((int)not_empty);
            sys_mutex_unlock((int)lock);
        }
        sys_exit(0);
    }
    int ordered = 1;
    for (int i = 0; i < 40; i++)
    {
        sys_mutex_lock((int)lock);
        while (sh->count == 0)
            sys_cond_wait((int)not_empty, (int)lock);
        ordered = ordered && sh->ring[sh->tail] == 1000 + i;
        sh->tail = (sh->tail + 1) % RING;
        sh->count--;
        sys_cond_signal((int)not_full);
        sys_mutex_unlock((int)lock);
    }
    CHECK(9, pid > 0 && ordered && sys_wait(pid, &st) == pid && st == 0);
    CHECK(10, sh->count == 0);

    /* broadcast: three processes wait for the same flag; one broadcast lets them all go */
    long long gate = sys_cond_open("muxtest.gate", SYS_CREATE);
    sh->go = 0;
    for (int i = 0; i < 3; i++)
    {
        long long c = fork();
        if (c == 0)
        {
            sys_mutex_lock((int)lock);
            while (!sh->go)
                sys_cond_wait((int)gate, (int)lock);
            sys_mutex_unlock((int)lock);
            sys_exit(33);
        }
        kids[i] = c;
    }
    for (int i = 0; i < 30; i++)
        sys_yield(); /* let them all reach their wait */
    sys_mutex_lock((int)lock);
    sh->go = 1;
    sys_cond_broadcast((int)gate);
    sys_mutex_unlock((int)lock);
    all = 1;
    for (int i = 0; i < 3; i++)
        all = all && kids[i] > 0 && sys_wait(kids[i], &st) == kids[i] && st == 33;
    CHECK(11, all);

    /* waiting without holding the mutex, and the wrong kind of descriptor */
    CHECK(12, sys_cond_wait((int)gate, (int)lock) == -1);
    CHECK(13, sys_cond_wait((int)lock, (int)gate) == -1 && sys_mutex_lock((int)gate) == -1);

    /* a holder that is killed does not keep the mutex */
    pid = fork();
    if (pid == 0)
    {
        sys_mutex_lock((int)lock);
        for (;;)
            sys_yield(); /* never lets go */
    }
    for (int i = 0; i < 30; i++)
        sys_yield();
    CHECK(14, sys_mutex_try_lock((int)lock) == 1); /* held by the child */
    CHECK(15, sys_kill(pid, SIGKILL) == 0 && sys_wait(pid, &st) == pid && st == 128 + SIGKILL);
    CHECK(16, sys_mutex_lock((int)lock) == 0 && sys_mutex_unlock((int)lock) == 0);

    /* names: found again by name, a mutex and a condition variable may share one, gone after unlink */
    long long again = sys_mutex_open("muxtest.m", 0);
    CHECK(17, again >= 3 && again != mtx && sys_mutex_open("muxtest.m", SYS_CREATE | SYS_EXCL) == -1);
    CHECK(18, sys_cond_open("muxtest.m", 0) == -1); /* no condition variable of that name */
    CHECK(19, sys_mutex_try_lock((int)mtx) == 0 && sys_mutex_try_lock((int)again) == -1 && sys_mutex_unlock((int)again) == 0); /* the same mutex */
    CHECK(20, sys_mutex_unlink("muxtest.m") == 0 && sys_mutex_open("muxtest.m", 0) == -1 && sys_mutex_unlink("muxtest.m") == -1);
    CHECK(21, sys_mutex_try_lock((int)again) == 0 && sys_mutex_unlock((int)again) == 0); /* the descriptors keep it alive */
    CHECK(22, sys_read((int)again, &st, 4) == -1 && sys_write((int)again, "x", 1) == -1);

    /* scheduling priority (SYS_SETPRIO): read, set, give back; bad values and strangers are refused. A mutex holder may run higher for a
     * while (priority inheritance), but that is not what this reads. */
    CHECK(23, sys_getprio(0) == 2 && sys_setprio(0, 3) == 2 && sys_getprio(0) == 3 && sys_setprio(0, 2) == 3);
    CHECK(24, sys_setprio(0, 5) == -1 && sys_setprio(0x7FFFFFF0, 1) == -1 && sys_getprio(0) == 2);

    sys_mutex_unlink("muxtest.buf");
    sys_cond_unlink("muxtest.nf");
    sys_cond_unlink("muxtest.ne");
    sys_cond_unlink("muxtest.gate");
    sys_shm_unlink("muxtest.shm");
    sys_close((int)mtx);
    sys_close((int)again);
    sys_close((int)lock);
    sys_close((int)not_full);
    sys_close((int)not_empty);
    sys_close((int)gate);
    sys_shm_unmap(sh);

    if (fail == 0)
        puts("muxtest: all checks passed");
    else
        printf("muxtest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 54 : fail);
}
