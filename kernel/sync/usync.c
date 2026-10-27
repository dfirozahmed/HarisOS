/* HarisOS named mutexes and condition variables for processes: see include/usync.h. */
#include "usync.h"
#include "heap.h"

typedef struct
{
    int linked, refs, kind;
    Mutex mutex;  /* kind == USYNC_MUTEX */
    CondVar cond; /* kind == USYNC_COND */
    char name[USYNC_NAME_MAX];
} Usync;

/* The table is a heap block of OBJ_CAP pointers, doubled when full (up to USYNC_MAX); each object is a heap block of its own, so the Mutex
 * and CondVar that usync_mutex / usync_cond hand out stay where they are when the table moves (the scheduler may hold on to one while a
 * thread sleeps on it). An object is named by its index; a free index holds NULL and is reused. */
static Usync **OBJS;
static int OBJ_CAP;

static Usync *get(int s) { return s >= 0 && s < OBJ_CAP ? OBJS[s] : 0; }

int usync_valid_name(const char *name)
{
    int n = 0;
    for (; name[n]; n++)
        if (n >= USYNC_NAME_MAX - 1 || name[n] == '/' || (unsigned char)name[n] < 32)
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

static int find(const char *name, int kind)
{
    for (int i = 0; i < OBJ_CAP; i++)
        if (OBJS[i] && OBJS[i]->linked && OBJS[i]->kind == kind && same(OBJS[i]->name, name))
            return i;
    return -1;
}

/* Double the table (at most USYNC_MAX entries); the new entries are free. 0 = at the limit or out of memory. */
static int grow_table(void)
{
    if (OBJ_CAP >= USYNC_MAX)
        return 0;
    int cap = OBJ_CAP ? OBJ_CAP * 2 : 16;
    if (cap > USYNC_MAX)
        cap = USYNC_MAX;
    Usync **t = krealloc(OBJS, (unsigned long long)cap * sizeof(Usync *));
    if (!t)
        return 0;
    for (int i = OBJ_CAP; i < cap; i++)
        t[i] = 0;
    OBJS = t;
    OBJ_CAP = cap;
    return 1;
}

static void release(int s)
{
    mutex_destroy(&OBJS[s]->mutex); /* the sets of sleepers give their memory back */
    cond_destroy(&OBJS[s]->cond);
    kfree(OBJS[s]);
    OBJS[s] = 0;
}

int usync_open(const char *name, int kind, unsigned flags)
{
    if (!usync_valid_name(name) || (kind != USYNC_MUTEX && kind != USYNC_COND))
        return -1;
    int s = find(name, kind);
    if (s >= 0)
        return (flags & USYNC_CREATE) && (flags & USYNC_EXCL) ? -1 : s;
    if (!(flags & USYNC_CREATE))
        return -1;
    for (s = 0; s < OBJ_CAP && OBJS[s]; s++)
        ;
    if (s == OBJ_CAP && !grow_table())
        return -1;
    Usync *o = kmalloc(sizeof(Usync));
    if (!o)
        return -1;
    o->linked = 1;
    o->refs = 0;
    o->kind = kind;
    mutex_init(&o->mutex);
    cond_init(&o->cond);
    int n = 0;
    for (; name[n]; n++)
        o->name[n] = name[n];
    o->name[n] = 0;
    OBJS[s] = o;
    return s;
}

void usync_ref(int s)
{
    Usync *o = get(s);
    if (o)
        o->refs++;
}

void usync_unref(int s)
{
    Usync *o = get(s);
    if (!o)
        return;
    if (o->refs > 0)
        o->refs--;
    if (o->refs == 0 && !o->linked)
        release(s);
}

int usync_unlink(const char *name, int kind)
{
    int s = find(name, kind);
    if (s < 0)
        return -1;
    OBJS[s]->linked = 0;
    if (OBJS[s]->refs == 0)
        release(s);
    return 0;
}

Mutex *usync_mutex(int s)
{
    Usync *o = get(s);
    return o && o->kind == USYNC_MUTEX ? &o->mutex : 0;
}

CondVar *usync_cond(int s)
{
    Usync *o = get(s);
    return o && o->kind == USYNC_COND ? &o->cond : 0;
}

int usync_in_use(void)
{
    int n = 0;
    for (int i = 0; i < OBJ_CAP; i++)
        n += OBJS[i] ? 1 : 0;
    return n;
}
