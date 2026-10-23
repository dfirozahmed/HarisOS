/* HarisOS page table access on the real CPU: see include/paging.h. */
#include "paging.h"
#include "pmm.h"

#if defined(__x86_64__)

typedef unsigned long long u64;

#define ADDR_MASK 0x000FFFFFFFFFF000ULL
#define CR0_WP (1ULL << 16)
#define CR4_PGE (1ULL << 7)
#define CR4_LA57 (1ULL << 12)

static int OWN;        /* the tables in CR3 are ours (writable) */
static unsigned NTABLES;

/* New tables come from the page allocator; the firmware maps all RAM 1:1, so a page's address is a pointer. Written
 * through volatile: no libc, a plain loop could become a memset call. */
static u64 *alloc_table(void)
{
    u64 p = pmm_alloc_page_tag(PMM_TAG_SYSTEM);
    if (!p)
        return 0;
    volatile u64 *t = (volatile u64 *)p;
    for (int i = 0; i < 512; i++)
        t[i] = 0;
    NTABLES++;
    return (u64 *)p;
}

static u64 read_cr(int n)
{
    u64 v;
    if (n == 0)
        __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    else if (n == 3)
        __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    else
        __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

int paging_init(void)
{
    if (OWN)
        return 0;
    u64 cr0 = read_cr(0), cr3 = read_cr(3), cr4 = read_cr(4);
    if (cr4 & CR4_LA57)
        return -1; /* 5-level paging: the walk is 4-level */

    /* The pages we write the new tables into may be mapped read-only by the firmware, so write protection is off
     * while the copy is made (CR0.WP = 0 lets the kernel write read-only pages). Interrupts are off at boot. */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 & ~CR0_WP) : "memory");
    u64 *copy = paging_clone((const u64 *)(cr3 & ADDR_MASK), alloc_table);
    if (copy)
    {
        __asm__ volatile("mov %0, %%cr3" : : "r"((u64)copy | (cr3 & ~ADDR_MASK)) : "memory");
        if (cr4 & CR4_PGE) /* global pages survive a CR3 load, and may still be cached as read-only: toggle PGE */
        {
            __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 & ~CR4_PGE) : "memory");
            __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
        }
        OWN = 1;
    }
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    return copy ? 0 : -1;
}

unsigned paging_tables(void) { return NTABLES; }

int paging_set_present(u64 virt, int present)
{
    if (!OWN)
        return -1; /* never write the firmware's tables: they may be read-only */
    int r = paging_set_present_in((u64 *)(read_cr(3) & ADDR_MASK), virt & ~4095ULL, present, alloc_table);
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory"); /* this CPU may have the old translation cached */
    return r;
}

#else

int paging_init(void) { return -1; }
unsigned paging_tables(void) { return 0; }

int paging_set_present(unsigned long long virt, int present)
{
    (void)virt;
    (void)present;
    return -1;
}

#endif
