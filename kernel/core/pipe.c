/* HarisOS pipes: see include/pipe.h. */
#include "pipe.h"
#include "task.h"

typedef struct
{
    int used;
    int readers, writers; /* holders of each end (several processes can hold one, after a spawn) */
    unsigned head, count; /* the queue is buf[head .. head + count), wrapping */
    unsigned rwait, wwait; /* bit i = thread i waits to read / to write */
    unsigned char buf[PIPE_SIZE];
} Pipe;

static Pipe PIPES[PIPE_MAX];

int pipe_valid(int p) { return p >= 0 && p < PIPE_MAX && PIPES[p].used; }

int pipe_in_use(void)
{
    int n = 0;
    for (int i = 0; i < PIPE_MAX; i++)
        n += PIPES[i].used ? 1 : 0;
    return n;
}

int pipe_new(void)
{
    for (int i = 0; i < PIPE_MAX; i++)
        if (!PIPES[i].used)
        {
            Pipe *p = &PIPES[i];
            p->used = 1;
            p->readers = p->writers = 0;
            p->head = p->count = 0;
            p->rwait = p->wwait = 0;
            return i;
        }
    return -1;
}

static void wake_all(unsigned *mask)
{
    unsigned m = *mask;
    *mask = 0;
    for (int t = 0; t < TASK_MAX; t++)
        if (m & (1u << t))
            task_wake(t);
}

void pipe_ref(int p, int writer)
{
    if (!pipe_valid(p))
        return;
    if (writer)
        PIPES[p].writers++;
    else
        PIPES[p].readers++;
}

void pipe_unref(int p, int writer)
{
    if (!pipe_valid(p))
        return;
    Pipe *q = &PIPES[p];
    if (writer && q->writers > 0)
        q->writers--;
    else if (!writer && q->readers > 0)
        q->readers--;
    /* the last writer gone: readers see end of input; the last reader gone: writers see a broken pipe */
    wake_all(&q->rwait);
    wake_all(&q->wwait);
    if (q->readers == 0 && q->writers == 0)
        q->used = 0;
}

int pipe_read(int p, void *buf, unsigned len)
{
    if (!pipe_valid(p))
        return PIPE_BROKEN;
    Pipe *q = &PIPES[p];
    if (q->count == 0)
        return q->writers > 0 ? PIPE_WOULDBLOCK : 0;
    unsigned n = len < q->count ? len : q->count;
    unsigned char *out = buf;
    for (unsigned i = 0; i < n; i++)
        out[i] = q->buf[(q->head + i) % PIPE_SIZE];
    q->head = (q->head + n) % PIPE_SIZE;
    q->count -= n;
    if (n)
        wake_all(&q->wwait); /* there is room now */
    return (int)n;
}

int pipe_write(int p, const void *buf, unsigned len)
{
    if (!pipe_valid(p))
        return PIPE_BROKEN;
    Pipe *q = &PIPES[p];
    if (q->readers == 0)
        return PIPE_BROKEN;
    if (len == 0)
        return 0;
    unsigned room = PIPE_SIZE - q->count;
    if (room == 0)
        return PIPE_WOULDBLOCK;
    unsigned n = len < room ? len : room;
    const unsigned char *in = buf;
    for (unsigned i = 0; i < n; i++)
        q->buf[(q->head + q->count + i) % PIPE_SIZE] = in[i];
    q->count += n;
    wake_all(&q->rwait); /* there is something to read */
    return (int)n;
}

void pipe_wait(int p, int writer_side, int task)
{
    if (!pipe_valid(p) || task < 0 || task >= TASK_MAX)
        return;
    if (writer_side)
        PIPES[p].wwait |= 1u << task;
    else
        PIPES[p].rwait |= 1u << task;
}
