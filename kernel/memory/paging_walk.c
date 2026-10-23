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

/* ---- user address spaces ------------------------------------------------------------------------------------- */

#define PTE_OWNED (1ULL << 9) /* software bit (the CPU ignores bits 9-11): this page is ours to free */

int paging_map_page(u64 *pml4, u64 virt, u64 phys, unsigned flags, PagingAllocFn alloc)
{
    if ((virt & 0xFFF) || (phys & 0xFFF) || (phys & ~ADDR_MASK) || virt < PAGING_USER_BASE || virt >= PAGING_USER_END)
        return -1;
    static const int shift[3] = {39, 30, 21};
    u64 *table = pml4;
    for (int level = 0; level < 3; level++)
    {
        volatile u64 *e = &table[(virt >> shift[level]) & 511];
        if (!(*e & PTE_P))
        {
            u64 *next = alloc();
            if (!next)
                return -1;
            /* Permissions are the AND of every level, so the tables on the way are as open as possible and the page
             * itself says what is allowed. */
            *e = (u64)next | PTE_P | PTE_RW | PTE_US;
        }
        else if (level > 0 && (*e & PTE_PS))
            return -1; /* a big page is in the way */
        else if (!(*e & PTE_US))
            return -1; /* a table ring 3 may not use: not one of ours (the kernel's), never changed from here */
        table = (u64 *)(*e & ADDR_MASK);
    }
    volatile u64 *leaf = &table[(virt >> 12) & 511];
    if (*leaf & PTE_P)
        return -1;
    u64 v = phys | PTE_P | PTE_US;
    if (flags & PAGING_WRITE)
        v |= PTE_RW;
    if (!(flags & PAGING_EXEC))
        v |= PTE_NX;
    if (flags & PAGING_OWNED)
        v |= PTE_OWNED;
    *leaf = v;
    return 0;
}

int paging_translate(const u64 *pml4, u64 virt, u64 *phys, u64 *entry)
{
    static const int shift[4] = {39, 30, 21, 12};
    const u64 *table = pml4;
    for (int level = 0; level < 4; level++)
    {
        u64 e = table[(virt >> shift[level]) & 511];
        if (!(e & PTE_P))
            return -1;
        u64 base = 0, off = 0;
        int leaf = 0;
        if (level == 1 && (e & PTE_PS))
        {
            base = e & 0x000FFFFFC0000000ULL;
            off = virt & 0x3FFFFFFFULL;
            leaf = 1;
        }
        else if (level == 2 && (e & PTE_PS))
        {
            base = e & 0x000FFFFFFFE00000ULL;
            off = virt & 0x1FFFFFULL;
            leaf = 1;
        }
        else if (level == 3)
        {
            base = e & ADDR_MASK;
            off = virt & 0xFFFULL;
            leaf = 1;
        }
        if (leaf)
        {
            if (phys)
                *phys = base + off;
            if (entry)
                *entry = e;
            return 0;
        }
        table = (const u64 *)(e & ADDR_MASK);
    }
    return -1;
}

int paging_user_range_ok(const u64 *pml4, u64 virt, u64 len, int write)
{
    if (len == 0 || virt < PAGING_USER_BASE || virt >= PAGING_USER_END || len > PAGING_USER_END - virt)
        return 0; /* also catches virt + len wrapping around */
    static const int shift[4] = {39, 30, 21, 12};
    for (u64 a = virt & ~0xFFFULL; a < virt + len; a += 4096)
    {
        const u64 *table = pml4;
        for (int level = 0; level < 4; level++)
        {
            u64 e = table[(a >> shift[level]) & 511];
            if (!(e & PTE_P) || !(e & PTE_US) || (write && !(e & PTE_RW)))
                return 0;
            if (level == 1 || level == 2)
                if (e & PTE_PS)
                    return 0; /* user memory is made of 4 KiB pages only */
            table = (const u64 *)(e & ADDR_MASK);
        }
    }
    return 1;
}

void paging_destroy_range(u64 *pml4, int first, int last, PagingFreeFn free_fn)
{
    for (int i = first; i <= last; i++)
    {
        if (!(pml4[i] & PTE_P))
            continue;
        u64 *pdpt = (u64 *)(pml4[i] & ADDR_MASK);
        for (int j = 0; j < 512; j++)
        {
            if (!(pdpt[j] & PTE_P) || (pdpt[j] & PTE_PS))
                continue; /* a big page cannot be one of ours */
            u64 *pd = (u64 *)(pdpt[j] & ADDR_MASK);
            for (int k = 0; k < 512; k++)
            {
                if (!(pd[k] & PTE_P) || (pd[k] & PTE_PS))
                    continue;
                u64 *pt = (u64 *)(pd[k] & ADDR_MASK);
                for (int m = 0; m < 512; m++)
                    if ((pt[m] & PTE_P) && (pt[m] & PTE_OWNED))
                        free_fn(pt[m] & ADDR_MASK);
                free_fn((u64)pt);
            }
            free_fn((u64)pd);
        }
        free_fn((u64)pdpt);
        pml4[i] = 0;
    }
}

int paging_clone_user(const u64 *src, u64 *dst, int first, int last, PagingAllocFn table_alloc, PagingAllocFn page_alloc,
                      PagingFreeFn page_free)
{
    for (int i = first; i <= last; i++)
    {
        if (!(src[i] & PTE_P))
            continue;
        const u64 *pdpt = (const u64 *)(src[i] & ADDR_MASK);
        for (int j = 0; j < 512; j++)
        {
            if (!(pdpt[j] & PTE_P) || (pdpt[j] & PTE_PS))
                continue; /* a big page cannot be one of ours */
            const u64 *pd = (const u64 *)(pdpt[j] & ADDR_MASK);
            for (int k = 0; k < 512; k++)
            {
                if (!(pd[k] & PTE_P) || (pd[k] & PTE_PS))
                    continue;
                const u64 *pt = (const u64 *)(pd[k] & ADDR_MASK);
                for (int m = 0; m < 512; m++)
                {
                    u64 e = pt[m];
                    if (!(e & PTE_P))
                        continue;
                    u64 virt = ((u64)i << 39) | ((u64)j << 30) | ((u64)k << 21) | ((u64)m << 12);
                    unsigned flags = ((e & PTE_RW) ? PAGING_WRITE : 0) | ((e & PTE_NX) ? 0 : PAGING_EXEC);
                    u64 phys = e & ADDR_MASK;
                    if (e & PTE_OWNED)
                    {
                        u64 *copy = page_alloc();
                        if (!copy)
                            return -1;
                        const volatile u64 *from = (const volatile u64 *)phys;
                        volatile u64 *to = (volatile u64 *)copy;
                        for (int w = 0; w < 512; w++)
                            to[w] = from[w];
                        phys = (u64)copy;
                        flags |= PAGING_OWNED;
                        if (paging_map_page(dst, virt, phys, flags, table_alloc) != 0)
                        {
                            page_free(phys); /* not in dst yet: nobody else would free it */
                            return -1;
                        }
                    }
                    else if (paging_map_page(dst, virt, phys, flags, table_alloc) != 0)
                        return -1;
                }
            }
        }
    }
    return 0;
}
