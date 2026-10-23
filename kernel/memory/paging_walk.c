/* HarisOS page table walk: see include/paging.h. No hardware access, so it is tested on the host. */
#include "paging.h"

typedef unsigned long long u64;

#define PTE_P (1ULL << 0)
#define PTE_RW (1ULL << 1)
#define PTE_US (1ULL << 2)
#define PTE_PS (1ULL << 7) /* in a PDPT / PD entry: a 1 GiB / 2 MiB page */
#define PTE_PAT4K (1ULL << 7)
#define PTE_PATBIG (1ULL << 12) /* the PAT bit of a 1 GiB / 2 MiB page */
#define PTE_NX (1ULL << 63)
#define ADDR_MASK 0x000FFFFFFFFFF000ULL

u64 *paging_pte(u64 *pml4, u64 virt, PagingAllocFn alloc)
{
    u64 e4 = pml4[(virt >> 39) & 511];
    if (!(e4 & PTE_P))
        return 0;
    u64 *pdpt = (u64 *)(e4 & ADDR_MASK);
    volatile u64 *e3 = &pdpt[(virt >> 30) & 511]; /* volatile: the tables are live, the CPU reads them too */
    if (!(*e3 & PTE_P))
        return 0;
    if (*e3 & PTE_PS) /* 1 GiB page -> 512 pages of 2 MiB with the same attributes */
    {
        u64 *pd = alloc();
        if (!pd)
            return 0;
        u64 base = *e3 & 0x000FFFFFC0000000ULL, attr = *e3 & (0x1FFFULL | PTE_NX);
        for (int i = 0; i < 512; i++)
            pd[i] = (base + ((u64)i << 21)) | attr | PTE_PS;
        *e3 = (u64)pd | PTE_P | PTE_RW | (*e3 & PTE_US);
    }
    u64 *pd = (u64 *)(*e3 & ADDR_MASK);
    volatile u64 *e2 = &pd[(virt >> 21) & 511];
    if (!(*e2 & PTE_P))
        return 0;
    if (*e2 & PTE_PS) /* 2 MiB page -> 512 pages of 4 KiB */
    {
        u64 *pt = alloc();
        if (!pt)
            return 0;
        u64 base = *e2 & 0x000FFFFFFFE00000ULL;
        /* P RW US PWT PCD A D (bits 0-6) and G (8) stay; the PAT bit moves from bit 12 to bit 7 */
        u64 attr = (*e2 & 0x17FULL) | (*e2 & 0x100ULL) | (*e2 & PTE_NX) | ((*e2 & PTE_PATBIG) ? PTE_PAT4K : 0);
        for (int i = 0; i < 512; i++)
            pt[i] = (base + ((u64)i << 12)) | attr;
        *e2 = (u64)pt | PTE_P | PTE_RW | (*e2 & PTE_US);
    }
    u64 *pt = (u64 *)(*e2 & ADDR_MASK);
    return &pt[(virt >> 12) & 511];
}

int paging_set_present_in(u64 *pml4, u64 virt, int present, PagingAllocFn alloc)
{
    volatile u64 *pte = paging_pte(pml4, virt, alloc);
    if (!pte)
        return -1;
    if (present)
        *pte |= PTE_P;
    else
        *pte &= ~PTE_P;
    return 0;
}

/* If the 512 entries of a page table map consecutive 4 KiB pages starting on a 2 MiB boundary, all present and all with
 * the same attributes, the same mapping is one 2 MiB page: returns that entry (writable), else 0. Firmware often
 * maps RAM with 4 KiB pages for no reason but a few of its own regions; merging saves a table per 2 MiB and makes
 * the TLB cover more. Accessed / Dirty are not attributes (they are set by the CPU): the merged entry has them if any
 * page had them. The directory entry above (de) still counts: its User bit must be set too, its No-Execute bit
 * applies to every page below it. */
static u64 coalesce(const u64 *pt, u64 de)
{
    const u64 same = PTE_NX | 0x19FULL; /* P RW US PWT PCD, PAT (bit 7), Global (bit 8), NX */
    u64 first = pt[0], ad = 0;
    u64 base = first & ADDR_MASK;
    if (!(first & PTE_P) || (base & 0x1FFFFFULL))
        return 0;
    for (int m = 0; m < 512; m++)
    {
        u64 e = pt[m];
        if (!(e & PTE_P) || (e & ADDR_MASK) != base + ((u64)m << 12) || (e & same) != (first & same))
            return 0;
        ad |= e & 0x60ULL;
    }
    u64 flags = first & (same & ~PTE_PAT4K & ~PTE_NX); /* P RW US PWT PCD G */
    if (!(de & PTE_US))
        flags &= ~PTE_US;
    return base | flags | ad | PTE_PS | PTE_RW | ((first | de) & PTE_NX) | ((first & PTE_PAT4K) ? PTE_PATBIG : 0);
}

/* A copy of every mapping below old_pml4, in new tables, with the Writable bit set on every entry. Same addresses,
 * same memory types (PWT / PCD / PAT), same NX: only the write protection is dropped. The copy is made table by table
 * (the old and the new tree share nothing), so changing it later never touches the firmware's tables. */
u64 *paging_clone(const u64 *old_pml4, PagingAllocFn alloc)
{
    u64 *pml4 = alloc();
    if (!pml4)
        return 0;
    for (int i = 0; i < 512; i++)
    {
        u64 e4 = old_pml4[i];
        if (!(e4 & PTE_P))
            continue;
        u64 *pdpt = alloc();
        if (!pdpt)
            return 0; /* what was built so far is lost (no way to give it back here): the caller keeps the old tables */
        const u64 *old_pdpt = (const u64 *)(e4 & ADDR_MASK);
        for (int j = 0; j < 512; j++)
        {
            u64 e3 = old_pdpt[j];
            if (!(e3 & PTE_P))
                continue;
            if (e3 & PTE_PS) /* a 1 GiB page: copied as it is */
            {
                pdpt[j] = e3 | PTE_RW;
                continue;
            }
            u64 *pd = alloc();
            if (!pd)
                return 0;
            const u64 *old_pd = (const u64 *)(e3 & ADDR_MASK);
            for (int k = 0; k < 512; k++)
            {
                u64 e2 = old_pd[k];
                if (!(e2 & PTE_P))
                    continue;
                if (e2 & PTE_PS) /* a 2 MiB page */
                {
                    pd[k] = e2 | PTE_RW;
                    continue;
                }
                const u64 *old_pt = (const u64 *)(e2 & ADDR_MASK);
                u64 big = coalesce(old_pt, e2);
                if (big) /* 512 equal, consecutive pages: one 2 MiB page does the same with no table */
                {
                    pd[k] = big;
                    continue;
                }
                u64 *pt = alloc();
                if (!pt)
                    return 0;
                for (int m = 0; m < 512; m++)
                    pt[m] = (old_pt[m] & PTE_P) ? (old_pt[m] | PTE_RW) : 0;
                pd[k] = (u64)pt | (e2 & ~ADDR_MASK) | PTE_RW;
            }
            pdpt[j] = (u64)pd | (e3 & ~ADDR_MASK) | PTE_RW;
        }
        pml4[i] = (u64)pdpt | (e4 & ~ADDR_MASK) | PTE_RW;
    }
    return pml4;
}
