/* HarisOS frame buffer caching: see include/fbwc.h. */
#include "fbwc.h"

#if defined(__x86_64__)

#include "pmm.h"

typedef unsigned long long u64;

#define MSR_MTRRCAP 0x0FE
#define MSR_MTRR_BASE0 0x200
#define MSR_MTRR_DEF 0x2FF
#define MSR_PAT 0x277

#define MT_UC 0
#define MT_WC 1
#define MT_WT 4
#define MT_WP 5
#define MT_WB 6

#define PTE_P (1ULL << 0)
#define PTE_RW (1ULL << 1)
#define PTE_US (1ULL << 2)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_PS (1ULL << 7) /* in a PDPT / PD entry: a 1 GiB / 2 MiB page */
#define PTE_PAT4K (1ULL << 7)
#define PTE_PATBIG (1ULL << 12) /* the PAT bit of a 1 GiB / 2 MiB page */
#define PTE_NX (1ULL << 63)
#define ADDR_MASK 0x000FFFFFFFFFF000ULL

static int STATUS = FBWC_NOT_RUN;
static int MTRR_TYPE = -1;

static u64 rdmsr(unsigned msr)
{
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void wrmsr(unsigned msr, u64 v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((unsigned)v), "d"((unsigned)(v >> 32)));
}

static void cpuid(unsigned leaf, unsigned r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

/* Memory type the MTRRs give the page at phys (SDM vol. 3, "MTRR precedence"): UC beats everything,
 * WT beats WB, otherwise the ranges agree. Fixed ranges only cover the first megabyte: not needed here.
 * *ranged is 1 when a variable range matched, 0 when the answer is just the default type. */
static int mtrr_type_at(u64 phys, int *ranged)
{
    u64 def = rdmsr(MSR_MTRR_DEF);
    *ranged = 0;
    if (!(def & (1ULL << 11)))
        return MT_UC; /* MTRRs disabled: everything is uncached */
    int vcnt = (int)(rdmsr(MSR_MTRRCAP) & 0xFF), type = -1;
    for (int i = 0; i < vcnt && i < 16; i++)
    {
        u64 base = rdmsr(MSR_MTRR_BASE0 + 2 * i), mask = rdmsr(MSR_MTRR_BASE0 + 2 * i + 1);
        if (!(mask & (1ULL << 11)))
            continue; /* range not valid */
        if (((phys ^ base) & mask & ADDR_MASK) != 0)
            continue;
        *ranged = 1;
        int t = (int)(base & 0xFF);
        if (t == MT_UC)
            return MT_UC;
        if (type < 0 || (t == MT_WT && type == MT_WB))
            type = t;
    }
    return type >= 0 ? type : (int)(def & 0xFF);
}

static void flush_tlb_all(void)
{
    u64 cr3, cr4;
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 7)) /* global pages survive a CR3 load: toggle PGE */
    {
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 & ~(1ULL << 7)) : "memory");
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }
}

/* Add a variable MTRR that makes [first, last) write-combining, for a frame buffer that no range covers (so the
 * default type, uncached, applies). A range is a power of two in size and aligned to it, so it may be bigger than
 * the frame buffer: it must not touch any other valid range except uncached ones (UC wins there anyway; other
 * overlaps are undefined by the CPU). Follows the SDM's order for changing MTRRs: no interrupts, cache disabled,
 * flush, MTRRs off, write, flush, MTRRs on. */
