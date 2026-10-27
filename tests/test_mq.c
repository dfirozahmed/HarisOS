/* Host test for message queues (kernel/ipc/mq.c): boundaries, order, depth, sizes, names, wake-ups, references. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mq.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* the kernel heap: host memory, counted, with a switch that makes it fail */
static int HEAP_BLOCKS, HEAP_FAIL;
void *kmalloc(unsigned long long size)
{
    if (HEAP_FAIL)
        return 0;
    void *p = calloc(1, size ? (size_t)size : 1);
    if (p)
        HEAP_BLOCKS++;
    return p;
}
void kfree(void *p)
{
    if (p)
    {
        free(p);
        HEAP_BLOCKS--;
    }
}
void *krealloc(void *p, unsigned long long size)
{
    if (HEAP_FAIL)
        return 0;
    void *n = realloc(p, size ? (size_t)size : 1);
    if (n && !p)
        HEAP_BLOCKS++;
    return n;
}

static int WOKEN[64], WOKEN_N;
void task_wake(int id) { if (WOKEN_N < 64) WOKEN[WOKEN_N++] = id; }
static int woke(int id) { for (int i = 0; i < WOKEN_N; i++) if (WOKEN[i] == id) return 1; return 0; }

int main(void)
{
    char buf[MQ_MSG_MAX + 8];

    CHECK(mq_valid_name("jobs") && !mq_valid_name("") && !mq_valid_name("a/b"));
    CHECK(mq_open("jobs", 0) == -1);                          /* not there, no CREATE */
    int q = mq_open("jobs", MQ_CREATE);
    CHECK(q >= 0 && mq_in_use() == 1 && mq_open("jobs", 0) == q && mq_open("jobs", MQ_CREATE) == q);
    CHECK(mq_open("jobs", MQ_CREATE | MQ_EXCL) == -1);
    mq_ref(q);

    /* message boundaries and order */
    CHECK(mq_recv(q, buf, sizeof(buf)) == MQ_WOULDBLOCK);
    CHECK(mq_send(q, "first", 5) == 5 && mq_send(q, "2nd", 3) == 3 && mq_send(q, "x", 1) == 1);
    CHECK(mq_recv(q, buf, sizeof(buf)) == 5 && memcmp(buf, "first", 5) == 0);
    CHECK(mq_recv(q, buf, sizeof(buf)) == 3 && memcmp(buf, "2nd", 3) == 0);
    CHECK(mq_recv(q, buf, sizeof(buf)) == 1 && buf[0] == 'x');
    CHECK(mq_recv(q, buf, sizeof(buf)) == MQ_WOULDBLOCK);

    /* sizes: 1 .. MQ_MSG_MAX; a small buffer does not lose the message */
    static char big[MQ_MSG_MAX + 1];
    memset(big, 'z', sizeof(big));
    CHECK(mq_send(q, big, 0) == -1 && mq_send(q, big, MQ_MSG_MAX + 1) == -1);
    CHECK(mq_send(q, big, MQ_MSG_MAX) == MQ_MSG_MAX);
    CHECK(mq_recv(q, buf, 10) == -1);                          /* the buffer is too small ... */
    CHECK(mq_recv(q, buf, MQ_MSG_MAX) == MQ_MSG_MAX && memcmp(buf, big, MQ_MSG_MAX) == 0); /* ... and it is still there */

    /* full: MQ_DEPTH messages, then the sender must wait; the ring wraps */
    for (int i = 0; i < MQ_DEPTH; i++)
    {
        char m = (char)('A' + i);
        CHECK(mq_send(q, &m, 1) == 1);
    }
    CHECK(mq_send(q, "late", 4) == MQ_WOULDBLOCK);
    for (int i = 0; i < 5; i++)
        CHECK(mq_recv(q, buf, 1) == 1 && buf[0] == 'A' + i);
    for (int i = 0; i < 5; i++)
    {
        char m = (char)('a' + i);
        CHECK(mq_send(q, &m, 1) == 1);                         /* wraps around the ring */
    }
    CHECK(mq_send(q, "x", 1) == MQ_WOULDBLOCK);
    int ok = 1;
    for (int i = 5; i < MQ_DEPTH; i++)
        ok = ok && mq_recv(q, buf, 1) == 1 && buf[0] == 'A' + i;
    for (int i = 0; i < 5; i++)
        ok = ok && mq_recv(q, buf, 1) == 1 && buf[0] == 'a' + i;
    CHECK(ok && mq_recv(q, buf, 1) == MQ_WOULDBLOCK);

    /* wake-ups */
    WOKEN_N = 0;
    mq_wait(q, 0, 4);
    mq_wait(q, 1, 6);
    CHECK(mq_send(q, "w", 1) == 1 && woke(4) && !woke(6));    /* a sender wakes a receiver */
    WOKEN_N = 0;
    CHECK(mq_recv(q, buf, 1) == 1 && woke(6) && !woke(4));     /* a receive wakes a sender */
    mq_wait(q, 0, 99);                                         /* out of range: ignored */
    mq_wait(q, 0, -1);

    /* names, references, unlink */
    int other = mq_open("other", MQ_CREATE);
    CHECK(other >= 0 && other != q && mq_in_use() == 2);
    CHECK(mq_send(other, "o", 1) == 1 && mq_recv(q, buf, 1) == MQ_WOULDBLOCK);   /* queues are separate */
    CHECK(mq_unlink("other") == 0 && mq_in_use() == 1 && mq_unlink("other") == -1); /* nobody holds it: gone at once */
    CHECK(mq_send(other, "x", 1) == -1 && mq_recv(other, buf, 1) == -1);              /* a stale number is refused */
    CHECK(mq_unlink("jobs") == 0 && mq_open("jobs", 0) == -1 && mq_in_use() == 1);    /* held: the queue stays */
    CHECK(mq_send(q, "still", 5) == 5 && mq_recv(q, buf, 8) == 5);                    /* and works */
    int fresh = mq_open("jobs", MQ_CREATE);                                           /* the name is free again */
    CHECK(fresh >= 0 && fresh != q && mq_recv(fresh, buf, 8) == MQ_WOULDBLOCK);
    mq_unref(q);
    CHECK(mq_in_use() == 1 && !mq_valid(q));                                           /* the last holder left: freed */
    mq_unref(q);
    mq_ref(q);
    CHECK(mq_in_use() == 1);
    CHECK(mq_unlink("jobs") == 0 && mq_in_use() == 0);

    /* all the slots */
    char nm[8];
    int made = 0;
    for (int i = 0; i <= MQ_MAX; i++)
    {
        snprintf(nm, sizeof(nm), "q%d", i);
        if (mq_open(nm, MQ_CREATE) >= 0)
            made++;
    }
    CHECK(made == MQ_MAX);
    for (int i = 0; i < MQ_MAX; i++)
    {
        snprintf(nm, sizeof(nm), "q%d", i);
        mq_unlink(nm);
    }
    CHECK(mq_in_use() == 0);

    if (fails == 0)
        printf("mq tests passed\n");
    return fails ? 1 : 0;
}
