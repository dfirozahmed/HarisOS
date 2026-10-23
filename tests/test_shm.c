/* Host test for shared memory segments (kernel/core/shm.c): names, creation, sizes, references, unlink, freeing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmm.h"
#include "shm.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* a fake page allocator: real host memory, counted, with a limit */
static int PAGES_OUT, PAGE_LIMIT = 100000;
unsigned long long pmm_alloc_page_tag(PmmTag tag)
{
    (void)tag;
    if (PAGES_OUT >= PAGE_LIMIT)
        return 0;
    void *p = calloc(1, 4096);
    if (!p)
        return 0;
    PAGES_OUT++;
    return (unsigned long long)p;
}

void pmm_free_page(unsigned long long phys)
{
    free((void *)phys);
    PAGES_OUT--;
}

/* the kernel heap (shm.c keeps its page lists there, gui.c its surfaces): host memory, counted, with a switch that makes it fail */
static int HEAP_BLOCKS, HEAP_FAIL;
void *kmalloc(unsigned long long size)
{
    if (HEAP_FAIL)
        return 0;
    void *p = calloc(1, size ? (size_t)size : 1);
    if (p)
        HEAP_BLOCKS++;
    return p;
}
void kfree(void *p)
{
    if (p)
    {
        free(p);
        HEAP_BLOCKS--;
    }
}

int main(void)
{
    /* names */
    CHECK(shm_valid_name("a") && shm_valid_name("frame.buffer-1"));
    CHECK(!shm_valid_name("") && !shm_valid_name("a/b") && !shm_valid_name("tab\there"));
    char longname[64];
    memset(longname, 'n', sizeof(longname));
    longname[SHM_NAME_MAX] = 0;
    CHECK(!shm_valid_name(longname));                    /* 32 characters do not fit with the final 0 */
    longname[SHM_NAME_MAX - 1] = 0;
    CHECK(shm_valid_name(longname));                     /* 31 do */

    /* create: size rounded up to pages, zeroed, found again by name */
    int a = shm_open("alpha", 5000, SHM_CREATE);
    CHECK(a >= 0 && shm_pages(a) == 2 && PAGES_OUT == 2 && shm_in_use() == 1);
    CHECK(shm_page(a, 0) != 0 && shm_page(a, 1) != 0 && shm_page(a, 0) != shm_page(a, 1) && shm_page(a, 2) == 0);
    int zero = 1;
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 4096; i++)
            zero = zero && ((unsigned char *)shm_page(a, (unsigned)p))[i] == 0;
    CHECK(zero);
    ((unsigned char *)shm_page(a, 1))[100] = 0x42;
    CHECK(shm_open("alpha", 0, 0) == a && shm_open("alpha", 99999999, SHM_CREATE) == a); /* an existing one: its size stays */
    CHECK(shm_pages(a) == 2);
    CHECK(((unsigned char *)shm_page(shm_open("alpha", 0, 0), 1))[100] == 0x42);          /* the same pages */
    CHECK(shm_open("alpha", 100, SHM_CREATE | SHM_EXCL) == -1);                          /* it exists */

    /* what cannot be made */
    CHECK(shm_open("beta", 100, 0) == -1);                 /* no CREATE and no such name */
    CHECK(shm_open("beta", 0, SHM_CREATE) == -1);          /* an empty one */
    CHECK(shm_open("beta", (unsigned long long)SHM_MAX_PAGES * 4096 + 1, SHM_CREATE) == -1);  /* too big */
    CHECK(shm_open("bad/name", 100, SHM_CREATE) == -1 && shm_open("", 100, SHM_CREATE) == -1);
    CHECK(shm_in_use() == 1 && PAGES_OUT == 2);            /* nothing was left behind */
    int big = shm_open("big", (unsigned long long)SHM_MAX_PAGES * 4096, SHM_CREATE);
    CHECK(big >= 0 && shm_pages(big) == SHM_MAX_PAGES && PAGES_OUT == 2 + SHM_MAX_PAGES);

    /* memory runs out half way: everything taken is given back */
    PAGE_LIMIT = PAGES_OUT + 3;
    CHECK(shm_open("greedy", 10 * 4096, SHM_CREATE) == -1 && PAGES_OUT == 2 + SHM_MAX_PAGES && shm_in_use() == 2);
    PAGE_LIMIT = 100000;

    /* references and unlink: a mapped segment outlives its name */
    shm_ref(a);
    shm_ref(a);
    CHECK(shm_unlink("alpha") == 0);
    CHECK(shm_open("alpha", 0, 0) == -1);                  /* the name is gone ... */
    CHECK(shm_pages(a) == 2 && shm_page(a, 0) != 0);       /* ... the pages are not */
    int again = shm_open("alpha", 4096, SHM_CREATE);       /* the name can be used again, for a new segment */
    CHECK(again >= 0 && again != a && shm_pages(again) == 1);
    shm_unref(a);
    CHECK(shm_pages(a) == 2);                              /* one mapping left */
    int before = PAGES_OUT;
    shm_unref(a);
    CHECK(shm_pages(a) == 0 && PAGES_OUT == before - 2);   /* the last one ended: freed */
    shm_unref(a);                                          /* a stale number is harmless */
    shm_ref(a);
    CHECK(shm_pages(a) == 0);

    /* unlink with nobody mapping: freed at once; unlink twice and of unknown names */
    CHECK(shm_unlink("alpha") == 0 && shm_open("alpha", 0, 0) == -1);
    CHECK(shm_unlink("alpha") == -1 && shm_unlink("never") == -1 && shm_unlink("") == -1);
    CHECK(shm_unlink("big") == 0 && PAGES_OUT == 0 && shm_in_use() == 0);

    /* all the slots */
    char nm[8];
    int made = 0;
    for (int i = 0; i <= SHM_MAX; i++)
    {
        snprintf(nm, sizeof(nm), "s%d", i);
        if (shm_open(nm, 100, SHM_CREATE) >= 0)
            made++;
    }
    CHECK(made == SHM_MAX && shm_open("one-too-many", 100, SHM_CREATE) == -1);
    for (int i = 0; i < SHM_MAX; i++)
    {
        snprintf(nm, sizeof(nm), "s%d", i);
        CHECK(shm_unlink(nm) == 0);
    }
    CHECK(shm_in_use() == 0 && PAGES_OUT == 0 && HEAP_BLOCKS == 0); /* every page list went back to the heap */

    /* the heap is out of memory: no segment, no pages, no list left behind */
    HEAP_FAIL = 1;
    CHECK(shm_open("nolist", 4096, SHM_CREATE) == -1);
    HEAP_FAIL = 0;
    CHECK(shm_in_use() == 0 && PAGES_OUT == 0 && HEAP_BLOCKS == 0);

    /* the largest segment: a 16 MiB page list is kept in the heap, not in the table */
    int most = shm_open("most", (unsigned long long)SHM_MAX_PAGES * 4096, SHM_CREATE);
    CHECK(most >= 0 && shm_pages(most) == SHM_MAX_PAGES && HEAP_BLOCKS == 1 && shm_page(most, SHM_MAX_PAGES - 1) != 0);
    CHECK(shm_unlink("most") == 0 && shm_in_use() == 0 && PAGES_OUT == 0 && HEAP_BLOCKS == 0);

    if (fails == 0)
        printf("shm tests passed\n");
    return fails ? 1 : 0;
}