static int mtrr_add_wc(u64 first, u64 last)
{
    unsigned r[4];
    cpuid(0x80000000, r);
    int bits = 36;
    if (r[0] >= 0x80000008)
    {
        cpuid(0x80000008, r);
        bits = (int)(r[0] & 0xFF);
    }
    if (bits < 32 || bits > 52)
        return -1;
    u64 phys_mask = ((1ULL << bits) - 1) & ADDR_MASK;

    u64 size = 4096;
    u64 base;
    for (;; size <<= 1)
    {
        base = first & ~(size - 1);
        if (base + size >= last)
            break;
        if (size >= (1ULL << 40))
            return -1;
    }

    int vcnt = (int)(rdmsr(MSR_MTRRCAP) & 0xFF), slot = -1;
    if (vcnt > 16)
        vcnt = 16;
    for (int i = 0; i < vcnt; i++)
    {
        u64 b = rdmsr(MSR_MTRR_BASE0 + 2 * i), m = rdmsr(MSR_MTRR_BASE0 + 2 * i + 1);
        if (!(m & (1ULL << 11)))
        {
            if (slot < 0)
                slot = i;
            continue;
        }
        u64 mm = m & ADDR_MASK;
        u64 lo = b & mm, span = ~mm & phys_mask; /* the range is lo .. lo+span when its mask is contiguous */
        if ((b & 0xFF) != MT_UC && lo <= base + size - 1 && base <= lo + span)
            return -2; /* overlaps a cached range */
    }
    if (slot < 0)
        return -3; /* no free variable MTRR */

    u64 flags, cr0, def = rdmsr(MSR_MTRR_DEF);
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"((cr0 | (1ULL << 30)) & ~(1ULL << 29)) : "memory"); /* CD=1, NW=0 */
    __asm__ volatile("wbinvd" : : : "memory");
    flush_tlb_all();
    wrmsr(MSR_MTRR_DEF, def & ~(1ULL << 11)); /* MTRRs off */
    wrmsr(MSR_MTRR_BASE0 + 2 * slot, base | MT_WC);
    wrmsr(MSR_MTRR_BASE0 + 2 * slot + 1, (~(size - 1) & phys_mask) | (1ULL << 11));
    __asm__ volatile("wbinvd" : : : "memory");
    flush_tlb_all();
    wrmsr(MSR_MTRR_DEF, def); /* MTRRs on again */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
    return 0;
}

static u64 *new_table(void)
{
    u64 p = pmm_alloc_page_tag(PMM_TAG_SYSTEM);
    if (!p)
        return 0;
    u64 *t = (u64 *)p; /* the firmware maps all RAM 1:1 */
    for (int i = 0; i < 512; i++)
        t[i] = 0;
    return t;
}

/* The page-table entry (4 KiB level) that maps virt (= phys), after splitting any 1 GiB / 2 MiB page on the way. */
static u64 *pte_for(u64 virt)
{
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    u64 *pml4 = (u64 *)(cr3 & ADDR_MASK);
    u64 e4 = pml4[(virt >> 39) & 511];
    if (!(e4 & PTE_P))
        return 0;
    u64 *pdpt = (u64 *)(e4 & ADDR_MASK);
    u64 *e3 = &pdpt[(virt >> 30) & 511];
    if (!(*e3 & PTE_P))
        return 0;
    if (*e3 & PTE_PS) /* 1 GiB page -> 512 pages of 2 MiB with the same attributes */
    {
        u64 *pd = new_table();
        if (!pd)
            return 0;
        u64 base = *e3 & 0x000FFFFFC0000000ULL, attr = *e3 & (0x1FFFULL | PTE_NX);
        for (int i = 0; i < 512; i++)
            pd[i] = (base + ((u64)i << 21)) | attr | PTE_PS;
        *e3 = (u64)pd | PTE_P | PTE_RW | (*e3 & PTE_US);
    }
    u64 *pd = (u64 *)(*e3 & ADDR_MASK);
    u64 *e2 = &pd[(virt >> 21) & 511];
    if (!(*e2 & PTE_P))
        return 0;
    if (*e2 & PTE_PS) /* 2 MiB page -> 512 pages of 4 KiB */
    {
        u64 *pt = new_table();
        if (!pt)
            return 0;
        u64 base = *e2 & 0x000FFFFFFFE00000ULL;
        u64 attr = (*e2 & 0x17FULL) | (*e2 & 0x100ULL) | (*e2 & PTE_NX) | ((*e2 & PTE_PATBIG) ? PTE_PAT4K : 0);
        for (int i = 0; i < 512; i++)
            pt[i] = (base + ((u64)i << 12)) | attr;
        *e2 = (u64)pt | PTE_P | PTE_RW | (*e2 & PTE_US);
    }
    u64 *pt = (u64 *)(*e2 & ADDR_MASK);
    return &pt[(virt >> 12) & 511];
}

