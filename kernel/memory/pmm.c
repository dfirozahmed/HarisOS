/* HarisOS physical memory manager: one bit per 4 KiB page (1 = used, 0 = free).
 *
 * Under UEFI physical memory is identity mapped, so a physical address can be
 * used directly as a pointer. Memory below 1 MiB is never handed out.
 *
 * Writes go through volatile pointers on purpose: without a C library, the
 * compiler could turn a plain fill loop into a call to memset(), which does
 * not exist in this freestanding build. */
#include "pmm.h"

#define LOW_LIMIT 0x100000ULL /* keep the first 1 MiB reserved */

static volatile unsigned long long *bitmap;
static unsigned long long page_count;   /* pages covered by the bitmap */
static unsigned long long usable_pages; /* pages that were ever free */
static unsigned long long free_count;
static unsigned long long search_hint; /* word index to start searching at */
static const BootMemDesc *desc_at(const BootInfo *info, unsigned long long i)
{
    return (const BootMemDesc *)((const char *)info->memmap + i * info->desc_size);
}

static int page_used(unsigned long long p)
{
    return (int)((bitmap[p / 64] >> (p % 64)) & 1ULL);
}

static void mark_used(unsigned long long p)
{
    if (p >= page_count || page_used(p))
        return;
    bitmap[p / 64] |= 1ULL << (p % 64);
    free_count--;
}

static void mark_free(unsigned long long p)
{
    if (p >= page_count || !page_used(p))
        return;
    bitmap[p / 64] &= ~(1ULL << (p % 64));
    free_count++;
}

/* Owner tags: 4 bits per page (two pages per byte), stored right after the bitmap. tag_pages[] counts the
 * pages each tag owns now, so the breakdown does not need to scan anything. */
static volatile unsigned char *tagmap;
static unsigned long long tag_pages[PMM_TAG_COUNT];

static PmmTag get_tag(unsigned long long p)
{
    unsigned t = (tagmap[p / 2] >> ((p % 2) * 4)) & 15u;
    return t < PMM_TAG_COUNT ? (PmmTag)t : PMM_TAG_OTHER;
}

static void set_tag(unsigned long long p, PmmTag tag)
{
    unsigned sh = (unsigned)(p % 2) * 4;
    tagmap[p / 2] = (unsigned char)((tagmap[p / 2] & ~(15u << sh)) | ((unsigned)tag << sh));
}

/* a page just taken from the free pool */
static void take_page(unsigned long long p, PmmTag tag)
{
    mark_used(p);
    set_tag(p, tag);
    tag_pages[tag]++;
}

