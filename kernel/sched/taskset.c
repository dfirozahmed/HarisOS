/* HarisOS thread sets: see include/taskset.h. */
#include "taskset.h"
#include "heap.h"
#include "task.h"

typedef unsigned long long u64;

/* Make room for bit `id` (>= TASKSET_INLINE). 0, or -1 if the heap is out of memory (the set is unchanged). */
static int reserve(TaskSet *s, int id)
{
    int need = (id - TASKSET_INLINE) / 64 + 1;
    if (need <= s->more_words)
        return 0;
    int words = s->more_words ? s->more_words : 1;
    while (words < need)
        words *= 2;
    u64 *n = krealloc(s->more, (u64)words * sizeof(u64));
    if (!n)
        return -1;
    for (int i = s->more_words; i < words; i++)
        n[i] = 0;
    s->more = n;
    s->more_words = words;
    return 0;
}

int taskset_add(TaskSet *s, int id)
{
    if (id < 0)
        return 0;
    if (id < TASKSET_INLINE)
    {
        s->lo |= 1ULL << id;
        return 0;
    }
    if (reserve(s, id) != 0)
        return -1;
    int b = id - TASKSET_INLINE;
    s->more[b / 64] |= 1ULL << (b % 64);
    return 0;
}

void taskset_remove(TaskSet *s, int id)
{
    if (id < 0)
        return;
    if (id < TASKSET_INLINE)
    {
        s->lo &= ~(1ULL << id);
        return;
    }
    int b = id - TASKSET_INLINE;
    if (b / 64 < s->more_words)
        s->more[b / 64] &= ~(1ULL << (b % 64));
}

int taskset_has(const TaskSet *s, int id)
{
    if (id < 0)
        return 0;
    if (id < TASKSET_INLINE)
        return (s->lo >> id) & 1;
    int b = id - TASKSET_INLINE;
    return b / 64 < s->more_words && ((s->more[b / 64] >> (b % 64)) & 1);
}

int taskset_empty(const TaskSet *s)
{
    if (s->lo)
        return 0;
    for (int i = 0; i < s->more_words; i++)
        if (s->more[i])
            return 0;
    return 1;
}

int taskset_next(const TaskSet *s, int from)
{
    if (from < 0)
        from = 0;
    for (; from < TASKSET_INLINE; from++)
        if ((s->lo >> from) & 1)
            return from;
    for (int b = from - TASKSET_INLINE; b / 64 < s->more_words; b++)
    {
        u64 w = s->more[b / 64] >> (b % 64);
        if (w == 0)
        {
            b = (b / 64 + 1) * 64 - 1; /* the rest of this word is empty: on to the next one */
            continue;
        }
        while (!(w & 1))
        {
            w >>= 1;
            b++;
        }
        return b + TASKSET_INLINE;
    }
    return -1;
}

void taskset_clear(TaskSet *s)
{
    s->lo = 0;
    for (int i = 0; i < s->more_words; i++)
        s->more[i] = 0;
}

void taskset_wake_all(TaskSet *s)
{
    /* (task_wake does not change the set, so it can be walked while waking; it is emptied after) */
    for (int i = taskset_next(s, 0); i >= 0; i = taskset_next(s, i + 1))
        task_wake(i);
    taskset_clear(s);
}

void taskset_free(TaskSet *s)
{
    kfree(s->more);
    s->more = 0;
    s->more_words = 0;
    s->lo = 0;
}
