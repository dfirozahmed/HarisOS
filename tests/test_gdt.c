/* Host test for gdt.c: the layout of the GDT (what gdt_build writes), decoded the way the CPU reads descriptors. */
#include "gdt.h"
#include <stdio.h>

typedef unsigned long long u64;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static unsigned access_byte(u64 d) { return (unsigned)(d >> 40) & 0xFF; }
static unsigned flags_nibble(u64 d) { return (unsigned)(d >> 52) & 0xF; }
static unsigned dpl(u64 d) { return (access_byte(d) >> 5) & 3; }
static int present(u64 d) { return (access_byte(d) >> 7) & 1; }
static int is_code(u64 d) { return (access_byte(d) >> 3) & 1; }
static int is_system(u64 d) { return !((access_byte(d) >> 4) & 1); } /* S = 0: a system descriptor such as the TSS */
static int long_mode(u64 d) { return (flags_nibble(d) >> 1) & 1; }
static int default_size(u64 d) { return (flags_nibble(d) >> 2) & 1; }
static int writable_data(u64 d) { return !is_code(d) && ((access_byte(d) >> 1) & 1); }

int main(void)
{
    u64 gdt[GDT_ENTRIES];
    for (int i = 0; i < GDT_ENTRIES; i++)
        gdt[i] = 0xDEADBEEFDEADBEEFULL; /* gdt_build must overwrite every slot */
    const u64 tss[2] = {0x0000890012345678ULL, 0x00000000FFFF8000ULL};
    gdt_build(gdt, tss);

    CHECK(gdt[0] == 0); /* the null descriptor */

    /* the selectors point at the right slots, and a selector's low 3 bits are table / privilege, not index */
    CHECK(SEL_KERNEL_CODE == 8 && SEL_KERNEL_DATA == 16);
    CHECK((SEL_USER_DATA & ~7) == 0x18 && (SEL_USER_DATA & 3) == 3);
    CHECK((SEL_USER_CODE & ~7) == 0x20 && (SEL_USER_CODE & 3) == 3);
    CHECK(SEL_TSS == 0x28 && SEL_TSS / 8 + 2 == GDT_ENTRIES);

    u64 kc = gdt[SEL_KERNEL_CODE / 8], kd = gdt[SEL_KERNEL_DATA / 8];
    u64 ud = gdt[SEL_USER_DATA / 8], uc = gdt[SEL_USER_CODE / 8];

    CHECK(present(kc) && is_code(kc) && dpl(kc) == 0 && long_mode(kc) && !default_size(kc));
    CHECK(present(kd) && writable_data(kd) && dpl(kd) == 0);
    CHECK(present(uc) && is_code(uc) && dpl(uc) == 3 && long_mode(uc) && !default_size(uc));
    CHECK(present(ud) && writable_data(ud) && dpl(ud) == 3);
    CHECK(!is_system(kc) && !is_system(kd) && !is_system(uc) && !is_system(ud));

    /* SYSRET (a later step) loads SS = base + 8 and CS = base + 16 from one base: user data must sit right below user
     * code, and SYSCALL's kernel code / data pair is base, base + 8 */
    CHECK((SEL_USER_CODE & ~7) - (SEL_USER_DATA & ~7) == 8);
    CHECK(SEL_KERNEL_DATA - SEL_KERNEL_CODE == 8);
    CHECK((SEL_USER_DATA & ~7) == 0x10 + 8 && (SEL_USER_CODE & ~7) == 0x10 + 16); /* STAR[63:48] = 0x10 + RPL 3 */

    /* What SYSRET really loads: base + 8 and base + 16 as plain numbers. They must be exactly the user selectors WITH
     * their RPL 3: a real CPU does not add the RPL, and an interrupt in ring 3 would push CS / SS with RPL 0, which IRETQ
     * refuses with #GP (the bug that crashed a real machine; an emulator forces RPL 3 and did not show it). */
    CHECK(SEL_SYSRET_BASE + 8 == SEL_USER_DATA && SEL_SYSRET_BASE + 16 == SEL_USER_CODE);
    CHECK((SEL_SYSRET_BASE & 3) == 3);
    CHECK((SEL_USER_DATA & 3) == (SEL_USER_CODE & 3)); /* IRETQ: the RPL of SS must equal the RPL (= CPL) of CS */

    /* the TSS descriptor is copied as it is, into two consecutive slots */
    CHECK(gdt[SEL_TSS / 8] == tss[0] && gdt[SEL_TSS / 8 + 1] == tss[1]);

    if (fails == 0)
        printf("gdt tests passed\n");
    return fails ? 1 : 0;
}
