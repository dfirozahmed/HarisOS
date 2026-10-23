/* ============================================================
 * HarisOS copy-on-write: how many address spaces hold a physical page
 *
 * fork does not copy memory: parent and child map the same physical pages, read-only, and a page is copied only when one of them writes
 * to it. This file keeps count of who holds a shared page. A page that is not in the table is held by exactly one address space.
 *
 *   cow_share(phys)      one more holder of the page (the second one, the third ...): 0, or -1 if the table is full (then the caller
 *                        copies the page instead of sharing it)
 *   cow_release(phys)    one holder lets go (it copies the page, or its address space ends): 1 if it was the last holder, so the page
 *                        has to be freed; 0 if others still hold it
 *   cow_shared(phys)     is somebody else holding this page too?
 *
 * Everything runs with interrupts off (system calls, page faults): no locks.
 * ============================================================ */
#ifndef HARIS_COW_H
#define HARIS_COW_H

#define COW_TABLE_SIZE 65536 /* pages that can be shared at once: 256 MiB */

int cow_share(unsigned long long phys);
int cow_release(unsigned long long phys);
int cow_shared(unsigned long long phys);
unsigned cow_shared_pages(void); /* pages in the table now (0 when no copy-on-write memory exists: the leak check) */

#endif
