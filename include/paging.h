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

/* ---- user address spaces (see aspace.h for the kernel side) ------------------------------------------------------
 *
 * The 4-level tree has 512 top-level slots, each 512 GiB. The kernel identity maps physical memory in the first
 * ones (slot 0 = 0-512 GiB, ...). User memory lives in the upper part of the lower half, slots 128..255 = virtual
 * addresses 0x0000_4000_0000_0000 .. 0x0000_8000_0000_0000 (64 TiB): far above any RAM, and the kernel's slots are
 * left out of it, so every address space can share the kernel's lower slots and own its user slots. */
#define PAGING_USER_SLOT_FIRST 128
#define PAGING_USER_SLOT_LAST 255
#define PAGING_USER_BASE 0x0000400000000000ULL
#define PAGING_USER_END 0x0000800000000000ULL /* exclusive: the end of the canonical lower half */

/* What a user page may do (readable always). The CPU also needs the User bit in every table on the way: that is added. */
#define PAGING_WRITE 1u
#define PAGING_EXEC 2u  /* without it the page has No-Execute: data and stacks cannot run as code */
#define PAGING_OWNED 4u /* paging_destroy_range frees the physical page (a software bit, ignored by the CPU) */
#define PAGING_COW 8u   /* read-only now, but a copy-on-write page: the first write makes a private copy (paging_cow_resolve) */

/* Map the 4 KiB page at virt (a user address, page aligned) to the physical page phys (page aligned), user accessible,
 * building the tables on the way with alloc. 0 = done, -1 = bad address, already mapped, a big page in the way, or
 * alloc failed (tables built so far stay: they are freed with the space). */
int paging_map_page(unsigned long long *pml4, unsigned long long virt, unsigned long long phys, unsigned flags,
                    PagingAllocFn alloc);

/* Where does virt go? 0 and *phys (with the offset), *entry (the leaf entry's bits) if mapped, -1 if not. Understands
 * 1 GiB and 2 MiB pages too (the kernel's identity map). Either out pointer may be NULL. */
int paging_translate(const unsigned long long *pml4, unsigned long long virt, unsigned long long *phys,
                     unsigned long long *entry);

/* May ring 3 access [virt, virt + len) (len > 0) with a read, or a write if `write`? Every page must be mapped, with the
 * User bit (and Writable for a write) in every table down to the page. This is the check a system call makes on a
 * pointer from user code before it reads or writes through it. */
int paging_user_range_ok(const unsigned long long *pml4, unsigned long long virt, unsigned long long len, int write);

/* Give back everything below the top-level slots first..last: for each page marked PAGING_OWNED free_fn(page), and for
 * every table free_fn(table); the slots are cleared. Pages without the mark (shared memory) are left alone. The pml4
 * itself is not freed. */
typedef void (*PagingFreeFn)(unsigned long long phys);
void paging_destroy_range(unsigned long long *pml4, int first, int last, PagingFreeFn free_fn);

/* Take the 4 KiB page at virt out of a user space: its entry is cleared (the tables stay; no TLB flush: the caller does that). Only a page
 * that is not marked PAGING_OWNED (shared memory) can be taken out this way. 0 and *phys, or -1 if nothing is mapped there or the page
 * is owned. */
int paging_unmap_page(unsigned long long *pml4, unsigned long long virt, unsigned long long *phys);

/* The same for fork without copying: every owned page that was writable is now shared by both spaces, read-only and marked
 * PAGING_COW (src is changed too: the original entries lose their write permission, so the caller must flush its TLB); an owned page
 * that was never writable is shared as it is; pages without the mark (shared memory) are mapped again as they are. cow.h counts the
 * holders of every shared page. If the sharing table is full the page is copied, as paging_clone_user does. 0, or -1 if an allocator
 * ran out (dst holds what was done so far; free it with paging_destroy_range: the counts stay right). */
int paging_clone_user_cow(unsigned long long *src, unsigned long long *dst, int first, int last, PagingAllocFn table_alloc,
                          PagingAllocFn page_alloc, PagingFreeFn page_free);

/* A write to the page at virt that was refused: if it is a copy-on-write page it becomes writable. When other spaces still hold it
 * it is copied to a new page first (page_alloc) and this space lets go of the shared one. 1 = done (the caller flushes virt from the
 * TLB), 0 = not a copy-on-write page (not mapped, or an ordinary page: a real fault), -1 = no memory for the copy. */
int paging_cow_resolve(unsigned long long *pml4, unsigned long long virt, PagingAllocFn page_alloc);

/* Copy the user part (top-level slots first..last) of src into dst, whose slots there are empty: every page marked PAGING_OWNED
 * gets a fresh copy of its 4096 bytes (page_alloc gives the new physical page; its old contents do not matter; page_free takes
 * back one that could not be mapped), every other page (shared memory) is mapped again as it is, and the tables are new
 * (table_alloc). The pages keep their permissions. 0, or -1 if an allocator ran out: dst then holds what was done so far and
 * the caller frees it with paging_destroy_range. This is fork. */
int paging_clone_user(const unsigned long long *src, unsigned long long *dst, int first, int last, PagingAllocFn table_alloc,
                      PagingAllocFn page_alloc, PagingFreeFn page_free);

#endif
