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


/* 00:01.0 VGA controller (class 03) */
static unsigned int fake_gpu(unsigned bus, unsigned dev, unsigned fn, unsigned off)
{
    if (bus != 0 || fn != 0 || dev != 1)
        return 0xFFFFFFFFu;
    if (off == 0x00) return 0x11111234u;
    if (off == 0x08) return 0x03000002u;
    if (off == 0x0C) return 0;
    return 0xFFFFFFFFu;
}

int main(void)
{
    pci_scan_with(fake);
    static DevmgrInputs in; /* zeroed (static): it is large */
    in.keyboard_ps2 = 1;
    in.mouse_sources = MOUSE_SRC_PS2;
    in.usb_running = 1;
    in.usb_mice = 1;
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

    /* USB mouse remark: boot protocol has no wheel; report protocol says whether the descriptor has one */
    CHECK(strcmp(devmgr_get(4)->note, "Boot protocol: 3-byte reports, no scroll wheel") == 0);
    in.usb_mouse_rp[0] = 1;
    devmgr_build(&in);
    CHECK(strcmp(devmgr_get(4)->note, "Report protocol; scroll wheel: no") == 0);
    in.usb_mouse_wheel[0] = 1;
    devmgr_build(&in);
    CHECK(strcmp(devmgr_get(4)->note, "Report protocol; scroll wheel: yes") == 0);
    in.usb_mouse_rp[0] = in.usb_mouse_wheel[0] = 0;

    /* hot-plug: a second mouse and a keyboard appear -> more rows after a rebuild */
    in.usb_mice = 2;
    in.usb_keyboards = 1;
    CHECK(devmgr_build(&in) == 7);
    CHECK(strcmp(devmgr_get(6)->name, "USB keyboard") == 0);
    in.usb_mice = 1;
    in.usb_keyboards = 0;

    /* xHCI present but the driver did not start -> no driver */
    in.usb_running = 0;
    devmgr_build(&in);
    CHECK(!devmgr_get(1)->has_driver);

    /* I2C-HID: one started touchpad, one the firmware describes but the driver did not start */
    in.i2c_count = 2;
    in.i2c[0] = (DevmgrI2c){0x2C, 400000, "I2CD", 1, 0, 0, 0, 0};
    in.i2c[0].reason = "ignored when started";
    in.i2c[1] = (DevmgrI2c){0x10, 100000, "", 0, 0, 0, 0, "device gave no HID descriptor"};
    CHECK(devmgr_build(&in) == 7);
    const DevEntry *t = devmgr_get(5);
    CHECK(strcmp(t->name, "I2C-HID touchpad") == 0 && t->has_driver && t->category == DEV_CAT_INPUT);
    CHECK(strcmp(t->detail, "I2C 0x2C on I2CD, 400 kHz") == 0);
    t = devmgr_get(6);
    CHECK(strcmp(t->name, "I2C-HID device") == 0 && !t->has_driver);
    CHECK(strcmp(t->detail, "I2C 0x10 on ?, 100 kHz") == 0);
    CHECK(strcmp(t->note, "device gave no HID descriptor") == 0);
    CHECK(strcmp(devmgr_get(5)->note, "Mouse mode; no scroll wheel in its report") == 0); /* started: pointer remark, reason ignored */
    in.i2c[0].has_wheel = 1;
    devmgr_build(&in);
    CHECK(strcmp(devmgr_get(5)->note, "Mouse mode; the report has a scroll wheel") == 0);
    in.i2c[0].has_wheel = 0;
    in.i2c[0].ptp_fingers = 5;                   /* precision pad without a wheel */
    in.i2c[0].ptp_mode = 1;
    devmgr_build(&in);
    CHECK(strcmp(devmgr_get(5)->note, "No wheel; precision pad: 5 contacts, mode switch: yes") == 0);
    in.i2c[0].ptp_mode = 0;
    devmgr_build(&in);
    CHECK(strcmp(devmgr_get(5)->note, "No wheel; precision pad: 5 contacts, mode switch: no") == 0);
    in.i2c[0].has_wheel = 1;                     /* a wheel wins: the plain wheel message */
    devmgr_build(&in);
    CHECK(strcmp(devmgr_get(5)->note, "Mouse mode; the report has a scroll wheel") == 0);
    in.i2c[0].has_wheel = in.i2c[0].ptp_fingers = 0;
    CHECK(devmgr_get(0)->note[0] == 0);          /* PCI: no note */
    in.i2c[1].reason = 0;                        /* unknown reason: empty note, no crash */
    devmgr_build(&in);
    CHECK(devmgr_get(6)->note[0] == 0);
    in.i2c_count = 99; /* out-of-range count is clamped */
    CHECK(devmgr_build(&in) == 5 + DEVMGR_MAX_I2C);

    CHECK(devmgr_build(NULL) == 2);
    CHECK(strcmp(devmgr_category_name(DEV_CAT_INPUT), "Input devices") == 0);

    /* display adapter: native (Bochs) / active firmware framebuffer / inactive */
    pci_scan_with(fake_gpu);
    static DevmgrInputs gin;
    gin.display_count = 1;
    gin.displays[0] = (DevmgrDisplay){0, 1, 0, 1, 1, 1280, 800, 32, 16};
    CHECK(devmgr_build(&gin) == 1);
    CHECK(devmgr_get(0)->category == DEV_CAT_DISPLAY && devmgr_get(0)->has_driver);
    CHECK(strcmp(devmgr_get(0)->note, "Bochs VBE 1280x800x32, 16 MB video memory") == 0);
    gin.displays[0].native = 0;
    devmgr_build(&gin);
    CHECK(devmgr_get(0)->has_driver && strcmp(devmgr_get(0)->note, "Firmware framebuffer; no mode switching") == 0);
    gin.displays[0].active = 0;
    devmgr_build(&gin);
    CHECK(!devmgr_get(0)->has_driver);
    gin.display_count = 0; /* no display driver data: no driver, no note */
    devmgr_build(&gin);
    CHECK(!devmgr_get(0)->has_driver && devmgr_get(0)->note[0] == 0);

    if (!fails) printf("devmgr tests passed\n");
    return fails ? 1 : 0;
}
