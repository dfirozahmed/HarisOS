/* HarisOS kernel heap: first-fit free list with boundary tags. See include/heap.h.
 *
 * Memory layout of one arena (a run of pages from the page allocator):
 *
 *   [head sentinel][ block ][ block ] ... [ block ][tail sentinel]
 *      16 bytes                                      16 bytes
 *
 * Every block starts with a 16-byte header:
 *   size  total size of the block, header included (a multiple of 16), bit 0 = the block is in use
 *   prev  total size of the block just before it in memory (so the previous block can be found without a list)
 * A used block's memory after the header is the caller's. A free block keeps two list pointers there, so the
 * smallest block is 32 bytes. The sentinels are "used" blocks of size 16 and 0: they stop merging and walking at
 * the ends of an arena, so no code has to ask "is there a neighbour?".
 *
 * Under UEFI physical memory is identity mapped, so the addresses the page allocator returns are pointers.
 * No libc: copying and zeroing go word by word through volatile pointers (a plain loop could be turned by the
 * compiler into a call to memcpy / memset, which this freestanding build does not have).
 *
 * Threads: a preemptible thread can be stopped by the timer anywhere, so every public call (at the end of this file)
 * runs inside preempt_disable() / preempt_enable() (include/preempt.h). The *_nl ("no lock") functions do the work. */
#include "heap.h"
#include "pmm.h"
#include "preempt.h"

typedef unsigned long long u64;

typedef struct Block
{
    u64 size; /* total size | USED */
    u64 prev; /* total size of the previous block in memory */
} Block;

typedef struct Node /* kept inside a free block, after its header */
{
    Block *next, *prev;
} Node;

#define HDR 16ULL
#define MIN_BLOCK 32ULL            /* header + the two list pointers */
#define USED 1ULL
#define SIZE_MASK (~15ULL)
#define PAGE PMM_PAGE_SIZE
#define GROW_PAGES 64              /* a new arena is 256 KiB unless a request needs more */
#define MAX_ARENAS 128
#define MAX_REQUEST (1ULL << 40)   /* anything bigger is refused before it can overflow the arithmetic */

typedef struct
{
    u64 base;
    u64 pages;
} Arena;

static Arena ARENAS[MAX_ARENAS];
static int NARENAS;
static Block *FREE_HEAD;
static u64 BAD_FREES;

static u64 bsize(const Block *b) { return b->size & SIZE_MASK; }
static int bused(const Block *b) { return (int)(b->size & USED); }
static Block *next_blk(Block *b) { return (Block *)((char *)b + bsize(b)); }
static Block *prev_blk(Block *b) { return (Block *)((char *)b - b->prev); }
static Node *node_of(Block *b) { return (Node *)((char *)b + HDR); }
static void *payload(Block *b) { return (char *)b + HDR; }

/* ---- the free list ---- */
static void list_insert(Block *b)
{
    Node *n = node_of(b);
    n->prev = 0;
    n->next = FREE_HEAD;
    if (FREE_HEAD)
        node_of(FREE_HEAD)->prev = b;
    FREE_HEAD = b;
}

static void list_remove(Block *b)
{
    Node *n = node_of(b);
    if (n->prev)
        node_of(n->prev)->next = n->next;
    else
        FREE_HEAD = n->next;
    if (n->next)
        node_of(n->next)->prev = n->prev;
}

/* ---- words ---- */
static void copy_words(void *dst, const void *src, u64 bytes) /* both 8-byte aligned, bytes a multiple of 8 */
{
    volatile u64 *d = (volatile u64 *)dst;
    const volatile u64 *s = (const volatile u64 *)src;
    for (u64 i = 0; i < bytes / 8; i++)
        d[i] = s[i];
}

static void zero_words(void *dst, u64 bytes)
{
    volatile u64 *d = (volatile u64 *)dst;
    for (u64 i = 0; i < bytes / 8; i++)
        d[i] = 0;
}

/* ---- arenas ---- */
static Arena *arena_of(const void *p)
{
    u64 a = (u64)p;
    for (int i = 0; i < NARENAS; i++)
        if (a >= ARENAS[i].base && a < ARENAS[i].base + ARENAS[i].pages * PAGE)
            return &ARENAS[i];
    return 0;
}

