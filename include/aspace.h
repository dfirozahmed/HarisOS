/* ============================================================
 * HarisOS address spaces: one set of page tables per user process
 *
 *   aspace_init();                         once, after paging_init(): learn the kernel's tables
 *   AddressSpace *as = aspace_create();    an empty user space; the kernel half is shared with every other space
 *   aspace_map_new(as, va, ASPACE_WRITE);  a fresh zeroed page at user address va (PAGING_USER_BASE ... END)
 *   aspace_switch(as);                     make it the one the CPU uses (aspace_switch(0) = back to the kernel's)
 *   aspace_destroy(as);                    free its pages and tables
 *
 * What is shared and what is not: the kernel identity maps RAM in the low top-level slots. Every space copies those
 * slots, which point at the same lower tables, so the kernel's code, data, stacks, IDT and the memory-mapped devices
 * are at the same addresses in every space and an interrupt or system call can run in whichever space is loaded.
 * The user slots (paging.h: PAGING_USER_SLOT_FIRST..LAST) are empty in the kernel and private to each space.
 *
 * The kernel reaches a user page by its physical address (RAM is identity mapped), without switching spaces:
 * aspace_translate() gives that address.
 *
 * Not yet: SMEP / SMAP (they stop the kernel from running or touching user pages by mistake), a lock (all callers
 * are the main loop or a thread that has called preempt_disable).
 * ============================================================ */
#ifndef HARIS_ASPACE_H
#define HARIS_ASPACE_H

#include "paging.h"

/* The top-level table itself: 512 entries. */
typedef struct AddressSpace
{
    unsigned long long entry[512];
} AddressSpace;

#define ASPACE_WRITE PAGING_WRITE
#define ASPACE_EXEC PAGING_EXEC

/* 0 = ready, -1 = user spaces cannot be used here (the kernel's tables use the user slots, No-Execute is not
 * enabled in the CPU, or paging_init() did not succeed). */
int aspace_init(void);

AddressSpace *aspace_create(void);                                 /* NULL: no memory, or aspace_init failed */
AddressSpace *aspace_clone(const AddressSpace *src);                 /* a copy of a user space (fork): its own pages with the same contents; NULL if memory ran out */
int aspace_cow_fault(AddressSpace *as, unsigned long long va); /* a write to a copy-on-write page: 1 = made private + writable, 0 = not such a page, -1 = no memory */
int aspace_map_shared(AddressSpace *as, unsigned long long va, unsigned long long phys, unsigned flags); /* an existing page (shared memory), not owned: 0 or -1 */
int aspace_unmap_shared(AddressSpace *as, unsigned long long va);    /* take such a page out again (flushes the TLB entry): 0 or -1 */
int aspace_map_new(AddressSpace *as, unsigned long long va, unsigned flags); /* 0 or -1 (see paging_map_page) */
int aspace_translate(const AddressSpace *as, unsigned long long va, unsigned long long *phys); /* 0 or -1 */
int aspace_user_ok(const AddressSpace *as, unsigned long long va, unsigned long long len, int write);
void aspace_switch(AddressSpace *as); /* NULL = the kernel's own tables */
void aspace_destroy(AddressSpace *as);

/* Boot self test: builds a space, checks the CPU really translates it as intended, destroys it and checks that no
 * page leaked. 0 = fine, otherwise a negative number that says which step failed. */
int aspace_selftest(void);

#endif
