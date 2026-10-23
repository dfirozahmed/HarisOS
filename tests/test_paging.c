/* Host test for paging_walk.c: splitting big pages and making single pages not present. The "page tables" are
 * ordinary host memory; their entries hold host addresses, which is all the walk needs. */
#include "paging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the Windows C runtime has no aligned_alloc */
#ifdef _WIN32
#include <malloc.h>
#define ALIGNED_ALLOC(a, s) _aligned_malloc((s), (a))
#else
#define ALIGNED_ALLOC(a, s) aligned_alloc((a), (s))
#endif

typedef unsigned long long u64;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define P (1ULL << 0)
#define RW (1ULL << 1)
#define PWT (1ULL << 3)
#define PCD (1ULL << 4)
#define PS (1ULL << 7)
#define PAT4K (1ULL << 7)
#define PATBIG (1ULL << 12)
#define NX (1ULL << 63)

static int TABLES_MADE, ALLOC_LIMIT = 1000;
static void *MADE[64];

static u64 *alloc_table(void)
{
    if (TABLES_MADE >= ALLOC_LIMIT)
        return 0;
    void *t = ALIGNED_ALLOC(4096, 4096);
    memset(t, 0, 4096);
    MADE[TABLES_MADE++] = t;
    return t;
}

static u64 FREED[64];
static int FREED_N;
static void free_record(u64 phys)
{
    if (FREED_N < 64)
        FREED[FREED_N++] = phys;
}
static int freed(u64 phys)
{
    for (int i = 0; i < FREED_N; i++)
        if (FREED[i] == phys)
            return 1;
    return 0;
}

static u64 *new_table(void)
{
    u64 *t = ALIGNED_ALLOC(4096, 4096);
    memset(t, 0, 4096);
    return t;
}

