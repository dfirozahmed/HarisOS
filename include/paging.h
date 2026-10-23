/* ============================================================
 * HarisOS page table access (4-level x86-64 paging, the firmware's tables are still the ones in use)
 *
 *   paging_set_present(addr, 0);   the 4 KiB page at addr (identity mapped: virtual = physical) is no longer
 *                                  mapped: touching it raises a page fault #PF with CR2 = the address
 *   paging_set_present(addr, 1);   mapped again, with the attributes it had
 *
 * Used for guard pages: the page below a thread stack is unmapped, so a stack that grows too far stops with a
 * page fault that names the thread (see task.h, fault.h), instead of silently overwriting whatever lies below.
 *
 * The firmware maps RAM with 2 MiB (or 1 GiB) pages. A big page cannot have a hole in it, so the walk first splits it
 * into smaller pages with the same attributes (fbwc.c does the same for the frame buffer).
 *
 * paging_set_present_in() is the part without hardware access: it works on a table the caller gives it, and
 * the host test (tests/test_paging.c) runs it on tables built in ordinary memory.
 * ============================================================ */
#ifndef HARIS_PAGING_H
#define HARIS_PAGING_H

/* Switch to page tables of our own: a copy of everything the firmware mapped (same addresses, same memory types), but
 * writable. Call once, early (right after pmm_init, before the allocator hands out memory that gets used).
 * 0 = done, -1 = it could not be done (the firmware's tables stay in use and paging_set_present() refuses to work).
 *
 * Why not just edit the firmware's tables: some firmware (recent EDK2) maps its page tables, and the unused pages around
 * them, read-only. A write to them faults. And the page allocator hands out those unused pages as ordinary RAM, so
 * writing to such a page would fault too. In our copy every page is writable. */
int paging_init(void);
unsigned paging_tables(void); /* 4 KiB pages used by our tables (0 before paging_init) */

/* 0 = done, -1 = the address is not mapped at all, paging_init() did not succeed, or paging is in a form this code
 * does not handle. */
int paging_set_present(unsigned long long virt, int present);

/* A new, zeroed 4 KiB table (its address is a pointer: tables are identity mapped), or NULL. */
typedef unsigned long long *(*PagingAllocFn)(void);

/* The page-table entry (the 4 KiB level) for virt below pml4, after splitting any 1 GiB / 2 MiB page on the way.
 * NULL if virt is not mapped or alloc fails. Splitting changes nothing visible: the pages keep their attributes. */
unsigned long long *paging_pte(unsigned long long *pml4, unsigned long long virt, PagingAllocFn alloc);

/* A copy of the whole tree below old_pml4 with every entry writable (see paging_init), or NULL if alloc ran out. */
unsigned long long *paging_clone(const unsigned long long *old_pml4, PagingAllocFn alloc);

/* paging_pte() + set or clear the Present bit. No TLB flush: the caller does that. */
int paging_set_present_in(unsigned long long *pml4, unsigned long long virt, int present, PagingAllocFn alloc);

#endif
