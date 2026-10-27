/* Host test for pipes (kernel/ipc/pipe.c): the byte queue, end of input, broken pipes, wake-ups, slots. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pipe.h"

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
    char buf[PIPE_SIZE + 16];

    /* basics: nothing yet, write, read back in order, in pieces */
    int p = pipe_new();
    CHECK(p >= 0 && pipe_valid(p) && pipe_in_use() == 1);
    pipe_ref(p, 0);
    pipe_ref(p, 1);
    CHECK(pipe_read(p, buf, 10) == PIPE_WOULDBLOCK);                  /* empty, a writer exists */
    CHECK(pipe_write(p, "hello world", 11) == 11);
    CHECK(pipe_read(p, buf, 5) == 5 && memcmp(buf, "hello", 5) == 0);
    CHECK(pipe_read(p, buf, 100) == 6 && memcmp(buf, " world", 6) == 0);
    CHECK(pipe_read(p, buf, 1) == PIPE_WOULDBLOCK);
    CHECK(pipe_write(p, "x", 0) == 0);

    /* full: a write takes what fits, then blocks; reading makes room; the queue wraps around */
    static char big[PIPE_SIZE + 100];
    for (int i = 0; i < (int)sizeof(big); i++)
        big[i] = (char)(i * 7 + 1);
    CHECK(pipe_write(p, big, 10) == 10 && pipe_read(p, buf, 10) == 10); /* moves head: later writes wrap */
    CHECK(pipe_write(p, big, sizeof(big)) == PIPE_SIZE);               /* only PIPE_SIZE fit */
    CHECK(pipe_write(p, big, 1) == PIPE_WOULDBLOCK);
    CHECK(pipe_read(p, buf, 100) == 100 && memcmp(buf, big, 100) == 0);
    CHECK(pipe_write(p, big + PIPE_SIZE, 100) == 100);                 /* wraps */
    int total = 0, ok = 1;
    static char all[PIPE_SIZE + 16];
    int got;
    while ((got = pipe_read(p, all + total, 50)) > 0)
        total += got;
    CHECK(got == PIPE_WOULDBLOCK && total == PIPE_SIZE);               /* 4096 - 100 + 100 */
    for (int i = 0; i < PIPE_SIZE - 100; i++)
        ok = ok && all[i] == big[100 + i];
    for (int i = 0; i < 100; i++)
        ok = ok && all[PIPE_SIZE - 100 + i] == big[PIPE_SIZE + i];
    CHECK(ok);

    /* wake-ups: a blocked reader is woken by a write, a blocked writer by a read; the registration is used up */
    WOKEN_N = 0;
    pipe_wait(p, 0, 3);
    pipe_wait(p, 1, 5);
    CHECK(pipe_write(p, "a", 1) == 1 && woke(3) && !woke(5));
    WOKEN_N = 0;
    CHECK(pipe_write(p, "b", 1) == 1 && WOKEN_N == 0);                  /* nobody registered any more */
    CHECK(pipe_read(p, buf, 1) == 1 && woke(5));
    CHECK(pipe_read(p, buf, 1) == 1);
    pipe_wait(p, 0, 99);                                                /* out of range: ignored */
    pipe_wait(p, 0, -1);

    /* the last writer leaves: readers get what is left, then end of input, and are woken */
    CHECK(pipe_write(p, "end", 3) == 3);
    pipe_ref(p, 1);                                                     /* two writers (after a spawn) */
    pipe_unref(p, 1);
    CHECK(pipe_read(p, buf, 2) == 2);                                   /* one is still here: no end yet */
    WOKEN_N = 0;
    pipe_wait(p, 0, 7);
    pipe_unref(p, 1);
    CHECK(woke(7));
    CHECK(pipe_read(p, buf, 10) == 1 && buf[0] == 'd');
    CHECK(pipe_read(p, buf, 10) == 0 && pipe_read(p, buf, 10) == 0);    /* end of input, every time */

    /* the last reader leaves: writers are woken and see a broken pipe */
    pipe_ref(p, 1);
    WOKEN_N = 0;
    pipe_wait(p, 1, 9);
    pipe_unref(p, 0);
    CHECK(woke(9) && pipe_write(p, "x", 1) == PIPE_BROKEN);
    CHECK(pipe_in_use() == 1);                                          /* one writer still holds it */
    pipe_unref(p, 1);
    CHECK(pipe_in_use() == 0 && !pipe_valid(p));                        /* both ends gone: freed */
    CHECK(pipe_read(p, buf, 1) == PIPE_BROKEN && pipe_write(p, "x", 1) == PIPE_BROKEN); /* a stale number is harmless */
    pipe_unref(p, 0);                                                    /* ... also to unref */
    pipe_ref(p, 0);
    CHECK(pipe_in_use() == 0);

    /* slots: PIPE_MAX pipes, then none; a freed one comes back clean */
    int ids[PIPE_MAX + 1], n = 0;
    while (n <= PIPE_MAX && (ids[n] = pipe_new()) >= 0)
        n++;
    CHECK(n == PIPE_MAX && pipe_in_use() == PIPE_MAX);
    pipe_ref(ids[3], 0);
    pipe_ref(ids[3], 1);
    CHECK(pipe_write(ids[3], "old", 3) == 3);
    pipe_unref(ids[3], 0);
    pipe_unref(ids[3], 1);
    int again = pipe_new();
    CHECK(again == ids[3]);
    pipe_ref(again, 0);
    pipe_ref(again, 1);
    CHECK(pipe_read(again, buf, 3) == PIPE_WOULDBLOCK);                  /* nothing left over from the old one */
    for (int i = 0; i < n; i++)
    {
        pipe_ref(ids[i], 0);
        pipe_unref(ids[i], 0);
    }
    CHECK(pipe_in_use() == 1);
    pipe_unref(again, 0);
    pipe_unref(again, 1);
    CHECK(pipe_in_use() == 0);

    /* an end nobody ever held: a pipe made and not used is freed by the first unref pair, and a write needs a reader */
    p = pipe_new();
    CHECK(pipe_write(p, "x", 1) == PIPE_BROKEN);
    pipe_ref(p, 0);
    CHECK(pipe_write(p, "x", 1) == 1);
    CHECK(pipe_read(p, buf, 1) == 1 && pipe_read(p, buf, 1) == 0);       /* the data, then end of input: no writer holds it */
    pipe_unref(p, 0);
    CHECK(pipe_in_use() == 0);

    if (fails == 0)
        printf("pipe tests passed\n");
    return fails ? 1 : 0;
}
