/* ============================================================
 * HarisOS reader-writer lock for kernel threads (see task.h, mutex.h)
 *
 *   static RWLock rw = RWLOCK_INIT;
 *   rw_rdlock(&rw)   any number of readers at once; sleeps while a writer holds it or waits for it. 0
 *   rw_wrlock(&rw)   one writer, no readers; sleeps until all readers and any other writer are gone. 0, or -1 if the caller holds it
 *   rw_tryrdlock / rw_trywrlock                    0 if taken, -1 if it would have to sleep (never sleeps)
 *   rw_rdunlock(&rw) a reader lets go (-1 if there is no reader);   rw_wrunlock(&rw)  the writer lets go (-1 if the caller is not it)
 *
 * Writers are preferred: a reader that arrives while a writer waits sleeps behind it, so a steady stream of readers cannot starve
 * writers. When the last reader leaves, the best waiting writer is woken (highest priority); when a writer leaves, the waiting
 * writers go first, and if there are none every waiting reader is woken. A woken thread checks again, so another thread may get in
 * ahead of it. Everything is decided with interrupts off, as in mutex.c, so a wake-up cannot slip between a check and the sleep.
 *
 * Limits: only the writer's id is remembered, not the readers' (rw_rdunlock cannot tell who calls it), so a thread must unlock
 * only what it locked. Readers do not nest if a writer may wait in between (the second rw_rdlock would sleep behind the writer,
 * which waits for the first: a deadlock), a reader cannot become a writer, and no priority is lent to a holder (see mutex.h for
 * that). A writer that ended or was killed loses the lock. The main loop (thread 0) never blocks, so it spins politely.
 * ============================================================ */
#ifndef HARIS_RWLOCK_H
#define HARIS_RWLOCK_H

typedef struct
{
    int readers;       /* threads holding it for reading */
    int writer;        /* thread id of the writer, -1 if none */
    unsigned rwaiters; /* bit i: thread i sleeps in rw_rdlock */
    unsigned wwaiters; /* bit i: thread i sleeps in rw_wrlock */
} RWLock;

#define RWLOCK_INIT { 0, -1, 0, 0 }

void rw_init(RWLock *l);
int rw_rdlock(RWLock *l);
int rw_tryrdlock(RWLock *l);
int rw_rdunlock(RWLock *l);
int rw_wrlock(RWLock *l);
int rw_trywrlock(RWLock *l);
int rw_wrunlock(RWLock *l);

#endif
