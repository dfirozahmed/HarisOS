/* ============================================================
 * HarisOS semaphores: named counters that processes wait on
 *
 *   int s = sem_open("name", SEM_CREATE, initial);   the semaphore of that name (made with that count if SEM_CREATE and it is not
 *                                                     there); -1 if it cannot be. `initial` is ignored for one that exists.
 *   sem_ref(s) / sem_unref(s)                         one more / one less holder (a process holds it through an open descriptor)
 *   sem_unlink("name")                                the name goes away; it is freed when the last holder lets go
 *   sem_trywait(s)                                    0 and the count goes down by one, or SEM_WOULDBLOCK if it is 0
 *   sem_post(s)                                       the count goes up by one (0), a sleeper is woken; -1 if it would overflow
 *
 * wait (P) and post (V) are the two operations: a semaphore starting at 1 is a lock; one starting at 0 lets one process tell another
 * that something is ready; one starting at N hands out N units of something. Like pipes it never sleeps by itself: the system call
 * layer registers with sem_wait_for() and blocks when sem_trywait says SEM_WOULDBLOCK; a post wakes everyone registered (they try again).
 * ============================================================ */
#ifndef HARIS_SEM_H
#define HARIS_SEM_H

#define SEM_MAX 4096 /* semaphores that can exist at once: the table is grown as needed up to this */
#define SEM_NAME_MAX 32
#define SEM_VALUE_MAX 0x7FFFFFFF

#define SEM_CREATE 1
#define SEM_EXCL 2

#define SEM_WOULDBLOCK -2

int sem_open(const char *name, unsigned flags, unsigned initial);
void sem_ref(int s);
void sem_unref(int s);
int sem_unlink(const char *name);
int sem_trywait(int s);
int sem_post(int s);
void sem_wait_for(int s, int task); /* wake `task` at the next post */
int sem_value(int s);               /* the count, -1 for a semaphore that does not exist */
int sem_valid(int s);
int sem_in_use(void);
int sem_valid_name(const char *name);

#endif
