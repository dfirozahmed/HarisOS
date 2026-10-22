/* HarisOS physical memory manager (bitmap, 4 KiB pages). */
#ifndef HARIS_PMM_H
#define HARIS_PMM_H

#include "bootinfo.h"

#define PMM_PAGE_SIZE 4096ULL

/* Build the page bitmap from the boot memory map. Returns 0 on success. */
int pmm_init(const BootInfo *info);

/* Returns the physical address of a free page, or 0 if out of memory. */
unsigned long long pmm_alloc_page(void);
void pmm_free_page(unsigned long long phys);

/* `count` physically consecutive pages (e.g. for a frame buffer). 0 if none found. */
unsigned long long pmm_alloc_contiguous(unsigned long long count);
void pmm_free_contiguous(unsigned long long phys, unsigned long long count);

/* Who owns a used page, for the Task Manager's memory breakdown. The plain alloc calls above use
 * PMM_TAG_OTHER. A page keeps its tag until it is freed. */
typedef enum
{
    PMM_TAG_OTHER,
    PMM_TAG_SYSTEM,   /* the allocator's own tables */
    PMM_TAG_GRAPHICS, /* back buffer and wallpaper cache */
    PMM_TAG_USB,      /* USB controller rings and transfer buffers */
    PMM_TAG_STORAGE,  /* AHCI / NVMe queues and bounce buffers */
    PMM_TAG_HEAP,     /* the kernel heap (heap.h) */
    PMM_TAG_COUNT
} PmmTag;

unsigned long long pmm_alloc_page_tag(PmmTag tag);
unsigned long long pmm_alloc_contiguous_tag(unsigned long long count, PmmTag tag);
unsigned long long pmm_tag_pages(PmmTag tag); /* pages currently owned with this tag */

unsigned long long pmm_total_pages(void); /* usable pages found in the map */
unsigned long long pmm_free_pages(void);

#endif
