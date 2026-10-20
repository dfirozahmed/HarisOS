/* Host test for pci.c using a fake config space. */
#include <stdio.h>
#include <string.h>
#include "pci.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* 00:00.0 host bridge (single function), 00:01.0 multi-function with fn0 + fn2 */
static unsigned int fake(unsigned bus, unsigned dev, unsigned fn, unsigned off)
{
    if (bus != 0)
        return 0xFFFFFFFFu;
    if (dev == 0 && fn == 0) {
        if (off == 0x00) return 0x12378086u;
        if (off == 0x08) return 0x06000002u;
        if (off == 0x0C) return 0x00000000u;
    }
    if (dev == 1 && fn == 0) {
        if (off == 0x00) return 0x1000144Du;
        if (off == 0x08) return 0x01080201u;
        if (off == 0x0C) return 0x00800000u;
    }
    if (dev == 1 && fn == 2) {
        if (off == 0x00) return 0x20001B36u;
        if (off == 0x08) return 0x0C033000u;
        if (off == 0x0C) return 0x00000000u;
    }
    return 0xFFFFFFFFu;
}

int main(void)
{
    CHECK(pci_scan_with(fake) == 3);
    const PciDevice *d = pci_get(0);
    CHECK(d && d->vendor_id == 0x8086 && d->device_id == 0x1237);
    CHECK(d && d->class_code == 0x06 && d->subclass == 0x00 && d->revision == 2);
    d = pci_get(1);
    CHECK(d && d->class_code == 0x01 && d->subclass == 0x08 && d->prog_if == 0x02);
    d = pci_get(2);
    CHECK(d && d->fn == 2 && d->vendor_id == 0x1B36);
    CHECK(pci_get(3) == NULL && pci_get(-1) == NULL);
    CHECK(strcmp(pci_class_name(0x0C, 0x03), "USB controller") == 0);
    CHECK(strcmp(pci_class_name(0xEE, 0), "Unknown device") == 0);
    CHECK(strcmp(pci_vendor_name(0x8086), "Intel") == 0);
    CHECK(strcmp(pci_vendor_name(0x0001), "Unknown vendor") == 0);
    CHECK(pci_scan_with(NULL) == 0 && pci_count() == 0);
    if (!fails) printf("pci tests passed\n");
    return fails ? 1 : 0;
}
