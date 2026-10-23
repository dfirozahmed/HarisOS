/* HarisOS message queues: see include/mq.h. */
#include "mq.h"
#include "task.h"

typedef struct
{
    int used, linked, refs;
    char name[MQ_NAME_MAX];
    unsigned head, count; /* the queue is slot[head .. head + count) */
    unsigned len[MQ_DEPTH];
    unsigned char msg[MQ_DEPTH][MQ_MSG_MAX];
    unsigned rwait, wwait; /* bit i = thread i waits to receive / to send */
} Queue;

static Queue QUEUES[MQ_MAX];

int mq_valid(int q) { return q >= 0 && q < MQ_MAX && QUEUES[q].used; }

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
    for (int i = 0; i < MQ_MAX; i++)
        if (QUEUES[i].used && QUEUES[i].linked && same(QUEUES[i].name, name))
            return i;
    return -1;
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
    for (q = 0; q < MQ_MAX && QUEUES[q].used; q++)
        ;
    if (q == MQ_MAX)
        return -1;
    Queue *s = &QUEUES[q];
    s->used = s->linked = 1;
    s->refs = 0;
    s->head = s->count = 0;
    s->rwait = s->wwait = 0;
    int n = 0;
    for (; name[n]; n++)
        s->name[n] = name[n];
    s->name[n] = 0;
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
    if (mq_valid(q))
        QUEUES[q].refs++;
}

void mq_unref(int q)
{
    if (!mq_valid(q))
        return;
    if (QUEUES[q].refs > 0)
        QUEUES[q].refs--;
    if (QUEUES[q].refs == 0 && !QUEUES[q].linked)
        QUEUES[q].used = 0;
}

int mq_unlink(const char *name)
{
    int q = find(name);
    if (q < 0)
        return -1;
    QUEUES[q].linked = 0;
    if (QUEUES[q].refs == 0)
        QUEUES[q].used = 0;
    return 0;
}

int mq_send(int q, const void *msg, unsigned len)
{
    if (!mq_valid(q) || len == 0 || len > MQ_MSG_MAX)
        return -1;
    Queue *s = &QUEUES[q];
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
    if (!mq_valid(q))
        return -1;
    Queue *s = &QUEUES[q];
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
    if (!mq_valid(q) || task < 0 || task >= TASK_MAX)
        return;
    if (send_side)
        QUEUES[q].wwait |= 1u << task;
    else
        QUEUES[q].rwait |= 1u << task;
}

int mq_in_use(void)
{
    int n = 0;
    for (int i = 0; i < MQ_MAX; i++)
        n += QUEUES[i].used ? 1 : 0;
    return n;
}
