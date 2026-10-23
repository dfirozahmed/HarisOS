/* Host test for semaphores (kernel/core/sem.c): counting, blocking protocol, wake-ups, names, references, overflow. */
#include <stdio.h>
#include <string.h>
#include "sem.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int WOKEN[64], WOKEN_N;
void task_wake(int id) { if (WOKEN_N < 64) WOKEN[WOKEN_N++] = id; }
static int woke(int id) { for (int i = 0; i < WOKEN_N; i++) if (WOKEN[i] == id) return 1; return 0; }

int main(void)
{
    CHECK(sem_valid_name("lock") && !sem_valid_name("") && !sem_valid_name("a/b"));
    CHECK(sem_open("lock", 0, 0) == -1);                                  /* not there, no CREATE */
    int s = sem_open("lock", SEM_CREATE, 2);
    CHECK(s >= 0 && sem_value(s) == 2 && sem_in_use() == 1);
    CHECK(sem_open("lock", SEM_CREATE, 9) == s && sem_value(s) == 2);      /* it exists: its count stays */
    CHECK(sem_open("lock", SEM_CREATE | SEM_EXCL, 0) == -1 && sem_open("lock", 0, 0) == s);
    sem_ref(s);

    /* counting */
    CHECK(sem_trywait(s) == 0 && sem_trywait(s) == 0 && sem_value(s) == 0);
    CHECK(sem_trywait(s) == SEM_WOULDBLOCK && sem_value(s) == 0);
    CHECK(sem_post(s) == 0 && sem_post(s) == 0 && sem_post(s) == 0 && sem_value(s) == 3);
    CHECK(sem_trywait(s) == 0 && sem_value(s) == 2);

    /* a post wakes the sleepers once; a failed wait registers nobody by itself */
    sem_trywait(s);
    sem_trywait(s);
    WOKEN_N = 0;
    sem_wait_for(s, 3);
    sem_wait_for(s, 5);
    sem_wait_for(s, 99);                                                  /* out of range: ignored */
    sem_wait_for(s, -1);
    CHECK(WOKEN_N == 0);
    CHECK(sem_post(s) == 0 && woke(3) && woke(5) && WOKEN_N == 2);
    WOKEN_N = 0;
    CHECK(sem_post(s) == 0 && WOKEN_N == 0);                              /* the registration was used up */

    /* the largest count, and not past it */
    int big = sem_open("big", SEM_CREATE, SEM_VALUE_MAX);
    CHECK(big >= 0 && sem_value(big) == SEM_VALUE_MAX && sem_post(big) == -1 && sem_value(big) == SEM_VALUE_MAX);
    CHECK(sem_open("toobig", SEM_CREATE, (unsigned)SEM_VALUE_MAX + 1) == -1);
    CHECK(sem_trywait(big) == 0 && sem_post(big) == 0);

    /* unlink and references */
    CHECK(sem_unlink("big") == 0 && sem_open("big", 0, 0) == -1 && sem_value(big) == -1 && sem_unlink("big") == -1);   /* nobody held it */
    CHECK(sem_unlink("lock") == 0 && sem_open("lock", 0, 0) == -1);        /* held: the semaphore stays */
    CHECK(sem_value(s) >= 0 && sem_trywait(s) == 0);
    int fresh = sem_open("lock", SEM_CREATE, 0);                          /* the name is free again */
    CHECK(fresh >= 0 && fresh != s && sem_value(fresh) == 0);
    sem_unref(s);
    CHECK(!sem_valid(s) && sem_in_use() == 1);
    sem_unref(s);                                                         /* a stale number is harmless */
    sem_ref(s);
    CHECK(sem_trywait(s) == -1 && sem_post(s) == -1 && sem_in_use() == 1);
    CHECK(sem_unlink("lock") == 0 && sem_in_use() == 0);

    /* all the slots */
    char nm[8];
    int made = 0;
    for (int i = 0; i <= SEM_MAX; i++)
    {
        snprintf(nm, sizeof(nm), "s%d", i);
        if (sem_open(nm, SEM_CREATE, 0) >= 0)
            made++;
    }
    CHECK(made == SEM_MAX);
    for (int i = 0; i < SEM_MAX; i++)
    {
        snprintf(nm, sizeof(nm), "s%d", i);
        sem_unlink(nm);
    }
    CHECK(sem_in_use() == 0);

    if (fails == 0)
        printf("sem tests passed\n");
    return fails ? 1 : 0;
}
