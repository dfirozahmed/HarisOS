/* Host test for taskset.c: one bit per thread id, inline for the first 64 and in a heap block above that. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "taskset.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int WOKEN[256], WOKEN_N;
void task_wake(int id)
{
    if (WOKEN_N < 256)
        WOKEN[WOKEN_N++] = id;
}

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

int main(void)
{
    TaskSet s = TASKSET_INIT;
    CHECK(taskset_empty(&s) && taskset_next(&s, 0) == -1 && !taskset_has(&s, 0) && HEAP_BLOCKS == 0);

    /* the first 64 ids need no memory */
    CHECK(taskset_add(&s, 0) == 0 && taskset_add(&s, 5) == 0 && taskset_add(&s, 63) == 0 && HEAP_BLOCKS == 0);
    CHECK(taskset_has(&s, 0) && taskset_has(&s, 5) && taskset_has(&s, 63) && !taskset_has(&s, 6) && !taskset_empty(&s));
    CHECK(taskset_next(&s, 0) == 0 && taskset_next(&s, 1) == 5 && taskset_next(&s, 6) == 63 && taskset_next(&s, 64) == -1);
    taskset_remove(&s, 5);
    CHECK(!taskset_has(&s, 5) && taskset_next(&s, 1) == 63);
    CHECK(taskset_add(&s, -1) == 0 && !taskset_has(&s, -1) && taskset_next(&s, -5) == 0); /* a bad id is ignored */

    /* higher ids grow the set; it iterates in order across the inline part and the block */
    CHECK(taskset_add(&s, 64) == 0 && taskset_add(&s, 130) == 0 && taskset_add(&s, 1000) == 0 && HEAP_BLOCKS == 1);
    CHECK(taskset_has(&s, 64) && taskset_has(&s, 130) && taskset_has(&s, 1000) && !taskset_has(&s, 129) && !taskset_has(&s, 5000));
    int order[8], n = 0;
    for (int i = taskset_next(&s, 0); i >= 0; i = taskset_next(&s, i + 1))
        order[n++] = i;
    CHECK(n == 5 && order[0] == 0 && order[1] == 63 && order[2] == 64 && order[3] == 130 && order[4] == 1000);
    taskset_remove(&s, 130);
    taskset_remove(&s, 9999); /* beyond the block: nothing */
    CHECK(!taskset_has(&s, 130) && taskset_next(&s, 65) == 1000 && taskset_next(&s, 1001) == -1);

    /* wake_all wakes every member once, lowest first, and empties the set (memory stays) */
    WOKEN_N = 0;
    taskset_wake_all(&s);
    CHECK(WOKEN_N == 4 && WOKEN[0] == 0 && WOKEN[1] == 63 && WOKEN[2] == 64 && WOKEN[3] == 1000);
    CHECK(taskset_empty(&s) && HEAP_BLOCKS == 1);

    /* no memory: a high id is refused and the set is as it was; low ids still work */
    TaskSet t = TASKSET_INIT;
    HEAP_FAIL = 1;
    CHECK(taskset_add(&t, 100) == -1 && taskset_empty(&t) && !taskset_has(&t, 100));
    CHECK(taskset_add(&t, 3) == 0 && taskset_has(&t, 3));
    HEAP_FAIL = 0;
    CHECK(taskset_add(&t, 100) == 0 && taskset_has(&t, 100) && HEAP_BLOCKS == 2);
    HEAP_FAIL = 1;
    CHECK(taskset_add(&t, 100000) == -1 && taskset_has(&t, 100) && taskset_has(&t, 3)); /* growing failed: nothing lost */
    HEAP_FAIL = 0;

    /* free gives the block back and leaves an empty, usable set */
    taskset_free(&s);
    taskset_free(&t);
    CHECK(HEAP_BLOCKS == 0 && taskset_empty(&s) && taskset_empty(&t));
    CHECK(taskset_add(&s, 2) == 0 && taskset_has(&s, 2));
    taskset_free(&s);

    if (fails)
    {
        printf("%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("taskset tests passed\n");
    return 0;
}