static void flush_all(u64 first, u64 last)
{
    u64 cr3, cr4;
    __asm__ volatile("wbinvd" : : : "memory");
    for (u64 a = first; a < last; a += 4096)
        __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory"); /* drops all non-global translations */
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 7)) /* global pages survive a CR3 load: toggle PGE */
    {
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 & ~(1ULL << 7)) : "memory");
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }
}

int fbwc_enable(u64 phys, u64 bytes)
{
    if (!phys || !bytes)
        return STATUS = FBWC_NO_MEMORY;
    u64 first = phys & ~4095ULL, last = (phys + bytes + 4095) & ~4095ULL;

    unsigned r[4];
    cpuid(1, r);
    if (!(r[3] & (1U << 16)))
        return STATUS = FBWC_NO_PAT;

    if (r[3] & (1U << 12)) /* MTRRs present: what do they say about the first and last page? */
    {
        int ra, rb;
        int a = mtrr_type_at(first, &ra), b = mtrr_type_at(last - 4096, &rb);
        MTRR_TYPE = a == b ? a : MT_UC; /* a mixed range: treat as the more careful answer */
        if (MTRR_TYPE == MT_WC)
            return STATUS = FBWC_ALREADY;
        if (MTRR_TYPE == MT_UC)
        {
            /* An uncached range always wins over write-combining, in the MTRRs and in the page tables. If the
             * frame buffer is only uncached because no range covers it (default type), give it a range. */
            if (ra || rb || a != b)
                return STATUS = FBWC_MTRR_UC;
            return STATUS = (mtrr_add_wc(first, last) == 0) ? FBWC_ADDED : FBWC_MTRR_FAIL;
        }
    }

    u64 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 12))
        return STATUS = FBWC_NO_PAGING; /* 5-level paging: the walk below is 4-level */

    /* Which PAT entry means write-combining? Use an existing one, else turn entry 4 (page-table bits PAT=1,
     * PCD=0, PWT=0, unused by the firmware) into one. */
    u64 pat = rdmsr(MSR_PAT);
    int k = -1;
    for (int i = 0; i < 8 && k < 0; i++)
        if (((pat >> (8 * i)) & 0xFF) == MT_WC)
            k = i;
    if (k < 0)
    {
        k = 4;
        pat = (pat & ~(0xFFULL << 32)) | ((u64)MT_WC << 32);
    }

    /* Split the big pages first (this may allocate): change nothing yet if a page cannot be reached. */
    for (u64 a = first; a < last; a += 4096)
        if (!pte_for(a))
            return STATUS = FBWC_NO_MEMORY;

    __asm__ volatile("wbinvd" : : : "memory");
    wrmsr(MSR_PAT, pat);
    for (u64 a = first; a < last; a += 4096)
    {
        u64 *pte = pte_for(a);
        u64 v = *pte & ~(PTE_PWT | PTE_PCD | PTE_PAT4K);
        if (k & 1)
            v |= PTE_PWT;
        if (k & 2)
            v |= PTE_PCD;
        if (k & 4)
            v |= PTE_PAT4K;
        *pte = v;
    }
    flush_all(first, last);
    return STATUS = FBWC_APPLIED;
}

#else

int fbwc_enable(unsigned long long phys, unsigned long long bytes)
{
    (void)phys;
    (void)bytes;
    return -1;
}

#endif

int fbwc_status(void) { return STATUS; }
int fbwc_mtrr_type(void) { return MTRR_TYPE; }
