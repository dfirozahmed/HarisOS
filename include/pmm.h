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

unsigned long long pmm_total_pages(void); /* usable pages found in the map */
unsigned long long pmm_free_pages(void);

#endif
