/* HarisOS user program: checks semaphores: a lock around a counter in shared memory (four forked processes), a producer and a consumer
 * with a ring buffer in shared memory, waiting and being woken, a waiter that is killed, try-wait and the count. Exits with 52 if every
 * check holds, otherwise with the number of the first one that did not. */
#include "libc.h"

#define CHECK(n, cond)        \
    do                        \
    {                         \
        if (!(cond) && !fail) \
            fail = (n);       \
    } while (0)

typedef struct
{
    volatile int counter;
    volatile int head, tail;
    volatile int ring[8];
} Shared;

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0, st = 0;
    Shared *sh = sys_shm_map("semtest.shm", sizeof(Shared), SYS_CREATE);
    CHECK(1, (long long)sh != -1);

    /* a lock: only one process at a time does read - yield - write, so no increment is lost */
    long long lock = sys_sem_open("semtest.lock", SYS_CREATE, 1);
    CHECK(2, lock >= 3 && sys_sem_value((int)lock) == 1);
    long long kids[4];
    for (int i = 0; i < 4; i++)
    {
        long long c = fork();
        if (c == 0)
        {
            for (int k = 0; k < 50; k++)
            {
                sys_sem_wait((int)lock);
                int v = sh->counter;
                sys_yield(); /* the other processes run here: without the lock they would overwrite each other */
                sh->counter = v + 1;
                sys_sem_post((int)lock);
            }
            sys_exit(0);
        }
        kids[i] = c;
    }
    int all = 1;
    for (int i = 0; i < 4; i++)
        all = all && kids[i] > 0 && sys_wait(kids[i], &st) == kids[i] && st == 0;
    CHECK(3, all);
    CHECK(4, sh->counter == 200);                      /* no lost update */
    CHECK(5, sys_sem_value((int)lock) == 1);

    /* try-wait and counting */
    CHECK(6, sys_sem_try_wait((int)lock) == 0 && sys_sem_try_wait((int)lock) == 1 && sys_sem_value((int)lock) == 0);
    CHECK(7, sys_sem_post((int)lock) == 0 && sys_sem_post((int)lock) == 0 && sys_sem_value((int)lock) == 2);
    CHECK(8, sys_sem_wait((int)lock) == 0 && sys_sem_wait((int)lock) == 0 && sys_sem_try_wait((int)lock) == 1);

    /* producer and consumer: items counts what is in the ring, slots what is free; the producer is a forked child */
    long long items = sys_sem_open("semtest.items", SYS_CREATE, 0), slots = sys_sem_open("semtest.slots", SYS_CREATE, 8);
    CHECK(9, items >= 3 && slots >= 3);
    sh->head = sh->tail = 0;
    long long pid = fork();
    if (pid == 0)
    {
        for (int i = 0; i < 40; i++)
        {
            sys_sem_wait((int)slots);
            sh->ring[sh->head] = 1000 + i;
            sh->head = (sh->head + 1) % 8;
            sys_sem_post((int)items);
        }
        sys_exit(0);
    }
    int ordered = 1;
    for (int i = 0; i < 40; i++)
    {
        sys_sem_wait((int)items); /* sleeps while the ring is empty */
        ordered = ordered && sh->ring[sh->tail] == 1000 + i;
        sh->tail = (sh->tail + 1) % 8;
        sys_sem_post((int)slots);
    }
    CHECK(10, pid > 0 && ordered && sys_wait(pid, &st) == pid && st == 0);
    CHECK(11, sys_sem_value((int)items) == 0 && sys_sem_value((int)slots) == 8);

    /* a waiter that is woken by a post from another process */
    long long gate = sys_sem_open("semtest.gate", SYS_CREATE, 0);
    pid = fork();
    if (pid == 0)
    {
        sys_sem_wait((int)gate); /* blocked until the parent posts */
        sys_exit(sys_sem_value((int)gate) == 0 ? 33 : 34);
    }
    for (int i = 0; i < 30; i++)
        sys_yield(); /* let the child reach its wait */
    CHECK(12, sys_sem_value((int)gate) == 0 && sys_sem_post((int)gate) == 0);
    CHECK(13, sys_wait(pid, &st) == pid && st == 33);

    /* a waiter that is killed does not hold anything: the semaphore still works and nothing is left over */
    pid = fork();
    if (pid == 0)
    {
        sys_sem_wait((int)gate); /* the count is 0 again */
        sys_exit(0);
    }
    for (int i = 0; i < 30; i++)
        sys_yield();
    CHECK(14, sys_kill(pid, SIGKILL) == 0 && sys_wait(pid, &st) == pid && st == 128 + SIGKILL);
    CHECK(15, sys_sem_post((int)gate) == 0 && sys_sem_try_wait((int)gate) == 0 && sys_sem_try_wait((int)gate) == 1);

    /* names: found again by name, gone after unlink, the descriptor keeps it alive */
    long long again = sys_sem_open("semtest.gate", 0, 0);
    CHECK(16, again >= 3 && again != gate && sys_sem_open("semtest.gate", SYS_CREATE | SYS_EXCL, 0) == -1);
    CHECK(17, sys_sem_post((int)again) == 0 && sys_sem_value((int)gate) == 1);   /* the same semaphore */
    CHECK(18, sys_sem_unlink("semtest.gate") == 0 && sys_sem_open("semtest.gate", 0, 0) == -1 && sys_sem_unlink("semtest.gate") == -1);
    CHECK(19, sys_sem_value((int)gate) == 1 && sys_close((int)gate) == 0 && sys_sem_value((int)again) == 1);
    CHECK(20, sys_read((int)again, &st, 4) == -1 && sys_write((int)again, "x", 1) == -1);
    sys_close((int)again);

    sys_sem_unlink("semtest.lock");
    sys_sem_unlink("semtest.items");
    sys_sem_unlink("semtest.slots");
    sys_shm_unlink("semtest.shm");
    sys_close((int)lock);
    sys_close((int)items);
    sys_close((int)slots);
    sys_shm_unmap(sh);

    if (fail == 0)
        puts("semtest: all checks passed");
    else
        printf("semtest: check %d failed\n", fail);
    sys_exit(fail == 0 ? 52 : fail);
}
