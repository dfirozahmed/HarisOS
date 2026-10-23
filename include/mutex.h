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
 * Priority inheritance: a thread that waits for a mutex lends its priority to the holder (task_set_inherited), so a low-priority
 * holder cannot be starved by medium-priority threads while a high-priority one waits for it (priority inversion). The loan is
 * transitive (A holds m1, B holds m2 and waits for m1, C waits for m2: A runs at C's priority), covers every mutex a thread holds
 * (it gives back only what no remaining waiter asks for) and ends when the holder unlocks. The holder is raised from the moment
 * the waiter registers; it is not preempted at once, it runs better from the next scheduling decision (see task_set_priority).
 * Condition variable waiters do not lend priority: nobody holds anything they wait for.
 *
 * Deadlock detection: before a thread sleeps in mutex_lock, the chain "holder waits for mutex, whose holder waits for ..." is followed;
 * if it leads back to the thread, mutex_lock returns -1 at once instead of sleeping (the last thread to close the cycle is the one
 * that is told; the others keep waiting and get the lock when it lets go). The system call SYS_MUTEX_LOCK does the same. Cycles through
 * other waits (a condition variable, a semaphore, a pipe) are not seen: only mutexes know who holds them.
 *
 * Limits: no recursive locking. A holder that ended or was
 * killed loses the lock (the next lock / trylock takes it back); only the thread id is remembered, so a new thread that reuses that
 * slot would look like the old holder. The main loop (thread 0) never blocks: task_block() only yields there, so it spins
 * politely instead of sleeping. Do not lock from an interrupt handler.
 * ============================================================ */
#ifndef HARIS_MUTEX_H
#define HARIS_MUTEX_H

typedef struct Mutex_
{
    int owner;         /* thread id of the holder, -1 if free */
    unsigned waiters;  /* bit i: thread i sleeps in mutex_lock */
    struct Mutex_ *next_held; /* the next mutex the holder holds (priority inheritance, mutex.c) */
} Mutex;

typedef struct
{
    unsigned waiters;  /* bit i: thread i sleeps in cond_wait */
} CondVar;

#define MUTEX_INIT { -1, 0, 0 }
#define COND_INIT { 0 }

void mutex_init(Mutex *m);
int mutex_lock(Mutex *m);    /* -1 instead of waiting if the wait would close a cycle of threads that wait for each other (a deadlock) */
int mutex_relock(Mutex *m);  /* mutex_lock without that check: for cond_wait, which must come back holding the mutex */
int mutex_would_deadlock(Mutex *m, int task); /* 1 if `task` waiting for m would close such a cycle (for the system call layer) */
int mutex_trylock(Mutex *m);
int mutex_unlock(Mutex *m);
int mutex_held(const Mutex *m); /* 1 if the running thread holds it */

/* For the system call layer (usync.h), which sleeps in its own loop so that a signal can end the wait: register / forget `task` as
 * a sleeper. The caller loops on mutex_trylock (or cond_waiting) and calls task_block() with interrupts off. */
void mutex_wait_for(Mutex *m, int task);
void mutex_cancel_wait(Mutex *m, int task);
void cond_wait_for(CondVar *c, int task);
void cond_cancel_wait(CondVar *c, int task);
int cond_waiting(const CondVar *c, int task);

void cond_init(CondVar *c);
int cond_wait(CondVar *c, Mutex *m);
void cond_signal(CondVar *c);
void cond_broadcast(CondVar *c);

#endif
