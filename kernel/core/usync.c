/* HarisOS named mutexes and condition variables for processes: see include/usync.h. */
#include "usync.h"

typedef struct
{
    int used, linked, refs, kind;
    Mutex mutex;  /* kind == USYNC_MUTEX */
    CondVar cond; /* kind == USYNC_COND */
    char name[USYNC_NAME_MAX];
} Usync;

static Usync OBJS[USYNC_MAX];

static int valid(int s) { return s >= 0 && s < USYNC_MAX && OBJS[s].used; }

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
    for (int i = 0; i < USYNC_MAX; i++)
        if (OBJS[i].used && OBJS[i].linked && OBJS[i].kind == kind && same(OBJS[i].name, name))
            return i;
    return -1;
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
    for (s = 0; s < USYNC_MAX && OBJS[s].used; s++)
        ;
    if (s == USYNC_MAX)
        return -1;
    Usync *o = &OBJS[s];
    o->used = o->linked = 1;
    o->refs = 0;
    o->kind = kind;
    mutex_init(&o->mutex);
    cond_init(&o->cond);
    int n = 0;
    for (; name[n]; n++)
        o->name[n] = name[n];
    o->name[n] = 0;
    return s;
}

void usync_ref(int s)
{
    if (valid(s))
        OBJS[s].refs++;
}

void usync_unref(int s)
{
    if (!valid(s))
        return;
    if (OBJS[s].refs > 0)
        OBJS[s].refs--;
    if (OBJS[s].refs == 0 && !OBJS[s].linked)
        OBJS[s].used = 0;
}

int usync_unlink(const char *name, int kind)
{
    int s = find(name, kind);
    if (s < 0)
        return -1;
    OBJS[s].linked = 0;
    if (OBJS[s].refs == 0)
        OBJS[s].used = 0;
    return 0;
}

Mutex *usync_mutex(int s) { return valid(s) && OBJS[s].kind == USYNC_MUTEX ? &OBJS[s].mutex : 0; }

CondVar *usync_cond(int s) { return valid(s) && OBJS[s].kind == USYNC_COND ? &OBJS[s].cond : 0; }

int usync_in_use(void)
{
    int n = 0;
    for (int i = 0; i < USYNC_MAX; i++)
        n += OBJS[i].used ? 1 : 0;
    return n;
}
