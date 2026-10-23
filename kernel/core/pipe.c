/* HarisOS pipes: see include/pipe.h. */
#include "pipe.h"
#include "heap.h"
#include "task.h"
#include "taskset.h"

typedef struct
{
    int readers, writers; /* holders of each end (several processes can hold one, after a spawn) */
    unsigned head, count; /* the queue is buf[head .. head + count), wrapping */
    TaskSet rwait, wwait; /* the threads that wait to read / to write */
    unsigned char buf[PIPE_SIZE];
} Pipe;

/* The table is a heap block of PIPE_CAP pointers, doubled when full (up to PIPE_MAX); each pipe is a heap block of its own, so a pointer to a
 * pipe stays good when the table moves. A pipe is named by its index; a free index holds NULL and is reused. */
static Pipe **PIPES;
static int PIPE_CAP;

static Pipe *get(int p) { return p >= 0 && p < PIPE_CAP ? PIPES[p] : 0; }

int pipe_valid(int p) { return get(p) != 0; }

int pipe_in_use(void)
{
    int n = 0;
    for (int i = 0; i < PIPE_CAP; i++)
        n += PIPES[i] ? 1 : 0;
    return n;
}

/* Double the table (at most PIPE_MAX entries); the new entries are free. 0 = at the limit or out of memory. */
static int grow_table(void)
{
    if (PIPE_CAP >= PIPE_MAX)
        return 0;
    int cap = PIPE_CAP ? PIPE_CAP * 2 : 16;
    if (cap > PIPE_MAX)
        cap = PIPE_MAX;
    Pipe **t = krealloc(PIPES, (unsigned long long)cap * sizeof(Pipe *));
    if (!t)
        return 0;
    for (int i = PIPE_CAP; i < cap; i++)
        t[i] = 0;
    PIPES = t;
    PIPE_CAP = cap;
    return 1;
}

int pipe_new(void)
{
    int i = 0;
    while (i < PIPE_CAP && PIPES[i])
        i++;
    if (i == PIPE_CAP && !grow_table())
        return -1;
    Pipe *p = kmalloc(sizeof(Pipe));
    if (!p)
        return -1;
    p->readers = p->writers = 0;
    p->head = p->count = 0;
    p->rwait = p->wwait = (TaskSet)TASKSET_INIT;
    PIPES[i] = p;
    return i;
}

static void wake_all(TaskSet *set) { taskset_wake_all(set); }

void pipe_ref(int p, int writer)
{
    Pipe *q = get(p);
    if (!q)
        return;
    if (writer)
        q->writers++;
    else
        q->readers++;
}

void pipe_unref(int p, int writer)
{
    Pipe *q = get(p);
    if (!q)
        return;
    if (writer && q->writers > 0)
        q->writers--;
    else if (!writer && q->readers > 0)
        q->readers--;
    /* the last writer gone: readers see end of input; the last reader gone: writers see a broken pipe */
    wake_all(&q->rwait);
    wake_all(&q->wwait);
    if (q->readers == 0 && q->writers == 0)
    {
        PIPES[p] = 0;
        taskset_free(&q->rwait);
        taskset_free(&q->wwait);
        kfree(q);
    }
}

int pipe_read(int p, void *buf, unsigned len)
{
    Pipe *q = get(p);
    if (!q)
        return PIPE_BROKEN;
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
    Pipe *q = get(p);
    if (!q)
        return PIPE_BROKEN;
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

int pipe_wait(int p, int writer_side, int task)
{
    Pipe *q = get(p);
    if (!q || task < 0)
        return 0;
    return taskset_add(writer_side ? &q->wwait : &q->rwait, task);
}
