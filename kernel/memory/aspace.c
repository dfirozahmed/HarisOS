/* HarisOS address spaces: see include/aspace.h. */
#include "aspace.h"
#include "irq.h"
#include "pmm.h"
#include "preempt.h"

#if defined(__x86_64__)

typedef unsigned long long u64;

#define ADDR_MASK 0x000FFFFFFFFFF000ULL
#define MSR_EFER 0xC0000080
#define EFER_NXE (1ULL << 11)

static AddressSpace *KERNEL; /* the tables in CR3 when aspace_init ran */
static int READY;

/* The page allocator has no lock of its own: no thread switch while it is in use. Written through volatile, no memset
 * in a freestanding build. */
static u64 *alloc_page(void)
{
    preempt_disable();
    u64 p = pmm_alloc_page_tag(PMM_TAG_SYSTEM);
    preempt_enable();
    if (!p)
        return 0;
    volatile u64 *t = (volatile u64 *)p;
    for (int i = 0; i < 512; i++)
        t[i] = 0;
    return (u64 *)p;
}

static void free_page(u64 phys)
{
    preempt_disable();
    pmm_free_page(phys);
    preempt_enable();
}

static u64 read_msr(unsigned msr)
{
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

int aspace_init(void)
{
    if (READY)
        return 0;
    if (!(read_msr(MSR_EFER) & EFER_NXE))
        return -1; /* user pages carry the No-Execute bit; with NXE off it is a reserved bit and every access faults */
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    AddressSpace *k = (AddressSpace *)(cr3 & ADDR_MASK);
    for (int i = PAGING_USER_SLOT_FIRST; i <= PAGING_USER_SLOT_LAST; i++)
        if (k->entry[i] & 1)
            return -1; /* the kernel's identity map reaches into the user part (a machine with a huge physical space) */
    KERNEL = k;
    READY = 1;
    return 0;
}

AddressSpace *aspace_create(void)
{
    if (!READY)
        return 0;
    AddressSpace *as = (AddressSpace *)alloc_page();
    if (!as)
        return 0;
    for (int i = 0; i < PAGING_USER_SLOT_FIRST; i++)
        as->entry[i] = KERNEL->entry[i]; /* the same lower tables: the kernel looks the same from every space */
    return as;
}

int aspace_map_new(AddressSpace *as, u64 va, unsigned flags)
{
    u64 *page = alloc_page();
    if (!page)
        return -1;
    if (paging_map_page(as->entry, va, (u64)page, flags | PAGING_OWNED, alloc_page) != 0)
    {
        free_page((u64)page);
        return -1;
    }
    return 0;
}

int aspace_translate(const AddressSpace *as, u64 va, u64 *phys) { return paging_translate(as->entry, va, phys, 0); }

int aspace_user_ok(const AddressSpace *as, u64 va, u64 len, int write)
{
    return paging_user_range_ok(as->entry, va, len, write);
}

void aspace_switch(AddressSpace *as)
{
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    u64 root = (u64)(as ? as : KERNEL);
    __asm__ volatile("mov %0, %%cr3" : : "r"(root | (cr3 & ~ADDR_MASK)) : "memory"); /* keeps the cache bits of CR3 */
}

void aspace_destroy(AddressSpace *as)
{
    if (!as)
        return;
    paging_destroy_range(as->entry, PAGING_USER_SLOT_FIRST, PAGING_USER_SLOT_LAST, free_page);
    free_page((u64)as); /* the top-level table; the kernel slots in it point at tables that stay */
}

int aspace_selftest(void)
{
    if (!READY)
        return -1;
    u64 free0 = pmm_free_pages();
    AddressSpace *as = aspace_create();
    if (!as)
        return -2;

    const u64 va = PAGING_USER_BASE + 0x10000;
    if (aspace_map_new(as, va, ASPACE_WRITE) != 0)
        return -3;
    u64 phys;
    if (aspace_translate(as, va, &phys) != 0 || (phys & 0xFFF))
        return -4;
    *(volatile u64 *)phys = 0x1122334455667788ULL; /* through the identity map: no space switch needed */

    if (!aspace_user_ok(as, va, 8, 1) || !aspace_user_ok(as, va + 8, 4088, 0))
        return -5;
    if (aspace_user_ok(as, va, 4097, 0))
        return -6; /* the second page is not mapped */
    if (aspace_user_ok(as, 0x1000, 8, 0))
        return -7; /* a kernel address is not user memory */

    /* the kernel half is shared: our own code is at the same address in the new space */
    u64 code = (u64)aspace_selftest, seen_code = 0;
    if (paging_translate(as->entry, code, &seen_code, 0) != 0 || seen_code != code)
        return -8;

    /* the real test: let the CPU walk the new tables. Switch, read the user page by its user address and a kernel
     * variable by its kernel address, switch back. */
    static volatile u64 marker = 0xFEEDFACECAFEBEEFULL;
    u64 irq = irq_save();
    aspace_switch(as);
    u64 seen = *(volatile u64 *)va;
    u64 mark = marker;
    aspace_switch(0);
    irq_restore(irq);
    if (seen != 0x1122334455667788ULL)
        return -9;
    if (mark != 0xFEEDFACECAFEBEEFULL)
        return -10;

    aspace_destroy(as);
    if (pmm_free_pages() != free0)
        return -11; /* a page or table was not given back */
    return 0;
}

#else

int aspace_init(void) { return -1; }
AddressSpace *aspace_create(void) { return 0; }
int aspace_selftest(void) { return -1; }

#endif