int main(void)
{
    u64 *pml4 = new_table(), *pdpt = new_table(), *pd = new_table();
    pml4[0] = (u64)pdpt | P | RW;
    pdpt[0] = (u64)pd | P | RW;                                          /* 0-1 GiB: 2 MiB pages */
    pdpt[1] = 0x40000000ULL | P | RW | PS | PCD | NX;                    /* 1-2 GiB: one 1 GiB page, uncached, no-exec */
    pd[0] = 0x000000ULL | P | RW | PS;                                   /* 0-2 MiB: write-back */
    pd[1] = 0x200000ULL | P | RW | PS | PWT | PATBIG;                    /* 2-4 MiB: PAT bit set (a different memory type) */
    /* pd[2] stays empty: 4-6 MiB is not mapped */

    /* a hole in a 2 MiB page: it is split, the neighbours keep their mapping and attributes */
    u64 a = 0x00100000ULL; /* 1 MiB */
    CHECK(paging_set_present_in(pml4, a, 0, alloc_table) == 0);
    CHECK(TABLES_MADE == 1);
    u64 *pt0 = (u64 *)(pd[0] & 0x000FFFFFFFFFF000ULL);
    CHECK(!(pd[0] & PS) && (pd[0] & P));
    CHECK(!(pt0[(a >> 12) & 511] & P));
    CHECK((pt0[((a >> 12) & 511) - 1] & P) && (pt0[((a >> 12) & 511) + 1] & P));
    CHECK((pt0[5] & 0x000FFFFFFFFFF000ULL) == 5 * 4096ULL); /* identity: page 5 maps physical 0x5000 */
    CHECK((pt0[5] & (RW | PS)) == RW);                       /* PS (bit 7) is PAT in a 4 KiB entry: must be clear here */

    /* the same page again: no new table, and it can be mapped back */
    CHECK(paging_set_present_in(pml4, a + 0x123, 0, alloc_table) == 0);
    CHECK(TABLES_MADE == 1);
    CHECK(paging_set_present_in(pml4, a, 1, alloc_table) == 0);
    CHECK(pt0[(a >> 12) & 511] & P);

    /* the PAT bit of a 2 MiB page moves to bit 7 of the 4 KiB entries; PWT stays */
    u64 b = 0x00300000ULL;
    CHECK(paging_set_present_in(pml4, b, 0, alloc_table) == 0);
    u64 *pt1 = (u64 *)(pd[1] & 0x000FFFFFFFFFF000ULL);
    CHECK((pt1[(b >> 12) & 511] & (PWT | PAT4K)) == (PWT | PAT4K));
    CHECK(!(pt1[(b >> 12) & 511] & PATBIG));
    CHECK((pt1[0] & (PWT | PAT4K)) == (PWT | PAT4K));

    /* a 1 GiB page: split twice (to 2 MiB, then 4 KiB); uncached and no-exec carry over */
    u64 c = 0x40000000ULL + 0x00500000ULL;
    int made = TABLES_MADE;
    CHECK(paging_set_present_in(pml4, c, 0, alloc_table) == 0);
    CHECK(TABLES_MADE == made + 2);
    u64 *pd1 = (u64 *)(pdpt[1] & 0x000FFFFFFFFFF000ULL);
    CHECK(!(pdpt[1] & PS));
    u64 e = pd1[(c >> 21) & 511];
    CHECK(!(e & PS));
    u64 *pt2 = (u64 *)(e & 0x000FFFFFFFFFF000ULL);
    CHECK(!(pt2[(c >> 12) & 511] & P));
    CHECK((pt2[0] & (PCD | NX | P | RW)) == (PCD | NX | P | RW));
    CHECK(pd1[0] & PS); /* the other 2 MiB pages of that gigabyte are still big */
    CHECK((pd1[0] & (PCD | NX)) == (PCD | NX));
    CHECK((pd1[3] & 0x000FFFFFFFE00000ULL) == 0x40000000ULL + 3 * 0x200000ULL);

    /* not mapped: reported, and nothing is built */
    made = TABLES_MADE;
    CHECK(paging_set_present_in(pml4, 0x00400000ULL, 0, alloc_table) == -1); /* empty pd[2] */
    CHECK(paging_set_present_in(pml4, 0x80000000ULL, 0, alloc_table) == -1); /* empty pdpt[2] */
    CHECK(paging_set_present_in(pml4, 1ULL << 39, 0, alloc_table) == -1);    /* empty pml4[1] */
    CHECK(TABLES_MADE == made);

    /* running out of memory while splitting leaves the big page as it was */
    u64 d = 0x00000000ULL + 0x1000; /* in pd[0], already split: no allocation needed */
    ALLOC_LIMIT = TABLES_MADE;
    CHECK(paging_set_present_in(pml4, d, 0, alloc_table) == 0);
    u64 *pd2 = new_table();
    pdpt[2] = (u64)pd2 | P | RW;
    pd2[0] = 0x80000000ULL | P | RW | PS;
    CHECK(paging_set_present_in(pml4, 0x80000000ULL + 0x1000, 0, alloc_table) == -1);
    CHECK(pd2[0] & PS); /* untouched */

    /* ---- paging_clone: the same mappings in new tables, all writable ---- */
    u64 *o4 = new_table(), *o3a = new_table(), *o3b = new_table(), *o2 = new_table(), *o1 = new_table();
    o4[0] = (u64)o3a | P | NX;                                /* a read-only directory chain (no RW anywhere) */
    o4[5] = (u64)o3b | P | RW;
    o3a[0] = (u64)o2 | P;
    o3a[1] = 0x40000000ULL | P | PS | PCD | NX;               /* 1 GiB page, read-only, uncached */
    o3b[7] = 0x1C0000000ULL | P | RW | PS | PWT;              /* 1 GiB page, already writable */
    o2[0] = 0x000000ULL | P | PS;                             /* 2 MiB page, read-only */
    o2[1] = (u64)o1 | P;                                      /* a table of 4 KiB pages */
    o2[2] = 0xDEADULL << 1;                                   /* not present, with junk in the other bits */
    o2[3] = 0x600000ULL | P | PS | PATBIG | NX;               /* 2 MiB page with the PAT bit */
    o1[0] = 0x200000ULL | P | PCD;                            /* read-only 4 KiB pages */
    o1[1] = 0x201000ULL | P | PWT | PAT4K | NX;
    o1[2] = 0;
    o1[511] = 0x3FF000ULL | P;

    TABLES_MADE = 0;
    ALLOC_LIMIT = 1000;
    u64 *n4 = paging_clone(o4, alloc_table);
    CHECK(n4 != 0 && n4 != o4);
    CHECK(TABLES_MADE == 5); /* pml4, two pdpt, one pd, one pt: a table of its own for every old one */
    CHECK(!(n4[1] & P) && !(n4[4] & P));
    u64 *n3a = (u64 *)(n4[0] & 0x000FFFFFFFFFF000ULL), *n3b = (u64 *)(n4[5] & 0x000FFFFFFFFFF000ULL);
    CHECK(n3a != o3a && n3b != o3b);
    CHECK((n4[0] & RW) && (n4[0] & NX) && (n4[5] & RW)); /* flags kept, RW added */
    CHECK((n3a[1] & ~RW) == o3a[1] && (n3a[1] & RW));    /* the 1 GiB page: identical but writable */
    CHECK(n3b[7] == o3b[7]);
    u64 *n2 = (u64 *)(n3a[0] & 0x000FFFFFFFFFF000ULL);
    CHECK(n2 != o2 && (n3a[0] & RW));
    CHECK(n2[0] == (o2[0] | RW));
    CHECK(n2[2] == 0);                                    /* not present: stays empty (junk is not copied) */
    CHECK(n2[3] == (o2[3] | RW));                         /* PAT bit and NX kept */
    u64 *n1 = (u64 *)(n2[1] & 0x000FFFFFFFFFF000ULL);
    CHECK(n1 != o1 && (n2[1] & RW));
    CHECK(n1[0] == (o1[0] | RW) && n1[1] == (o1[1] | RW) && n1[511] == (o1[511] | RW));
    CHECK(n1[2] == 0 && n1[3] == 0);
    CHECK(!(o2[0] & RW) && !(o1[0] & RW) && !(o4[0] & RW)); /* the old tables were not touched */

    /* the copy can be edited like any table (a hole in the read-only 2 MiB page), the old one stays as it was */
    TABLES_MADE = 0;
    CHECK(paging_set_present_in(n4, 0x1000, 0, alloc_table) == 0);
    CHECK(!(n2[0] & PS) && o2[0] == (0x000000ULL | P | PS));

    /* ---- 512 equal consecutive 4 KiB pages become one 2 MiB page (no table) ---- */
    u64 *c4 = new_table(), *c3 = new_table(), *c2 = new_table();
    u64 *cpt[6];
    for (int i = 0; i < 6; i++)
        cpt[i] = new_table();
    c4[0] = (u64)c3 | P | RW;
    c3[0] = (u64)c2 | P | RW;
    for (int m = 0; m < 512; m++)
    {
        cpt[0][m] = (0x400000ULL + ((u64)m << 12)) | P | PCD | PAT4K | NX | (m % 3 ? 0x20ULL : 0); /* accessed on some */
        cpt[1][m] = (0x800000ULL + ((u64)m << 12)) | P | RW;
        cpt[2][m] = (0xA00000ULL + ((u64)m << 12)) | P | RW;
        cpt[3][m] = (0xC00000ULL + 0x1000 + ((u64)m << 12)) | P | RW;   /* does not start on a 2 MiB boundary */
        cpt[4][m] = (0xE00000ULL + ((u64)m << 12)) | P | RW;
        cpt[5][m] = (0x1000000ULL + ((u64)m << 12)) | P | RW;
    }
    cpt[2][100] += 0x1000;       /* one page out of line */
    cpt[4][511] |= PWT;          /* one page with another attribute */
    cpt[5][7] &= ~P;             /* one page missing */
    c2[0] = (u64)cpt[0] | P | RW;                                       /* merges: keeps PCD, NX, PAT -> PATBIG */
    c2[1] = (u64)cpt[1] | P | RW;                                       /* merges */
    c2[2] = (u64)cpt[2] | P | RW;
    c2[3] = (u64)cpt[3] | P | RW;
    c2[4] = (u64)cpt[4] | P | RW;
    c2[5] = (u64)cpt[5] | P | RW;
    c2[6] = (u64)cpt[1] | P | RW | NX;                                  /* the same pages under a No-Execute directory entry */

    TABLES_MADE = 0;
    ALLOC_LIMIT = 1000;
    u64 *d4 = paging_clone(c4, alloc_table);
    CHECK(d4 != 0);
    CHECK(TABLES_MADE == 3 + 4);  /* pml4 + pdpt + pd, and one table each for the four that cannot merge */
    u64 *d2 = (u64 *)(((u64 *)(d4[0] & 0x000FFFFFFFFFF000ULL))[0] & 0x000FFFFFFFFFF000ULL);
    CHECK((d2[0] & PS) && (d2[0] & 0x000FFFFFFFE00000ULL) == 0x400000ULL);
    CHECK((d2[0] & (P | RW | PCD | NX | PATBIG | 0x20)) == (P | RW | PCD | NX | PATBIG | 0x20));
    CHECK(!(d2[0] & PWT)); /* no stray bits */
    CHECK((d2[1] & PS) && (d2[1] & 0x000FFFFFFFE00000ULL) == 0x800000ULL && !(d2[1] & NX));
    CHECK(!(d2[2] & PS) && !(d2[3] & PS) && !(d2[4] & PS) && !(d2[5] & PS)); /* kept as tables */
    CHECK((d2[6] & PS) && (d2[6] & NX));                                      /* the directory's NX is applied */
    u64 *dpt = (u64 *)(d2[2] & 0x000FFFFFFFFFF000ULL);
    CHECK(dpt != cpt[2] && dpt[100] == (cpt[2][100] | RW));
    CHECK(!(((u64 *)(d2[5] & 0x000FFFFFFFFFF000ULL))[7] & P)); /* the missing page is still missing */

    /* ---- user address spaces ---- */
    TABLES_MADE = 0;
    ALLOC_LIMIT = 1000;
    u64 *us = new_table();
    const u64 UB = PAGING_USER_BASE;
    CHECK(PAGING_USER_BASE == 0x0000400000000000ULL && PAGING_USER_END == 0x0000800000000000ULL);
    CHECK(((PAGING_USER_BASE >> 39) & 511) == PAGING_USER_SLOT_FIRST);
    CHECK((((PAGING_USER_END - 1) >> 39) & 511) == PAGING_USER_SLOT_LAST);

    /* mapping: writable data, read-only data, and code */
    CHECK(paging_map_page(us, UB + 0x1000, 0x123000, PAGING_WRITE, alloc_table) == 0);
    CHECK(TABLES_MADE == 3); /* pdpt, pd, pt */
    u64 ph = 0, en = 0;
    CHECK(paging_translate(us, UB + 0x1000 + 0x234, &ph, &en) == 0 && ph == 0x123234);
    CHECK((en & (P | RW | (1ULL << 2) | NX)) == (P | RW | (1ULL << 2) | NX)); /* present, writable, user, no-exec */
    CHECK(!(en & (1ULL << 9))); /* not marked owned */
    CHECK(paging_map_page(us, UB + 0x2000, 0x124000, 0, alloc_table) == 0);
    CHECK(paging_translate(us, UB + 0x2000, &ph, &en) == 0 && !(en & RW) && (en & NX));
    CHECK(paging_map_page(us, UB + 0x3000, 0x125000, PAGING_EXEC | PAGING_OWNED, alloc_table) == 0);
    CHECK(paging_translate(us, UB + 0x3000, &ph, &en) == 0 && !(en & NX) && (en & (1ULL << 9)));
    CHECK(TABLES_MADE == 3); /* the same pt: no new tables */
    /* the tables on the way are open to ring 3 and writable (the page decides) */
    {
        u64 *p3 = (u64 *)(us[PAGING_USER_SLOT_FIRST] & 0x000FFFFFFFFFF000ULL);
        CHECK((us[PAGING_USER_SLOT_FIRST] & (P | RW | (1ULL << 2))) == (P | RW | (1ULL << 2)));
        CHECK((p3[0] & (P | RW | (1ULL << 2))) == (P | RW | (1ULL << 2)));
    }

    /* refused: already mapped, unaligned, outside the user part */
    CHECK(paging_map_page(us, UB + 0x1000, 0x999000, 0, alloc_table) == -1);
    CHECK(paging_map_page(us, UB + 0x1001, 0x999000, 0, alloc_table) == -1);
    CHECK(paging_map_page(us, UB + 0x9000, 0x999001, 0, alloc_table) == -1);
    CHECK(paging_map_page(us, UB - 0x1000, 0x999000, 0, alloc_table) == -1);
    CHECK(paging_map_page(us, PAGING_USER_END, 0x999000, 0, alloc_table) == -1);
    CHECK(paging_map_page(us, 0x1000, 0x999000, 0, alloc_table) == -1);
    CHECK(paging_map_page(us, UB + 0x9000, 1ULL << 60, 0, alloc_table) == -1); /* not a physical address */
    CHECK(paging_translate(us, UB + 0x9000, &ph, &en) == -1);

    /* a table that ring 3 may not use (the kernel's) or a big page in the way is never modified */
    {
        u64 *k4 = new_table(), *k3 = new_table(), *b3 = new_table();
        u64 va2 = UB + (5ULL << 39);                       /* slot 133 */
        k4[133] = (u64)k3 | P | RW;                        /* no User bit */
        CHECK(paging_map_page(k4, va2, 0x1000, 0, alloc_table) == -1);
        CHECK(k4[133] == ((u64)k3 | P | RW)); /* untouched */
        k4[134] = (u64)b3 | P | RW | (1ULL << 2);
        b3[0] = 0x40000000ULL | P | RW | PS | (1ULL << 2); /* a 1 GiB page */
        CHECK(paging_map_page(k4, UB + (6ULL << 39), 0x1000, 0, alloc_table) == -1);
        CHECK(!paging_user_range_ok(k4, UB + (6ULL << 39), 8, 0)); /* a big page is not user memory either */
    }

    /* translate understands the big pages of the kernel's identity map */
    {
        u64 *t4 = new_table(), *t3 = new_table(), *t2 = new_table();
        t4[0] = (u64)t3 | P | RW;
        t3[0] = (u64)t2 | P | RW;
        t3[2] = 0x80000000ULL | P | RW | PS;
        t2[1] = 0x200000ULL | P | RW | PS;
        CHECK(paging_translate(t4, 0x80012345ULL, &ph, 0) == 0 && ph == 0x80012345ULL);
        CHECK(paging_translate(t4, 0x2ABCDEULL, &ph, &en) == 0 && ph == 0x2ABCDEULL && (en & PS));
        CHECK(paging_translate(t4, 0x400000ULL, &ph, 0) == -1); /* t2[2] is empty */
        CHECK(paging_translate(t4, 0x40000000ULL, &ph, 0) == -1);
        CHECK(paging_translate(t4, 1ULL << 40, &ph, 0) == -1);
    }

    /* what a system call checks before it touches a pointer from ring 3 */
    CHECK(paging_user_range_ok(us, UB + 0x1000, 1, 1));
    CHECK(paging_user_range_ok(us, UB + 0x1000 + 0xFF8, 8, 1));            /* the last bytes of the page */
    CHECK(paging_user_range_ok(us, UB + 0x1000 + 0xFF8, 9, 0));            /* one byte into the next page: it is mapped, read-only, so a read is fine */
    CHECK(!paging_user_range_ok(us, UB + 0x1000 + 0xFF8, 9, 1));           /* ... but a write there is not */
    CHECK(paging_user_range_ok(us, UB + 0x1800, 0x1000, 0));               /* two pages, both mapped, read */
    CHECK(!paging_user_range_ok(us, UB + 0x1800, 0x1000, 1));              /* ... but the second is not writable */
    CHECK(paging_user_range_ok(us, UB + 0x2000, 16, 0));                   /* read-only page, read */
    CHECK(!paging_user_range_ok(us, UB + 0x2000, 16, 1));                  /* ... write */
    CHECK(!paging_user_range_ok(us, UB + 0x4000, 1, 0));                   /* not mapped */
    CHECK(!paging_user_range_ok(us, UB + 0x1000, 0, 0));                   /* empty range */
    CHECK(!paging_user_range_ok(us, 0x1000, 8, 0));                        /* kernel address */
    CHECK(!paging_user_range_ok(us, UB - 1, 2, 0));                        /* starts below the user part */
    CHECK(!paging_user_range_ok(us, PAGING_USER_END - 4, 8, 0));           /* runs past the end */
    CHECK(!paging_user_range_ok(us, UB + 0x1000, ~0ULL, 0));               /* length that wraps around */
    CHECK(!paging_user_range_ok(us, UB + 0x1000, ~0ULL - UB, 0));

    /* tearing down: owned pages and every table are freed, shared pages are not */
    CHECK(paging_map_page(us, PAGING_USER_END - 0x1000, 0x777000, PAGING_WRITE | PAGING_OWNED, alloc_table) == 0); /* slot 255 */
    CHECK(paging_map_page(us, UB + 0x5000, 0x778000, PAGING_OWNED, alloc_table) == 0);
    int tables = TABLES_MADE; /* pdpt+pd+pt for slot 128, the same again for slot 255 */
    CHECK(tables == 6);
    us[3] = 0xABCD000ULL | P | RW; /* a kernel slot: must stay as it is */
    FREED_N = 0;
    paging_destroy_range(us, PAGING_USER_SLOT_FIRST, PAGING_USER_SLOT_LAST, free_record);
    CHECK(FREED_N == 6 + 3); /* 6 tables + the owned pages 0x125000, 0x778000, 0x777000 */
    CHECK(freed(0x125000) && freed(0x778000) && freed(0x777000));
    CHECK(!freed(0x123000) && !freed(0x124000)); /* mapped without PAGING_OWNED: shared, left alone */
    CHECK(us[PAGING_USER_SLOT_FIRST] == 0 && us[PAGING_USER_SLOT_LAST] == 0);
    CHECK(us[3] == (0xABCD000ULL | P | RW));
    CHECK(paging_translate(us, UB + 0x1000, &ph, 0) == -1); /* nothing is mapped any more */
    int found = 0; /* every table the walk allocated was freed (the first one in MADE is us itself: made by new_table) */
    for (int j = 0; j < FREED_N; j++)
        for (int t = 0; t < TABLES_MADE; t++)
            if (FREED[j] == (u64)MADE[t])
                found++;
    CHECK(found == 6);
    paging_destroy_range(us, PAGING_USER_SLOT_FIRST, PAGING_USER_SLOT_LAST, free_record); /* nothing left: nothing freed */
    CHECK(FREED_N == 9);

    /* not enough memory for the copy: NULL for every limit below what it needs, never a crash */
    for (int limit = 0; limit < 5; limit++)
    {
        TABLES_MADE = 0;
        ALLOC_LIMIT = limit;
        CHECK(paging_clone(o4, alloc_table) == 0);
    }

    if (fails == 0)
        printf("paging tests passed\n");
    return fails ? 1 : 0;
}
