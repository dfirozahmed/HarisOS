/* ============================================================
 * HarisOS named mutexes and condition variables for processes (the system call side of mutex.h)
 *
 *   int s = usync_open("name", USYNC_MUTEX, SYNC_CREATE);   the object of that kind and name (made if USYNC_CREATE and it is not there); -1 if not
 *   usync_ref(s) / usync_unref(s)                           one more / one less holder (a process holds it through an open descriptor)
 *   usync_unlink("name", kind)                              the name goes away; it is freed when the last holder lets go
 *   usync_mutex(s) / usync_cond(s)                          the Mutex / CondVar inside (NULL if s is not one of that kind)
 *
 * A mutex and a condition variable may share a name: names are looked up per kind. Like semaphores, the table never sleeps by itself:
 * the system call layer loops on mutex_trylock / the waiter bits and calls task_block, so a signal can interrupt the wait.
 * ============================================================ */
#ifndef HARIS_USYNC_H
#define HARIS_USYNC_H

#include "mutex.h"

#define USYNC_MAX 16
#define USYNC_NAME_MAX 32

#define USYNC_MUTEX 0
#define USYNC_COND 1

#define USYNC_CREATE 1
#define USYNC_EXCL 2

int usync_open(const char *name, int kind, unsigned flags);
void usync_ref(int s);
void usync_unref(int s);
int usync_unlink(const char *name, int kind);
Mutex *usync_mutex(int s);
CondVar *usync_cond(int s);
int usync_valid_name(const char *name);
int usync_in_use(void);

#endif
