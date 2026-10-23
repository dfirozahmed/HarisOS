/* Host test for protect.c: which CR4 bits are set for what CPUID leaf 7 announces. */
#include "protect.h"
#include <stdio.h>

typedef unsigned long long u64;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    const unsigned SMEP = 1u << 7, SMAP = 1u << 20;
    const u64 base = 0x00000000000006F0ULL | (1ULL << 7); /* some CR4 bits already on (PGE is bit 7) */

    CHECK(CR4_SMEP == (1ULL << 20) && CR4_SMAP == (1ULL << 21));

    CHECK(protect_cr4(0, base) == base);                                    /* neither: nothing changes */
    CHECK(protect_cr4(SMEP, base) == (base | CR4_SMEP));                    /* SMEP only */
    CHECK(protect_cr4(SMAP, base) == (base | CR4_SMAP));                    /* SMAP only */
    CHECK(protect_cr4(SMEP | SMAP, base) == (base | CR4_SMEP | CR4_SMAP));  /* both */

    /* other CPUID bits in the same register mean other things and must not turn anything on */
    CHECK(protect_cr4(~(SMEP | SMAP), base) == base);
    CHECK(protect_cr4(1u << 6 | 1u << 8 | 1u << 19 | 1u << 21, 0) == 0);

    /* idempotent, and it never clears a bit that is already set */
    u64 once = protect_cr4(SMEP | SMAP, base);
    CHECK(protect_cr4(SMEP | SMAP, once) == once);
    CHECK(protect_cr4(0, once) == once);
    CHECK(protect_cr4(SMEP | SMAP, ~0ULL) == ~0ULL);

    if (fails == 0)
        printf("protect tests passed\n");
    return fails ? 1 : 0;
}
