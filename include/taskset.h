/* ============================================================
 * HarisOS thread sets: "which threads sleep on this object" for pipes, queues, semaphores, mutexes and the like
 *
 *   TaskSet s = TASKSET_INIT;           (or all zero bytes: that is an empty set)
 *   taskset_add(&s, id);                the thread sleeps here; 0, or -1 if the set could not grow (no memory)
 *   taskset_remove(&s, id);  taskset_has(&s, id);  taskset_empty(&s);
 *   for (int i = taskset_next(&s, 0); i >= 0; i = taskset_next(&s, i + 1)) ...   every member, lowest id first
 *   taskset_wake_all(&s);               task_wake every member and empty the set
 *   taskset_free(&s);                   empty the set and give its memory back (call it when the object that holds the set goes away)
 *
 * A set is one bit per thread id. The first TASKSET_INLINE ids live inside the set itself and never need memory; a higher id makes
 * the set take a block from the kernel heap (it grows as needed, never shrinks until taskset_free). Only taskset_add can fail, and only
 * for such an id when the heap is out of memory: the caller then must not task_block() (nothing would wake it), it yields and looks
 * again. Everything is meant to run with interrupts off, like the objects that hold the sets.
 * ============================================================ */
#ifndef HARIS_TASKSET_H
#define HARIS_TASKSET_H

#define TASKSET_INLINE 64

typedef struct
{
    unsigned long long lo;    /* bit i = thread i, for i < TASKSET_INLINE */
    unsigned long long *more; /* bit i - TASKSET_INLINE of a heap block of more_words words, NULL if none */
    int more_words;
} TaskSet;

#define TASKSET_INIT { 0, 0, 0 }

int taskset_add(TaskSet *s, int id);
void taskset_remove(TaskSet *s, int id);
int taskset_has(const TaskSet *s, int id);
int taskset_empty(const TaskSet *s);
int taskset_next(const TaskSet *s, int from); /* the lowest member >= from, or -1 */
void taskset_wake_all(TaskSet *s);
void taskset_clear(TaskSet *s); /* empty it, keep its memory */
void taskset_free(TaskSet *s);

#endif
