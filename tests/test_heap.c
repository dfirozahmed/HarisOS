/* Host-side tests for the kernel heap (kernel/memory/heap.c). The page allocator is replaced by one that takes its pages
 * from the host, so the tests can limit it, count what is outstanding and find leaks. */
#include "heap.h"
#include "pmm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the page allocator, on the host ---- */
typedef struct
{
    unsigned long long addr, pages;
    void *raw;
} Chunk;
static Chunk CHUNKS[512];
static unsigned long long PAGES_OUT;   /* pages handed out and not given back */
static unsigned long long PAGES_LIMIT = ~0ULL;
static unsigned long long ALLOC_CALLS, FREE_CALLS;
static int LAST_TAG = -1;

unsigned long long pmm_alloc_contiguous_tag(unsigned long long count, PmmTag tag)
{
    ALLOC_CALLS++;
    if (count == 0 || PAGES_OUT + count > PAGES_LIMIT)
        return 0;
    for (int i = 0; i < 512; i++)
        if (!CHUNKS[i].raw)
        {
            void *raw = calloc(count * PMM_PAGE_SIZE + PMM_PAGE_SIZE, 1);
            if (!raw)
                return 0;
            unsigned long long a = ((unsigned long long)raw + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
            CHUNKS[i].addr = a;
            CHUNKS[i].pages = count;
            CHUNKS[i].raw = raw;
            PAGES_OUT += count;
            LAST_TAG = (int)tag;
            return a;
        }
    return 0;
}

void pmm_free_contiguous(unsigned long long phys, unsigned long long count)
{
    FREE_CALLS++;
    for (int i = 0; i < 512; i++)
        if (CHUNKS[i].raw && CHUNKS[i].addr == phys)
        {
            assert(CHUNKS[i].pages == count);
            free(CHUNKS[i].raw);
            CHUNKS[i].raw = 0;
            PAGES_OUT -= count;
            return;
        }
    assert(!"pmm_free_contiguous: not an address the stub handed out");
}

/* ---- helpers ---- */
static unsigned SEED = 12345;
static unsigned rnd(void)
{
    SEED = SEED * 1103515245u + 12345u;
    return SEED >> 8;
}

static void fill(unsigned char *p, unsigned long long n, unsigned char v)
{
    for (unsigned long long i = 0; i < n; i++)
        p[i] = v;
}

static int filled(const unsigned char *p, unsigned long long n, unsigned char v)
{
    for (unsigned long long i = 0; i < n; i++)
        if (p[i] != v)
            return 0;
    return 1;
}

int main(void)
{
    HeapStats s;

    /* the first arena, tagged for the Task Manager */
    assert(heap_init() == 0 && heap_check() == 0 && heap_init() == 0);
    assert(LAST_TAG == PMM_TAG_HEAP && PAGES_OUT == 64);
    heap_stats(&s);
    assert(s.arenas == 1 && s.used_blocks == 0 && s.free_blocks == 1 && s.free_bytes == 64 * 4096 - 32);

    /* alignment, sizes, no overlap */
    unsigned char *a = kmalloc(100), *b = kmalloc(1), *c = kmalloc(5000), *z = kmalloc(0);
    assert(a && b && c && z && a != b && b != c && z != a);
    assert(((unsigned long long)a % 16) == 0 && ((unsigned long long)b % 16) == 0 && ((unsigned long long)c % 16) == 0);
    assert(heap_block_size(a) >= 100 && heap_block_size(b) >= 1 && heap_block_size(c) >= 5000 && heap_block_size(z) >= 16);
    fill(a, 100, 0xA1);
    fill(b, 1, 0xB2);
    fill(c, 5000, 0xC3);
    assert(filled(a, 100, 0xA1) && filled(b, 1, 0xB2) && filled(c, 5000, 0xC3));
    assert(heap_check() == 0);
    heap_stats(&s);
    assert(s.used_blocks == 4);

    /* a freed block is reused; neighbours merge: after freeing all, one block is left */
    kfree(b);
    unsigned char *b2 = kmalloc(1);
    assert(b2 == b);                                   /* the same hole */
    kfree(b2);
    kfree(a);                                          /* a and the hole next to it become one block */
    assert(heap_check() == 0);
    kfree(c);
    kfree(z);
    assert(heap_check() == 0);
    heap_stats(&s);
    assert(s.used_blocks == 0 && s.free_blocks == 1 && s.free_bytes == 64 * 4096 - 32 && s.largest_free == s.free_bytes);

    /* first fit splits: two small blocks come from the start, the rest stays one block */
    unsigned char *p1 = kmalloc(48), *p2 = kmalloc(48);
    assert(p2 == p1 + 64);                             /* header 16 + payload 48 */
    heap_stats(&s);
    assert(s.free_blocks == 1 && s.used_blocks == 2);
    kfree(p1);
    kfree(p2);

    /* a block that would leave less than a minimum block behind is not split: the caller gets a bit more */
    unsigned char *q1 = kmalloc(100), *q2 = kmalloc(100);
    kfree(q1);
    unsigned char *q3 = kmalloc(96);                   /* needs 112, the hole is 128: the rest (16) is too small to split */
    assert(q3 == q1 && heap_block_size(q3) == 112);
    assert(heap_check() == 0);
    kfree(q3);
    kfree(q2);

    /* realloc: shrink and grow in place, move with the content kept, edge cases */
    unsigned char *r = kmalloc(100);
    fill(r, 100, 0x5A);
    unsigned char *r2 = krealloc(r, 40);
    assert(r2 == r && filled(r2, 40, 0x5A));           /* shrunk in place */
    unsigned char *r3 = krealloc(r2, 3000);
    assert(r3 == r2 && filled(r3, 40, 0x5A));          /* grown in place: the free space was right behind it */
    unsigned char *blocker = kmalloc(16);
    unsigned char *r4 = krealloc(r3, 6000);            /* blocked by `blocker`: it has to move */
    assert(r4 && r4 != r3 && filled(r4, 40, 0x5A));
    assert(heap_check() == 0);
    unsigned char *from_null = krealloc(0, 64); /* NULL p = malloc */
    assert(from_null != 0);
    kfree(from_null);
    assert(krealloc(r4, 0) == 0);                      /* size 0 frees */
    kfree(blocker);
    assert(heap_check() == 0);
    {
        unsigned char *junk = (unsigned char *)(r4 + 1);
        unsigned long long bad = 0;
        heap_stats(&s);
        bad = s.bad_frees;
        assert(krealloc(junk, 10) == 0);               /* a pointer that is not a block */
        heap_stats(&s);
        assert(s.bad_frees == bad + 1);
    }

    /* calloc: zeroed (also over a block that held data), and the overflow check */
    unsigned char *dirty = kmalloc(256);
    fill(dirty, 256, 0xFF);
    kfree(dirty);
    unsigned char *cl = kcalloc(16, 16);
    assert(cl == dirty && filled(cl, 256, 0));
    assert(kcalloc(~0ULL, 2) == 0 && kcalloc(1ULL << 63, 4) == 0 && kcalloc(1ULL << 41, 1) == 0);
    kfree(cl);
    assert(kmalloc(~0ULL) == 0 && kmalloc((1ULL << 40) + 1) == 0);

    /* bad frees are ignored and counted: twice, from outside, in the middle of a block, misaligned */
    heap_stats(&s);
    unsigned long long bad0 = s.bad_frees;
    unsigned char *f = kmalloc(200);
    kfree(f);
    kfree(f);                                          /* double free */
    int on_stack;
    kfree(&on_stack);
    unsigned char *g = kmalloc(200);
    kfree(g + 16);                                     /* inside a block */
    kfree(g + 1);                                      /* not aligned */
    kfree(0);                                          /* NULL is fine and not counted */
    heap_stats(&s);
    assert(s.bad_frees == bad0 + 4 && heap_check() == 0);
    kfree(g);

    /* a big request gets its own arena; freeing it gives the pages back (the first arena stays) */
    unsigned long long pages_before = PAGES_OUT;
    unsigned char *big = kmalloc(1024 * 1024);
    assert(big && PAGES_OUT > pages_before && PAGES_OUT - pages_before >= 256);
    heap_stats(&s);
    assert(s.arenas == 2);
    fill(big, 1024 * 1024, 0x77);
    assert(filled(big, 1024 * 1024, 0x77) && heap_check() == 0);
    kfree(big);
    assert(PAGES_OUT == pages_before && heap_check() == 0);
    heap_stats(&s);
    assert(s.arenas == 1);

    /* out of memory: NULL, and nothing is damaged; later requests work again when memory comes back */
    unsigned long long saved_limit = PAGES_LIMIT;
    PAGES_LIMIT = PAGES_OUT;                           /* no more pages */
    unsigned char *held[2000];
    int nheld = 0;
    for (;; nheld++)
    {
        held[nheld] = kmalloc(1000);
        if (!held[nheld])
            break;
        assert(nheld < 1999);
    }
    assert(nheld > 100 && heap_check() == 0);
    assert(kmalloc(5 * 1024 * 1024) == 0);
    /* the heap is full and the page allocator has 10 pages left: a normal 64-page arena fails, one of the needed size works */
    PAGES_LIMIT = PAGES_OUT + 10;
    unsigned long long out0 = PAGES_OUT;
    unsigned char *odd = kmalloc(20000);
    assert(odd && PAGES_OUT - out0 == 5 && heap_check() == 0);
    kfree(odd);
    assert(PAGES_OUT == out0); /* the small arena went back */
    PAGES_LIMIT = out0;
    for (int i = 0; i < nheld; i++)
        kfree(held[i]);
    assert(heap_check() == 0);
    PAGES_LIMIT = saved_limit;
    unsigned char *again = kmalloc(1000);
    assert(again);
    kfree(again);

    /* random stress: sizes from 1 byte to 40 KB, content checked, the heap verified now and then */
    enum { SLOTS = 400 };
    static unsigned char *slot[SLOTS];
    static unsigned long long len[SLOTS];
    static unsigned char tag[SLOTS];
    for (int round = 0; round < 60000; round++)
    {
        int i = (int)(rnd() % SLOTS);
        if (slot[i])
        {
            assert(filled(slot[i], len[i], tag[i])); /* nobody scribbled over it */
            if (rnd() % 3 == 0)
            {
                unsigned long long n = 1 + rnd() % 6000;
                unsigned char *np = krealloc(slot[i], n);
                if (np)
                {
                    unsigned long long keep = n < len[i] ? n : len[i];
                    assert(filled(np, keep, tag[i]));
                    slot[i] = np;
                    len[i] = n;
                    fill(np, n, tag[i]);
                }
            }
            else
            {
                kfree(slot[i]);
                slot[i] = 0;
            }
        }
        else
        {
            unsigned long long n = rnd() % 10 == 0 ? 1 + rnd() % 40000 : 1 + rnd() % 700;
            slot[i] = kmalloc(n);
            if (slot[i])
            {
                len[i] = n;
                tag[i] = (unsigned char)(1 + rnd() % 250);
                assert(heap_block_size(slot[i]) >= n && ((unsigned long long)slot[i] % 16) == 0);
                fill(slot[i], n, tag[i]);
            }
        }
        if (round % 1000 == 0)
            assert(heap_check() == 0);
    }
    /* the blocks do not overlap */
    for (int i = 0; i < SLOTS; i++)
        for (int j = i + 1; j < SLOTS; j++)
            if (slot[i] && slot[j])
                assert(slot[i] + len[i] <= slot[j] || slot[j] + len[j] <= slot[i]);
    for (int i = 0; i < SLOTS; i++)
        if (slot[i])
        {
            assert(filled(slot[i], len[i], tag[i]));
            kfree(slot[i]);
        }
    assert(heap_check() == 0);
    heap_stats(&s);
    assert(s.used_blocks == 0 && s.free_blocks == s.arenas);  /* everything merged: one free block per arena */
    assert(s.arenas == 1 && PAGES_OUT == 64);                  /* ... and the extra arenas were given back */

    printf("heap tests passed (%llu page requests, %llu releases)\n", ALLOC_CALLS, FREE_CALLS);
    return 0;
}
