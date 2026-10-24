/* Host test for ioapic.c: the redirection entry encoding. (The chip itself is hardware: it is checked at boot, see net_irq_init.) */
#include <stdio.h>
#include "ioapic.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* a PCI line: level triggered (bit 15), active low (bit 13), fixed delivery, physical destination, not masked */
    CHECK(ioapic_entry(0x41, 1, 1, 0) == (0x41u | 1u << 13 | 1u << 15));
    CHECK(ioapic_entry(0x41, 1, 1, 0) == 0x0000A041u);
    /* the same, masked */
    CHECK(ioapic_entry(0x41, 1, 1, 1) == 0x0001A041u);
    /* an ISA style line: edge, active high */
    CHECK(ioapic_entry(0x30, 0, 0, 0) == 0x00000030u);
    CHECK(ioapic_entry(0x30, 0, 0, 1) == 0x00010030u);
    /* only the low 8 bits of the vector are used */
    CHECK(ioapic_entry(0x1FF, 0, 0, 0) == 0xFFu);
    /* nothing is present before ioapic_init */
    CHECK(!ioapic_present() && ioapic_gsi_count() == 0);
    /* routing a line that no chip serves is refused (and touches no hardware) */
    CHECK(ioapic_route(16, 0x41, 0) == -1);
    ioapic_mask(16); /* does nothing, does not crash */

    if (fails == 0)
        printf("ioapic tests passed\n");
    return fails ? 1 : 0;
}