int pmm_init(const BootInfo *info)
{
    unsigned long long n, i, max_end = 0;

    if (!info || !info->memmap || info->desc_size < sizeof(BootMemDesc))
        return -1;
    n = info->memmap_size / info->desc_size;

    /* 1) Highest usable address decides how many pages the bitmap covers. */
    for (i = 0; i < n; i++)
    {
        const BootMemDesc *d = desc_at(info, i);
        if (d->type != BOOT_MEM_CONVENTIONAL)
            continue;
        unsigned long long end = d->phys_start + d->pages * PMM_PAGE_SIZE;
        if (end > max_end)
            max_end = end;
    }
    if (max_end <= LOW_LIMIT)
        return -2;

    page_count = max_end / PMM_PAGE_SIZE;
    unsigned long long words = (page_count + 63) / 64;
    unsigned long long bitmap_pages = (words * 8 + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    unsigned long long tag_bytes = (page_count + 1) / 2;
    unsigned long long table_pages = bitmap_pages + (tag_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    /* 2) Find a free region (above 1 MiB) big enough to hold the bitmap and the tag table. */
    unsigned long long bitmap_phys = 0;
    for (i = 0; i < n; i++)
    {
        const BootMemDesc *d = desc_at(info, i);
        unsigned long long start = d->phys_start;
        if (d->type != BOOT_MEM_CONVENTIONAL)
            continue;
        if (start < LOW_LIMIT)
        {
            unsigned long long skip = (LOW_LIMIT - start) / PMM_PAGE_SIZE;
            if (skip >= d->pages)
                continue;
            if (d->pages - skip < table_pages)
                continue;
            start = LOW_LIMIT;
        }
        else if (d->pages < table_pages)
            continue;
        bitmap_phys = start;
        break;
    }
    if (!bitmap_phys)
        return -3;

    /* 3) Start with every page "used", then free what the map says is free. */
    bitmap = (volatile unsigned long long *)(unsigned long long)bitmap_phys;
    for (i = 0; i < words; i++)
        bitmap[i] = ~0ULL;
    tagmap = (volatile unsigned char *)(unsigned long long)(bitmap_phys + bitmap_pages * PMM_PAGE_SIZE);
    for (i = 0; i < tag_bytes; i++)
        tagmap[i] = 0;
    for (i = 0; i < PMM_TAG_COUNT; i++)
        tag_pages[i] = 0;
    free_count = 0;
    usable_pages = 0;

    for (i = 0; i < n; i++)
    {
        const BootMemDesc *d = desc_at(info, i);
        if (d->type != BOOT_MEM_CONVENTIONAL)
            continue;
        for (unsigned long long k = 0; k < d->pages; k++)
        {
            unsigned long long addr = d->phys_start + k * PMM_PAGE_SIZE;
            if (addr < LOW_LIMIT)
                continue;
            mark_free(addr / PMM_PAGE_SIZE);
            usable_pages++;
        }
    }

    /* 4) The bitmap's and tag table's own pages are in use. */
    for (i = 0; i < table_pages; i++)
        take_page(bitmap_phys / PMM_PAGE_SIZE + i, PMM_TAG_SYSTEM);

    search_hint = 0;
    return 0;
}

unsigned long long pmm_alloc_page(void)
{
    return pmm_alloc_page_tag(PMM_TAG_OTHER);
}

unsigned long long pmm_alloc_page_tag(PmmTag tag)
{
    unsigned long long words = (page_count + 63) / 64;
    if (!bitmap)
        return 0;
    for (unsigned long long n = 0; n < words; n++)
    {
        unsigned long long w = (search_hint + n) % words;
        unsigned long long v = bitmap[w];
        if (v == ~0ULL)
            continue;
        unsigned long long bit = (unsigned long long)__builtin_ctzll(~v);
        unsigned long long page = w * 64 + bit;
        if (page >= page_count)
            continue;
        take_page(page, tag);
        search_hint = w;
        return page * PMM_PAGE_SIZE;
    }
    return 0;
}

void pmm_free_page(unsigned long long phys)
{
    if (!bitmap || (phys % PMM_PAGE_SIZE) || phys < LOW_LIMIT)
        return;
    unsigned long long p = phys / PMM_PAGE_SIZE;
    if (p < page_count && page_used(p))
    {
        PmmTag t = get_tag(p);
        if (tag_pages[t])
            tag_pages[t]--;
        set_tag(p, PMM_TAG_OTHER);
    }
    mark_free(p);
    search_hint = (phys / PMM_PAGE_SIZE) / 64;
}

unsigned long long pmm_alloc_contiguous(unsigned long long count)
{
    return pmm_alloc_contiguous_tag(count, PMM_TAG_OTHER);
}

unsigned long long pmm_alloc_contiguous_tag(unsigned long long count, PmmTag tag)
{
    if (!bitmap || count == 0)
        return 0;
    unsigned long long run = 0, start = 0;
    for (unsigned long long p = 0; p < page_count;)
    {
        if ((p % 64) == 0 && bitmap[p / 64] == ~0ULL)
        {
            run = 0; /* a whole word of used pages: skip it quickly */
            p += 64;
            continue;
        }
        if (page_used(p))
        {
            run = 0;
            p++;
            continue;
        }
        if (run == 0)
            start = p;
        run++;
        p++;
        if (run == count)
        {
            for (unsigned long long k = 0; k < count; k++)
                take_page(start + k, tag);
            return start * PMM_PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free_contiguous(unsigned long long phys, unsigned long long count)
{
    for (unsigned long long k = 0; k < count; k++)
        pmm_free_page(phys + k * PMM_PAGE_SIZE);
}

unsigned long long pmm_total_pages(void) { return usable_pages; }
unsigned long long pmm_free_pages(void) { return free_count; }
unsigned long long pmm_tag_pages(PmmTag tag) { return tag < PMM_TAG_COUNT ? tag_pages[tag] : 0; }
