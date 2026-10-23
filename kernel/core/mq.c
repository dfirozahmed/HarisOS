/* HarisOS message queues: see include/mq.h. */
#include "mq.h"
#include "heap.h"
#include "task.h"

typedef struct
{
    int linked, refs;
    char name[MQ_NAME_MAX];
    unsigned head, count; /* the queue is slot[head .. head + count) */
    unsigned len[MQ_DEPTH];
    unsigned char msg[MQ_DEPTH][MQ_MSG_MAX];
    unsigned rwait, wwait; /* bit i = thread i waits to receive / to send */
} Queue;

/* The table is a heap block of Q_CAP pointers, doubled when full (up to MQ_MAX); each queue is a heap block of its own, so a pointer to a
 * queue stays good when the table moves. A queue is named by its index; a free index holds NULL and is reused. */
static Queue **QUEUES;
static int Q_CAP;

static Queue *get(int q) { return q >= 0 && q < Q_CAP ? QUEUES[q] : 0; }

int mq_valid(int q) { return get(q) != 0; }

int mq_valid_name(const char *name)
{
    int n = 0;
    for (; name[n]; n++)
        if (n >= MQ_NAME_MAX - 1 || name[n] == '/' || (unsigned char)name[n] < 32)
            return 0;
    return n > 0;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

static int find(const char *name)
{
    for (int i = 0; i < Q_CAP; i++)
        if (QUEUES[i] && QUEUES[i]->linked && same(QUEUES[i]->name, name))
            return i;
    return -1;
}

/* Double the table (at most MQ_MAX entries); the new entries are free. 0 = at the limit or out of memory. */
static int grow_table(void)
{
    if (Q_CAP >= MQ_MAX)
        return 0;
    int cap = Q_CAP ? Q_CAP * 2 : 8;
    if (cap > MQ_MAX)
        cap = MQ_MAX;
    Queue **t = krealloc(QUEUES, (unsigned long long)cap * sizeof(Queue *));
    if (!t)
        return 0;
    for (int i = Q_CAP; i < cap; i++)
        t[i] = 0;
    QUEUES = t;
    Q_CAP = cap;
    return 1;
}

static void release(int q)
{
    kfree(QUEUES[q]);
    QUEUES[q] = 0;
}

int mq_open(const char *name, unsigned flags)
{
    if (!mq_valid_name(name))
        return -1;
    int q = find(name);
    if (q >= 0)
        return (flags & MQ_CREATE) && (flags & MQ_EXCL) ? -1 : q;
    if (!(flags & MQ_CREATE))
        return -1;
    for (q = 0; q < Q_CAP && QUEUES[q]; q++)
        ;
    if (q == Q_CAP && !grow_table())
        return -1;
    Queue *s = kmalloc(sizeof(Queue));
    if (!s)
        return -1;
    s->linked = 1;
    s->refs = 0;
    s->head = s->count = 0;
    s->rwait = s->wwait = 0;
    int n = 0;
    for (; name[n]; n++)
        s->name[n] = name[n];
    s->name[n] = 0;
    QUEUES[q] = s;
    return q;
}

static void wake_all(unsigned *mask)
{
    unsigned m = *mask;
    *mask = 0;
    for (int t = 0; t < TASK_MAX; t++)
        if (m & (1u << t))
            task_wake(t);
}

void mq_ref(int q)
{
    Queue *s = get(q);
    if (s)
        s->refs++;
}

void mq_unref(int q)
{
    Queue *s = get(q);
    if (!s)
        return;
    if (s->refs > 0)
        s->refs--;
    if (s->refs == 0 && !s->linked)
        release(q);
}

int mq_unlink(const char *name)
{
    int q = find(name);
    if (q < 0)
        return -1;
    QUEUES[q]->linked = 0;
    if (QUEUES[q]->refs == 0)
        release(q);
    return 0;
}

int mq_send(int q, const void *msg, unsigned len)
{
    Queue *s = get(q);
    if (!s || len == 0 || len > MQ_MSG_MAX)
        return -1;
    if (s->count == MQ_DEPTH)
        return MQ_WOULDBLOCK;
    unsigned slot = (s->head + s->count) % MQ_DEPTH;
    const unsigned char *in = msg;
    for (unsigned i = 0; i < len; i++)
        s->msg[slot][i] = in[i];
    s->len[slot] = len;
    s->count++;
    wake_all(&s->rwait);
    return (int)len;
}

int mq_recv(int q, void *buf, unsigned cap)
{
    Queue *s = get(q);
    if (!s)
        return -1;
    if (s->count == 0)
        return MQ_WOULDBLOCK;
    unsigned len = s->len[s->head];
    if (cap < len)
        return -1; /* the message stays */
    unsigned char *out = buf;
    for (unsigned i = 0; i < len; i++)
        out[i] = s->msg[s->head][i];
    s->head = (s->head + 1) % MQ_DEPTH;
    s->count--;
    wake_all(&s->wwait);
    return (int)len;
}

void mq_wait(int q, int send_side, int task)
{
    Queue *s = get(q);
    if (!s || task < 0 || task >= TASK_MAX)
        return;
    if (send_side)
        s->wwait |= 1u << task;
    else
        s->rwait |= 1u << task;
}

int mq_in_use(void)
{
    int n = 0;
    for (int i = 0; i < Q_CAP; i++)
        n += QUEUES[i] ? 1 : 0;
    return n;
}