/* Take pages from the page allocator and make them one big free block between two sentinels. */
static int grow(u64 need)
{
    if (NARENAS >= MAX_ARENAS)
        return 0;
    u64 pages = (need + 2 * HDR + PAGE - 1) / PAGE;
    u64 base = 0;
    if (pages < GROW_PAGES) /* a normal arena; if memory is too fragmented for it, just what is needed */
    {
        base = pmm_alloc_contiguous_tag(GROW_PAGES, PMM_TAG_HEAP);
        if (base)
            pages = GROW_PAGES;
    }
    if (!base)
        base = pmm_alloc_contiguous_tag(pages, PMM_TAG_HEAP);
    if (!base)
        return 0;

    u64 total = pages * PAGE;
    Block *head = (Block *)base;
    Block *first = (Block *)(base + HDR);
    Block *tail = (Block *)(base + total - HDR);
    head->size = HDR | USED;
    head->prev = 0;
    first->size = total - 2 * HDR; /* free */
    first->prev = HDR;
    tail->size = 0 | USED;
    tail->prev = first->size;
    ARENAS[NARENAS].base = base;
    ARENAS[NARENAS].pages = pages;
    NARENAS++;
    list_insert(first);
    return 1;
}

/* the first free block of at least `need` bytes */
static Block *find_fit(u64 need)
{
    for (Block *b = FREE_HEAD; b; b = node_of(b)->next)
        if (bsize(b) >= need)
            return b;
    return 0;
}

/* b is a block of size `cur` (already off the free list). Make its first `need` bytes a used block and give the rest
 * back as a free block (merged with a free block after it) if the rest is big enough to be a block. */
static void take(Block *b, u64 need)
{
    u64 cur = bsize(b);
    if (cur - need >= MIN_BLOCK)
    {
        u64 rest = cur - need;
        Block *r = (Block *)((char *)b + need);
        Block *after = (Block *)((char *)r + rest);
        b->size = need | USED;
        r->prev = need;
        if (!bused(after)) /* only happens when shrinking a block that sits before a free one */
        {
            list_remove(after);
            rest += bsize(after);
            after = (Block *)((char *)r + rest);
        }
        r->size = rest;
        after->prev = rest;
        list_insert(r);
    }
    else
        b->size = cur | USED; /* the rest is too small to be a block: it stays with this one */
}

static u64 block_need(u64 size)
{
    u64 need = (size + HDR + (HEAP_ALIGN - 1)) & ~(u64)(HEAP_ALIGN - 1);
    return need < MIN_BLOCK ? MIN_BLOCK : need;
}

static int heap_init_nl(void)
{
    if (NARENAS > 0)
        return 0;
    return grow(MIN_BLOCK) ? 0 : -1;
}

static void *kmalloc_nl(u64 size)
{
    if (size > MAX_REQUEST)
        return 0;
    u64 need = block_need(size);
    Block *b = find_fit(need);
    if (!b)
    {
        if (!grow(need))
            return 0;
        b = find_fit(need);
        if (!b)
            return 0;
    }
    list_remove(b);
    take(b, need);
    return payload(b);
}

void *kcalloc(u64 n, u64 size)
{
    if (n && size > MAX_REQUEST / n)
        return 0;
    void *p = kmalloc(n * size);
    if (p)
        zero_words(p, bsize((Block *)((char *)p - HDR)) - HDR); /* the whole block: its size is a multiple of 16 */
    return p;
}

/* Is p the payload of a used block that kmalloc made? (Cheap checks only: aligned, inside an arena, marked used,
 * and the block after it agrees about its size.) */
static Block *used_block(const void *p)
{
    u64 a = (u64)p;
    if (!p || (a % HEAP_ALIGN))
        return 0;
    Arena *ar = arena_of(p);
    if (!ar)
        return 0;
    Block *b = (Block *)(a - HDR);
    u64 lo = ar->base + HDR, hi = ar->base + ar->pages * PAGE - HDR;
    if ((u64)b < lo || (u64)b >= hi || !bused(b))
        return 0;
    u64 s = bsize(b);
    if (s < MIN_BLOCK || (u64)b + s > hi || next_blk(b)->prev != s)
        return 0;
    return b;
}

static void kfree_nl(void *p)
{
    if (!p)
        return;
    Block *b = used_block(p);
    if (!b)
    {
        BAD_FREES++;
        return;
    }
    u64 sz = bsize(b);
    Block *n = next_blk(b);
    if (!bused(n)) /* the tail sentinel is "used", so there is always a block here */
    {
        list_remove(n);
        sz += bsize(n);
    }
    Block *pb = prev_blk(b);
    if (!bused(pb)) /* ... and the head sentinel before the first block */
    {
        list_remove(pb);
        sz += bsize(pb);
        b = pb;
    }
    b->size = sz;
    ((Block *)((char *)b + sz))->prev = sz;

    /* an arena that is entirely free goes back to the page allocator (one stays, so the next kmalloc is quick) */
    Arena *ar = arena_of(b);
    if (NARENAS > 1 && ar && (u64)b == ar->base + HDR && sz == ar->pages * PAGE - 2 * HDR)
    {
        u64 base = ar->base, pages = ar->pages;
        *ar = ARENAS[NARENAS - 1];
        NARENAS--;
        pmm_free_contiguous(base, pages);
        return;
    }
    list_insert(b);
}

