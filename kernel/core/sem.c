/* HarisOS semaphores: see include/sem.h. */
#include "sem.h"
#include "task.h"

typedef struct
{
    int used, linked, refs;
    unsigned value;
    unsigned waiters; /* bit i = thread i sleeps in a wait */
    char name[SEM_NAME_MAX];
} Semaphore;

static Semaphore SEMS[SEM_MAX];

int sem_valid(int s) { return s >= 0 && s < SEM_MAX && SEMS[s].used; }

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
    for (int i = 0; i < SEM_MAX; i++)
        if (SEMS[i].used && SEMS[i].linked && same(SEMS[i].name, name))
            return i;
    return -1;
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
    for (s = 0; s < SEM_MAX && SEMS[s].used; s++)
        ;
    if (s == SEM_MAX)
        return -1;
    Semaphore *m = &SEMS[s];
    m->used = m->linked = 1;
    m->refs = 0;
    m->value = initial;
    m->waiters = 0;
    int n = 0;
    for (; name[n]; n++)
        m->name[n] = name[n];
    m->name[n] = 0;
    return s;
}

void sem_ref(int s)
{
    if (sem_valid(s))
        SEMS[s].refs++;
}

void sem_unref(int s)
{
    if (!sem_valid(s))
        return;
    if (SEMS[s].refs > 0)
        SEMS[s].refs--;
    if (SEMS[s].refs == 0 && !SEMS[s].linked)
        SEMS[s].used = 0;
}

int sem_unlink(const char *name)
{
    int s = find(name);
    if (s < 0)
        return -1;
    SEMS[s].linked = 0;
    if (SEMS[s].refs == 0)
        SEMS[s].used = 0;
    return 0;
}

int sem_trywait(int s)
{
    if (!sem_valid(s))
        return -1;
    if (SEMS[s].value == 0)
        return SEM_WOULDBLOCK;
    SEMS[s].value--;
    return 0;
}

int sem_post(int s)
{
    if (!sem_valid(s) || SEMS[s].value >= SEM_VALUE_MAX)
        return -1;
    SEMS[s].value++;
    unsigned m = SEMS[s].waiters;
    SEMS[s].waiters = 0;
    for (int t = 0; t < TASK_MAX; t++)
        if (m & (1u << t))
            task_wake(t);
    return 0;
}

void sem_wait_for(int s, int task)
{
    if (sem_valid(s) && task >= 0 && task < TASK_MAX)
        SEMS[s].waiters |= 1u << task;
}

int sem_value(int s) { return sem_valid(s) ? (int)SEMS[s].value : -1; }

int sem_in_use(void)
{
    int n = 0;
    for (int i = 0; i < SEM_MAX; i++)
        n += SEMS[i].used ? 1 : 0;
    return n;
}
