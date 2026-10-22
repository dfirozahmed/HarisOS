/* HarisOS kernel heap: kmalloc / kfree for blocks of any size (no libc here).
 *
 * The page allocator (pmm.h) only hands out whole 4 KiB pages. The heap takes pages from it in big pieces ("arenas")
 * and cuts them into blocks of the sizes callers ask for.
 *
 *   char *p = kmalloc(100);          16-byte aligned, 100 bytes usable; NULL if there is no memory
 *   p = krealloc(p, 5000);           bigger or smaller; the content is kept; may move
 *   kfree(p);                        the block is reused; kfree(NULL) is fine
 *   int *a = kcalloc(n, sizeof *a);  zeroed, and n * size is checked for overflow
 *
 * How it works (see heap.c): every block has a 16-byte header with its size and its predecessor's size, so a freed
 * block merges with free neighbours at once (no fragmentation from neighbours that are both free). Free blocks are
 * kept in a list and the first one that fits is used (first fit); a bigger one is split. When all of an arena
 * is free again and it is not the only one, its pages go back to the page allocator.
 *
 * Rules: use it from the main loop only (it is not safe against interrupts); never keep a pointer to a block after
 * kfree / krealloc; a pointer must be one that kmalloc gave. Bad pointers given to kfree (twice-freed, not from the
 * heap, in the middle of a block) are ignored and counted in HeapStats.bad_frees instead of damaging the heap. */
#ifndef HARIS_HEAP_H
#define HARIS_HEAP_H

#define HEAP_ALIGN 16

/* Takes the first arena. Not required (the heap grows when the first request comes), but it makes sure the
 * page allocator is asked early, before memory gets fragmented. Returns 0, or -1 if there is no memory. */
int heap_init(void);

/* A block of at least `size` bytes (size 0 gives the smallest block), or NULL. */
void *kmalloc(unsigned long long size);
/* n * size bytes, all zero; NULL if the multiplication overflows or there is no memory. */
void *kcalloc(unsigned long long n, unsigned long long size);
/* Change the size of a block. NULL p = kmalloc; size 0 frees p and gives NULL. On failure the old block is left
 * untouched and NULL is returned. */
void *krealloc(void *p, unsigned long long size);
void kfree(void *p);

/* The usable bytes in the block (at least what was asked for); 0 for NULL. */
unsigned long long heap_block_size(const void *p);

typedef struct
{
    unsigned long long arena_bytes; /* pages taken from the page allocator, in bytes */
    unsigned long long used_bytes;  /* bytes in used blocks, headers included */
    unsigned long long free_bytes;  /* bytes in free blocks, headers included */
    unsigned long long used_blocks, free_blocks;
    unsigned long long largest_free; /* the biggest block a kmalloc could get without growing the heap */
    unsigned long long arenas;
    unsigned long long bad_frees; /* kfree calls that were refused because the pointer was not a used heap block */
} HeapStats;

void heap_stats(HeapStats *out);

/* Walks every arena and the free list and checks that everything is consistent (sizes, links, no two free blocks next
 * to each other, free list = the free blocks). 0 = fine, otherwise a number that says which check failed. For
 * tests and debugging. */
int heap_check(void);

#endif
