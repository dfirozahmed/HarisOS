/* ============================================================
 * HarisOS message queues: named queues of whole messages between processes
 *
 *   int q = mq_open("name", MQ_CREATE);   the queue of that name (made, empty, if MQ_CREATE and it is not there); -1 if it cannot be
 *   mq_ref(q) / mq_unref(q)                one more / one less holder (a process holds it through an open descriptor)
 *   mq_unlink("name")                      the name goes away; the queue is freed when the last holder lets go
 *   n = mq_send(q, msg, len)               queue one message of 1 .. MQ_MSG_MAX bytes
 *   n = mq_recv(q, buf, cap)               take the oldest message: its length
 *
 * A queue keeps message boundaries (one send = one receive), the order of sending, and at most MQ_DEPTH waiting messages. Like pipes
 * it never blocks by itself: mq_send returns MQ_WOULDBLOCK when full, mq_recv when empty; the caller registers with mq_wait() and
 * sleeps; the next recv / send wakes it (spuriously at worst). mq_recv returns -1, and keeps the message, if cap is smaller than it.
 * ============================================================ */
#ifndef HARIS_MQ_H
#define HARIS_MQ_H

#define MQ_MAX 1024 /* queues that can exist at once: the table is grown as needed up to this */
#define MQ_NAME_MAX 32
#define MQ_MSG_MAX 256
#define MQ_DEPTH 16

#define MQ_CREATE 1
#define MQ_EXCL 2

#define MQ_WOULDBLOCK -2

int mq_open(const char *name, unsigned flags);
void mq_ref(int q);
void mq_unref(int q);
int mq_unlink(const char *name);
int mq_send(int q, const void *msg, unsigned len);
int mq_recv(int q, void *buf, unsigned cap);
void mq_wait(int q, int send_side, int task);
int mq_valid(int q);
int mq_in_use(void);
int mq_valid_name(const char *name);

#endif
