/* Host test for devmgr.c: PCI rows + input rows, categories, driver flags. */
#include <stdio.h>
#include <string.h>
#include "devmgr.h"
#include "mouse.h"
#include "pci.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* 00:00.0 host bridge, 00:02.0 xHCI (class 0C03, prog-if 30) */
static unsigned int fake(unsigned bus, unsigned dev, unsigned fn, unsigned off)
{
    if (bus != 0 || fn != 0)
        return 0xFFFFFFFFu;
    if (dev == 0) {
        if (off == 0x00) return 0x12378086u;
        if (off == 0x08) return 0x06000000u;
        if (off == 0x0C) return 0;
    }
    if (dev == 2) {
        if (off == 0x00) return 0x000D1B36u;
        if (off == 0x08) return 0x0C033001u;
        if (off == 0x0C) return 0;
    }
    return 0xFFFFFFFFu;
}

int main(void)
{
    pci_scan_with(fake);
    DevmgrInputs in = {1, MOUSE_SRC_PS2 | MOUSE_SRC_USB, 1};
    CHECK(devmgr_build(&in) == 5); /* 2 PCI + kbd + 2 mice */

    const DevEntry *e = devmgr_get(0);
    CHECK(e && e->category == DEV_CAT_SYSTEM && !e->has_driver);
    CHECK(e && strcmp(e->name, "Host bridge") == 0);
    CHECK(e && strcmp(e->detail, "Intel 8086:1237 at 00:00.0") == 0);

    e = devmgr_get(1);
    CHECK(e && e->category == DEV_CAT_USB && e->has_driver);
    CHECK(e && strcmp(e->detail, "Red Hat (QEMU) 1B36:000D at 00:02.0") == 0);

    CHECK(strcmp(devmgr_get(2)->name, "PS/2 keyboard") == 0);
    CHECK(strcmp(devmgr_get(4)->name, "USB mouse") == 0);
    CHECK(devmgr_get(5) == NULL);

    /* xHCI present but the driver did not start -> no driver */
    in.usb_running = 0;
    devmgr_build(&in);
    CHECK(!devmgr_get(1)->has_driver);

    CHECK(devmgr_build(NULL) == 2);
    CHECK(strcmp(devmgr_category_name(DEV_CAT_INPUT), "Input devices") == 0);

    if (!fails) printf("devmgr tests passed\n");
    return fails ? 1 : 0;
}