static void *krealloc_nl(void *p, u64 size)
{
    if (!p)
        return kmalloc(size);
    Block *b = used_block(p);
    if (!b)
    {
        BAD_FREES++;
        return 0;
    }
    if (size == 0)
    {
        kfree(p);
        return 0;
    }
    if (size > MAX_REQUEST)
        return 0;
    u64 need = block_need(size), cur = bsize(b);
    if (need <= cur) /* smaller (or the same): cut the end off in place */
    {
        take(b, need);
        return p;
    }
    Block *n = next_blk(b);
    if (!bused(n) && cur + bsize(n) >= need) /* the free block after it is enough: grow in place */
    {
        list_remove(n);
        u64 total = cur + bsize(n);
        b->size = total | USED;
        ((Block *)((char *)b + total))->prev = total;
        take(b, need); /* gives back what is more than needed */
        return p;
    }
    void *q = kmalloc(size); /* move: the old block stays valid if this fails */
    if (!q)
        return 0;
    copy_words(q, p, cur - HDR);
    kfree(p);
    return q;
}

u64 heap_block_size(const void *p)
{
    Block *b = used_block(p);
    return b ? bsize(b) - HDR : 0;
}

static void heap_stats_nl(HeapStats *out)
{
    out->arena_bytes = out->used_bytes = out->free_bytes = out->used_blocks = out->free_blocks = out->largest_free = 0;
    out->arenas = (u64)NARENAS;
    out->bad_frees = BAD_FREES;
    for (int i = 0; i < NARENAS; i++)
    {
        out->arena_bytes += ARENAS[i].pages * PAGE;
        for (Block *b = (Block *)(ARENAS[i].base + HDR); bsize(b) != 0; b = next_blk(b))
        {
            if (bused(b))
            {
                out->used_bytes += bsize(b);
                out->used_blocks++;
            }
            else
            {
                out->free_bytes += bsize(b);
                out->free_blocks++;
                if (bsize(b) > out->largest_free)
                    out->largest_free = bsize(b);
            }
        }
    }
}

static int heap_check_nl(void)
{
    u64 free_seen = 0;
    for (int i = 0; i < NARENAS; i++)
    {
        u64 base = ARENAS[i].base, end = base + ARENAS[i].pages * PAGE;
        Block *head = (Block *)base;
        if (head->size != (HDR | USED) || head->prev != 0)
            return 1;
        u64 prev = HDR;
        int prev_free = 0;
        Block *b = (Block *)(base + HDR);
        for (;;)
        {
            u64 s = bsize(b);
            if ((u64)b >= end)
                return 2;
            if (s == 0) /* the tail sentinel */
            {
                if ((u64)b != end - HDR || !bused(b) || b->prev != prev)
                    return 3;
                break;
            }
            if ((s % 16) || s < MIN_BLOCK || (u64)b + s > end - HDR || b->prev != prev)
                return 4;
            if (!bused(b))
            {
                if (prev_free)
                    return 5; /* two free blocks next to each other: they should have merged */
                free_seen++;
            }
            prev_free = !bused(b);
            prev = s;
            b = next_blk(b);
        }
    }
    u64 listed = 0;
    Block *last = 0;
    for (Block *b = FREE_HEAD; b; b = node_of(b)->next)
    {
        if (++listed > free_seen)
            return 6; /* a cycle, or blocks in the list that are not free */
        if (bused(b) || !arena_of(b) || node_of(b)->prev != last)
            return 7;
        last = b;
    }
    return listed == free_seen ? 0 : 8;
}

/* ---- public entry points: the work above, with the timer interrupt not allowed to switch threads meanwhile ---- */
int heap_init(void)
{
    preempt_disable();
    int r = heap_init_nl();
    preempt_enable();
    return r;
}

void *kmalloc(u64 size)
{
    preempt_disable();
    void *p = kmalloc_nl(size);
    preempt_enable();
    return p;
}

void kfree(void *p)
{
    preempt_disable();
    kfree_nl(p);
    preempt_enable();
}

void *krealloc(void *p, u64 size)
{
    preempt_disable();
    void *q = krealloc_nl(p, size);
    preempt_enable();
    return q;
}

void heap_stats(HeapStats *out)
{
    preempt_disable();
    heap_stats_nl(out);
    preempt_enable();
}

int heap_check(void)
{
    preempt_disable();
    int r = heap_check_nl();
    preempt_enable();
    return r;
}
