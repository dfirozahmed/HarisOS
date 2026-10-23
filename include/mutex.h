/* ============================================================
 * HarisOS kernel mutex and condition variable (for kernel threads, see task.h)
 *
 *   static Mutex m = MUTEX_INIT;          the caller owns the storage; no allocation
 *   mutex_lock(&m);                       sleeps (task_block) while another thread holds it; 0, or -1 if the caller holds it already
 *   mutex_trylock(&m);                    0 if taken, -1 if it is busy (never sleeps)
 *   mutex_unlock(&m);                     0, or -1 if the caller is not the holder; wakes the best waiter
 *
 *   static CondVar cv = COND_INIT;
 *   cond_wait(&cv, &m);                   m must be held: releases it, sleeps until signalled, takes it again. -1 if m is not held by
 *                                         the caller. Check the condition in a loop: another thread may have changed it again.
 *   cond_signal(&cv) / cond_broadcast(&cv) wake the best waiter / all waiters. Nothing happens if nobody waits (a signal is not kept).
 *
 * How it works: a lock holds its owner thread id and a bit per sleeping thread (TASK_MAX is 16). Everything is decided with
 * interrupts off, so the check "is it free?" and task_block() cannot be separated by a wake-up (see task_block in task.h). When
 * several threads wait, the one with the highest priority (task_priority) is woken first; a thread that ended while waiting is
 * skipped. A woken thread checks again, so another thread may take the lock first ("barging"); it then sleeps again.
 *
 * Limits: no priority inheritance (a low-priority holder can keep a high-priority waiter waiting), no recursive locking, and a
 * lock whose holder is killed stays locked. The main loop (thread 0) never blocks: task_block() only yields there, so it spins
 * politely instead of sleeping. Do not lock from an interrupt handler.
 * ============================================================ */
#ifndef HARIS_MUTEX_H
#define HARIS_MUTEX_H

typedef struct
{
    int owner;         /* thread id of the holder, -1 if free */
    unsigned waiters;  /* bit i: thread i sleeps in mutex_lock */
} Mutex;

typedef struct
{
    unsigned waiters;  /* bit i: thread i sleeps in cond_wait */
} CondVar;

#define MUTEX_INIT { -1, 0 }
#define COND_INIT { 0 }

void mutex_init(Mutex *m);
int mutex_lock(Mutex *m);
int mutex_trylock(Mutex *m);
int mutex_unlock(Mutex *m);
int mutex_held(const Mutex *m); /* 1 if the running thread holds it */

void cond_init(CondVar *c);
int cond_wait(CondVar *c, Mutex *m);
void cond_signal(CondVar *c);
void cond_broadcast(CondVar *c);

#endif
