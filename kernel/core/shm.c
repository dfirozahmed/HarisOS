/* HarisOS shared memory segments: see include/shm.h. */
#include "shm.h"
#include "heap.h"
#include "pmm.h"

typedef unsigned long long u64;

typedef struct
{
    int used;
    int linked; /* the name is still there */
    int refs;   /* mappings */
    unsigned pages;
    char name[SHM_NAME_MAX];
    u64 *phys; /* physical address of each page: a heap block of `pages` entries, made with the segment */
} Segment;

static Segment *SEGS; /* the table: a heap block of SEG_CAP entries, doubled when full (callers only ever hold indexes, so it may move) */
static int SEG_CAP;

static int names_equal(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

int shm_valid_name(const char *name)
{
    int n = 0;
    for (; name[n]; n++)
        if (n >= SHM_NAME_MAX - 1 || name[n] == '/' || (unsigned char)name[n] < 32)
            return 0;
    return n > 0;
}

static void release(Segment *s)
{
    for (unsigned i = 0; i < s->pages; i++)
        if (s->phys[i])
            pmm_free_page(s->phys[i]);
    kfree(s->phys); /* (NULL if the list was never made) */
    s->phys = 0;
    s->used = 0;
    s->pages = 0;
}

/* Double the table (at most SHM_MAX entries); the new entries are unused. 0 = at the limit or out of memory. */
static int grow_table(void)
{
    if (SEG_CAP >= SHM_MAX)
        return 0;
    int cap = SEG_CAP ? SEG_CAP * 2 : 16;
    if (cap > SHM_MAX)
        cap = SHM_MAX;
    Segment *t = krealloc(SEGS, (u64)cap * sizeof(Segment));
    if (!t)
        return 0;
    for (int i = SEG_CAP; i < cap; i++)
        t[i].used = 0;
    SEGS = t;
    SEG_CAP = cap;
    return 1;
}

static int find(const char *name)
{
    for (int i = 0; i < SEG_CAP; i++)
        if (SEGS[i].used && SEGS[i].linked && names_equal(SEGS[i].name, name))
            return i;
    return -1;
}

int shm_open(const char *name, u64 size, unsigned flags)
{
    if (!shm_valid_name(name))
        return -1;
    int s = find(name);
    if (s >= 0)
        return (flags & SHM_CREATE) && (flags & SHM_EXCL) ? -1 : s;
    if (!(flags & SHM_CREATE) || size == 0 || size > (u64)SHM_MAX_PAGES * PMM_PAGE_SIZE)
        return -1;
    for (s = 0; s < SEG_CAP && SEGS[s].used; s++)
        ;
    if (s == SEG_CAP && !grow_table())
        return -1;
    Segment *g = &SEGS[s];
    unsigned pages = (unsigned)((size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    g->used = 1;
    g->linked = 1;
    g->refs = 0;
    g->pages = 0;
    int n = 0;
    for (; name[n]; n++)
        g->name[n] = name[n]; /* (shm_valid_name checked the length) */
    g->name[n] = 0;
    g->phys = kmalloc((u64)pages * sizeof(u64));
    if (!g->phys)
    {
        release(g);
        return -1;
    }
    for (unsigned i = 0; i < pages; i++)
    {
        u64 p = pmm_alloc_page_tag(PMM_TAG_OTHER);
        if (!p)
        {
            release(g);
            return -1;
        }
        volatile u64 *w = (volatile u64 *)p; /* a fresh segment is zero */
        for (int k = 0; k < 512; k++)
            w[k] = 0;
        g->phys[i] = p;
        g->pages = i + 1;
    }
    return s;
}

unsigned shm_pages(int s) { return s >= 0 && s < SEG_CAP && SEGS[s].used ? SEGS[s].pages : 0; }

u64 shm_page(int s, unsigned i) { return s >= 0 && s < SEG_CAP && SEGS[s].used && i < SEGS[s].pages ? SEGS[s].phys[i] : 0; }

void shm_ref(int s)
{
    if (s >= 0 && s < SEG_CAP && SEGS[s].used)
        SEGS[s].refs++;
}

void shm_unref(int s)
{
    if (s < 0 || s >= SEG_CAP || !SEGS[s].used)
        return;
    if (SEGS[s].refs > 0)
        SEGS[s].refs--;
    if (SEGS[s].refs == 0 && !SEGS[s].linked)
        release(&SEGS[s]);
}

int shm_unlink(const char *name)
{
    int s = find(name);
    if (s < 0)
        return -1;
    SEGS[s].linked = 0;
    if (SEGS[s].refs == 0)
        release(&SEGS[s]);
    return 0;
}

int shm_in_use(void)
{
    int n = 0;
    for (int i = 0; i < SEG_CAP; i++)
        n += SEGS[i].used ? 1 : 0;
    return n;
}
