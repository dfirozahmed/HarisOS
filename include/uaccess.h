/* ============================================================
 * HarisOS user memory access (SMAP)
 *
 *   user_access_begin();    the kernel may now read and write user pages
 *   ... copy from / to a pointer that was checked with paging_user_range_ok ...
 *   user_access_end();      user pages are off limits again
 *
 * With SMAP on (CR4.SMAP, see protect.h) the CPU refuses every ring 0 access to a page that ring 3 may use, unless the
 * AC flag is set. That turns "the kernel followed a user pointer by mistake" (a classic hole: a forgotten check, a pointer
 * that was swapped in for a kernel one) from a silent security bug into an immediate page fault. The kernel
 * opens the window deliberately, with STAC, for exactly the instructions that touch user memory, and closes it with CLAC.
 *
 * The window is short and never contains a call that can block, sleep or switch threads: AC is a flag of the running
 * thread, and interrupts and exceptions clear it themselves on entry.
 *
 * Accessing a user page through its physical address (the kernel's identity map of RAM, which is supervisor memory)
 * needs no window: SMAP looks at the page the access goes through, and that one is not a user page.
 * ============================================================ */
#ifndef HARIS_UACCESS_H
#define HARIS_UACCESS_H

#if defined(HARIS_HOST)

/* Host tests: a depth counter the test can look at (volatile int host_uaccess). */
extern volatile int host_uaccess;
static inline void user_access_begin(void) { host_uaccess++; }
static inline void user_access_end(void) { host_uaccess--; }

#else

extern int smap_enabled; /* protect.c: 1 once SMAP is on; STAC / CLAC are undefined instructions on a CPU without it */

static inline void user_access_begin(void)
{
    if (smap_enabled)
        __asm__ volatile("stac" : : : "memory", "cc");
}

static inline void user_access_end(void)
{
    if (smap_enabled)
        __asm__ volatile("clac" : : : "memory", "cc");
}

#endif

#endif
