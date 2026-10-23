/* Host test for the copy-on-write page counts (kernel/memory/cow.c): holders, last release, many pages, collisions, a full table. */
#include <stdio.h>
#include "cow.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

typedef unsigned long long u64;

int main(void)
{
    /* a page nobody shares: the one holder is the last */
    CHECK(!cow_shared(0x5000) && cow_release(0x5000) == 1 && cow_shared_pages() == 0);

    /* two holders: the first to let go leaves one, the second is the last */
    CHECK(cow_share(0x5000) == 0 && cow_shared(0x5000) && cow_shared_pages() == 1);
    CHECK(cow_release(0x5000) == 0 && !cow_shared(0x5000) && cow_shared_pages() == 0);
    CHECK(cow_release(0x5000) == 1);

    /* three holders (a fork of a fork) */
    CHECK(cow_share(0x6000) == 0 && cow_share(0x6000) == 0 && cow_shared_pages() == 1);
    CHECK(cow_release(0x6000) == 0 && cow_shared(0x6000));                 /* two left */
    CHECK(cow_release(0x6000) == 0 && !cow_shared(0x6000));                /* one left: it is the last holder alone now */
    CHECK(cow_release(0x6000) == 1);

    /* the offset inside a page does not matter */
    CHECK(cow_share(0x7123) == 0 && cow_shared(0x7000) && cow_shared(0x7FFF) && !cow_shared(0x8000));
    CHECK(cow_release(0x7FFF) == 0 && cow_shared_pages() == 0);

    /* many pages at once, then released in another order: every one ends up last exactly once */
    enum { N = 40000 };
    int ok = 1;
    for (int i = 0; i < N; i++)
        ok = ok && cow_share((u64)(i + 100) << 12) == 0;
    CHECK(ok && cow_shared_pages() == N);
    for (int i = 0; i < N; i += 2)
        ok = ok && cow_release((u64)(i + 100) << 12) == 0;                   /* the even ones: one holder left */
    CHECK(ok && cow_shared_pages() == N / 2);
    for (int i = 1; i < N; i += 2)
        ok = ok && cow_shared((u64)(i + 100) << 12);                         /* the odd ones are still shared, found through the removals */
    for (int i = 0; i < N; i += 2)
        ok = ok && !cow_shared((u64)(i + 100) << 12);
    CHECK(ok);
    for (int i = N - 1; i >= 1; i -= 2)
        ok = ok && cow_release((u64)(i + 100) << 12) == 0;
    CHECK(ok && cow_shared_pages() == 0);
    for (int i = 0; i < N; i++)
        ok = ok && cow_release((u64)(i + 100) << 12) == 1;
    CHECK(ok);

    /* many pages with similar numbers: all stay findable while some are removed from the middle of a probe run */
    for (int i = 0; i < 3000; i++)
        cow_share((u64)(i * 65536 + 1) << 12);
    for (int i = 0; i < 3000; i += 3)
        cow_release((u64)(i * 65536 + 1) << 12);
    ok = 1;
    for (int i = 0; i < 3000; i++)
        ok = ok && (cow_shared((u64)(i * 65536 + 1) << 12) == (i % 3 != 0));
    CHECK(ok && cow_shared_pages() == 2000);
    for (int i = 0; i < 3000; i++)
        if (i % 3 != 0)
            cow_release((u64)(i * 65536 + 1) << 12);
    CHECK(cow_shared_pages() == 0);

    /* a full table refuses (the caller copies the page instead), and frees up again */
    int made = 0;
    while (cow_share((u64)(made + 1000000) << 12) == 0)
        made++;
    CHECK(made > 50000 && made < COW_TABLE_SIZE && cow_shared_pages() == (unsigned)made);
    CHECK(cow_share((u64)1000000 << 12) == 0);                              /* one that is in already can still get another holder */
    cow_release((u64)1000000 << 12);
    for (int i = 0; i < made; i++)
        cow_release((u64)(i + 1000000) << 12);
    CHECK(cow_shared_pages() == 0 && cow_share(0x9000) == 0 && cow_release(0x9000) == 0 && cow_release(0x9000) == 1);

    if (fails == 0)
        printf("cow tests passed\n");
    return fails ? 1 : 0;
}
