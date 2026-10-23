/* HarisOS semaphores: see include/sem.h. */
#include "sem.h"
#include "heap.h"
#include "task.h"
#include "taskset.h"

typedef struct
{
    int linked, refs;
    unsigned value;
    TaskSet waiters; /* the threads that sleep in a wait */
    char name[SEM_NAME_MAX];
} Semaphore;

/* The table is a heap block of SEM_CAP pointers, doubled when full (up to SEM_MAX); each semaphore is a heap block of its own, so a pointer
 * to one stays good when the table moves. A semaphore is named by its index; a free index holds NULL and is reused. */
static Semaphore **SEMS;
static int SEM_CAP;

static Semaphore *get(int s) { return s >= 0 && s < SEM_CAP ? SEMS[s] : 0; }

int sem_valid(int s) { return get(s) != 0; }

int sem_valid_name(const char *name)
{
    int n = 0;
    for (; name[n]; n++)
        if (n >= SEM_NAME_MAX - 1 || name[n] == '/' || (unsigned char)name[n] < 32)
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
    for (int i = 0; i < SEM_CAP; i++)
        if (SEMS[i] && SEMS[i]->linked && same(SEMS[i]->name, name))
            return i;
    return -1;
}

/* Double the table (at most SEM_MAX entries); the new entries are free. 0 = at the limit or out of memory. */
static int grow_table(void)
{
    if (SEM_CAP >= SEM_MAX)
        return 0;
    int cap = SEM_CAP ? SEM_CAP * 2 : 16;
    if (cap > SEM_MAX)
        cap = SEM_MAX;
    Semaphore **t = krealloc(SEMS, (unsigned long long)cap * sizeof(Semaphore *));
    if (!t)
        return 0;
    for (int i = SEM_CAP; i < cap; i++)
        t[i] = 0;
    SEMS = t;
    SEM_CAP = cap;
    return 1;
}

static void release(int s)
{
    taskset_free(&SEMS[s]->waiters);
    kfree(SEMS[s]);
    SEMS[s] = 0;
}

int sem_open(const char *name, unsigned flags, unsigned initial)
{
    if (!sem_valid_name(name))
        return -1;
    int s = find(name);
    if (s >= 0)
        return (flags & SEM_CREATE) && (flags & SEM_EXCL) ? -1 : s;
    if (!(flags & SEM_CREATE) || initial > SEM_VALUE_MAX)
        return -1;
    for (s = 0; s < SEM_CAP && SEMS[s]; s++)
        ;
    if (s == SEM_CAP && !grow_table())
        return -1;
    Semaphore *m = kmalloc(sizeof(Semaphore));
    if (!m)
        return -1;
    m->linked = 1;
    m->refs = 0;
    m->value = initial;
    m->waiters = (TaskSet)TASKSET_INIT;
    int n = 0;
    for (; name[n]; n++)
        m->name[n] = name[n];
    m->name[n] = 0;
    SEMS[s] = m;
    return s;
}

void sem_ref(int s)
{
    Semaphore *m = get(s);
    if (m)
        m->refs++;
}

void sem_unref(int s)
{
    Semaphore *m = get(s);
    if (!m)
        return;
    if (m->refs > 0)
        m->refs--;
    if (m->refs == 0 && !m->linked)
        release(s);
}

int sem_unlink(const char *name)
{
    int s = find(name);
    if (s < 0)
        return -1;
    SEMS[s]->linked = 0;
    if (SEMS[s]->refs == 0)
        release(s);
    return 0;
}

int sem_trywait(int s)
{
    Semaphore *m = get(s);
    if (!m)
        return -1;
    if (m->value == 0)
        return SEM_WOULDBLOCK;
    m->value--;
    return 0;
}

int sem_post(int s)
{
    Semaphore *m = get(s);
    if (!m || m->value >= SEM_VALUE_MAX)
        return -1;
    m->value++;
    taskset_wake_all(&m->waiters);
    return 0;
}

int sem_wait_for(int s, int task)
{
    Semaphore *m = get(s);
    if (!m || task < 0)
        return 0;
    return taskset_add(&m->waiters, task);
}

int sem_value(int s)
{
    Semaphore *m = get(s);
    return m ? (int)m->value : -1;
}

int sem_in_use(void)
{
    int n = 0;
    for (int i = 0; i < SEM_CAP; i++)
        n += SEMS[i] ? 1 : 0;
    return n;
}
