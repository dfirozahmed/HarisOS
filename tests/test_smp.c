/* Host test for smp.c: the processor list and the fields patched into the start-up page. (The INIT-SIPI sequence and the climb
 * to 64-bit mode are hardware: they are checked at boot, "HARISOS: SMP: CPU n online" on the serial port.) */
#include <stdio.h>
#include <string.h>
#include "smp.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return rd16(p) | rd16(p + 2) << 16; }
static unsigned long long rd64(const unsigned char *p) { return rd32(p) | (unsigned long long)rd32(p + 4) << 32; }

int main(void)
{
    /* processor list: the boot processor, duplicates and ids that do not fit in 8 bits are left out */
    unsigned ids[] = {0, 1, 2, 2, 3, 300, 5};
    unsigned out[8];
    CHECK(smp_pick_aps(ids, 7, 0, 0xFF, out, 8) == 4);
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 5);
    CHECK(smp_pick_aps(ids, 7, 2, 0xFF, out, 8) == 4 && out[0] == 0 && out[1] == 1 && out[2] == 3);
    CHECK(smp_pick_aps(ids, 7, 0, 0xFF, out, 2) == 2); /* the limit */
    CHECK(smp_pick_aps(ids, 0, 0, 0xFF, out, 8) == 0);
    CHECK(smp_pick_aps(ids, 7, 0, 0xFFFFFFFEu, out, 8) == 5 && out[3] == 300 && out[4] == 5); /* x2APIC: ids above 255 are fine */
    unsigned big[] = {0, 1, 0xFFFFFFFFu};
    CHECK(smp_pick_aps(big, 3, 0, 0xFFFFFFFEu, out, 8) == 1); /* 0xFFFFFFFF is the broadcast id, not a processor */
    unsigned one[] = {4};
    CHECK(smp_pick_aps(one, 1, 4, 0xFF, out, 8) == 0); /* only the boot processor */

    /* the patched page */
    unsigned char img[SMP_IMAGE_SIZE];
    memset(img, 0xEE, sizeof(img));
    smp_patch_image(img, 0x9A000, 0x9B000, 0x9C000, 0x40, 0x80);
    CHECK(rd16(img + SMP_OFF_GDTR) == 31);
    CHECK(rd32(img + SMP_OFF_GDTR + 2) == 0x9A000 + SMP_OFF_GDT); /* the GDT is in the page itself */
    CHECK(rd32(img + SMP_OFF_PM_PTR) == 0x9A040 && rd16(img + SMP_OFF_PM_PTR + 4) == 0x18);
    CHECK(rd32(img + SMP_OFF_LM_PTR) == 0x9A080 && rd16(img + SMP_OFF_LM_PTR + 4) == 0x08);
    CHECK(rd32(img + SMP_OFF_DATA) == 0x9B000 && rd32(img + SMP_OFF_PML4) == 0x9C000);
    CHECK(rd64(img + SMP_OFF_GDT) == 0);
    CHECK(rd64(img + SMP_OFF_GDT + 8) == 0x00209A0000000000ULL);  /* L = 1, D = 0: 64-bit code */
    CHECK(rd64(img + SMP_OFF_GDT + 16) == 0x00CF92000000FFFFULL); /* flat data */
    CHECK(rd64(img + SMP_OFF_GDT + 24) == 0x00CF9A000000FFFFULL); /* flat 32-bit code */
    CHECK(img[0] == 0xEE && img[SMP_OFF_GDTR - 1] == 0xEE);       /* the code in front of the fields is not touched */
    CHECK(SMP_OFF_GDT + 32 == SMP_IMAGE_SIZE);

    /* the fields do not overlap */
    CHECK(SMP_OFF_GDTR + 8 <= SMP_OFF_PM_PTR && SMP_OFF_PM_PTR + 8 <= SMP_OFF_LM_PTR && SMP_OFF_LM_PTR + 8 <= SMP_OFF_DATA);
    CHECK(SMP_OFF_DATA + 4 <= SMP_OFF_PML4 && SMP_OFF_PML4 + 4 <= SMP_OFF_GDT);

    /* the trampoline's page tables: 2 MiB at address 0, nothing else */
    static unsigned long long t[3][512];
    memset(t, 0xFF, sizeof(t));
    smp_build_tables(t[0], t[1], t[2], 0x9D000, 0x9E000);
    CHECK(t[0][0] == (0x9D000 | 3) && t[1][0] == (0x9E000 | 3) && t[2][0] == 0x83);
    int rest = 0;
    for (int tb = 0; tb < 3; tb++)
        for (int i = 1; i < 512; i++)
            rest |= t[tb][i] != 0;
    CHECK(!rest);

    /* without the hardware: one processor, the boot one */
    CHECK(smp_cpu_total() == 1 && smp_cpu_online(0) && !smp_cpu_online(1));
    CHECK(smp_start(0, 0) == 0 && smp_install(0, 0) == -1 && smp_cpu_ticks(1) == 0);

    if (fails == 0)
        printf("smp tests passed\n");
    return fails ? 1 : 0;
}
