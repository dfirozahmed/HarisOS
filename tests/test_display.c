/* Host test for display.c: finds GPUs, Bochs VBE probe and mode setting on a fake register file. */
#include <stdio.h>
#include <string.h>
#include "display.h"
#include "pci.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* 00:01.0 Bochs VGA (BAR0 = 0xFD000000, BAR2 = 0xFEBF0000), 00:02.0 Intel iGPU (64-bit BAR0 = 0x1_C000_0000), 00:03.0 NIC */
static unsigned int fake(unsigned bus, unsigned dev, unsigned fn, unsigned off)
{
    if (bus != 0 || fn != 0)
        return 0xFFFFFFFFu;
    if (dev == 1) {
        if (off == 0x00) return 0x11111234u;
        if (off == 0x08) return 0x03000002u;
        if (off == 0x0C) return 0;
        if (off == 0x10) return 0xFD000008u;
        if (off == 0x14) return 0;
        if (off == 0x18) return 0xFEBF0000u;
    }
    if (dev == 2) {
        if (off == 0x00) return 0x9A498086u;
        if (off == 0x08) return 0x03000001u;
        if (off == 0x0C) return 0;
        if (off == 0x10) return 0xC000000Cu; /* 64-bit prefetchable */
        if (off == 0x14) return 0x1u;
    }
    if (dev == 3) {
        if (off == 0x00) return 0x100E8086u;
        if (off == 0x08) return 0x02000000u;
        if (off == 0x0C) return 0;
    }
    return 0xFFFFFFFFu;
}

/* fake DISPI register file: size registers hold the limits while GETCAPS is set */
static unsigned short R[16];
static void wr(unsigned short i, unsigned short v)
{
    if (i < 16)
        R[i] = v;
}
static unsigned short rd(unsigned short i)
{
    if (i == 4)
        return R[4];
    if ((R[4] & 2) && i == 1) return 2560;
    if ((R[4] & 2) && i == 2) return 1600;
    if ((R[4] & 2) && i == 3) return 32;
    return i < 16 ? R[i] : 0xFFFF;
}

int main(void)
{
    pci_scan_with(fake);
    R[0] = 0xB0C5;
    R[10] = 128; /* 8 MB */
    R[1] = 1280; R[2] = 800; R[3] = 32; R[4] = 0x41;
    DisplayRegs regs = {rd, wr, fake};

    CHECK(display_init(0xFD000000ull + 0x1000, &regs) == 2);
    const DisplayAdapter *a = display_get(0);
    CHECK(a && a->kind == DISP_BOCHS && a->vendor_id == 0x1234 && a->device_id == 0x1111);
    CHECK(a && a->fb_base == 0xFD000000ull && a->mmio_base == 0xFEBF0000ull);
    CHECK(a && a->active);
    CHECK(a && a->vram_bytes == 8u << 20);
    CHECK(a && a->cur_w == 1280 && a->cur_h == 800 && a->cur_bpp == 32);
    CHECK(a && a->max_w == 2560 && a->max_h == 1600 && a->max_bpp == 32);
    CHECK(R[4] == 0x41); /* probing restored ENABLE */

    const DisplayAdapter *b = display_get(1);
    CHECK(b && b->kind == DISP_BASIC && !b->active);
    CHECK(b && b->fb_base == 0x1C0000000ull); /* 64-bit BAR uses two slots */
    CHECK(display_get(2) == NULL);

    /* mode setting */
    CHECK(display_set_mode(0, 1024, 768) == 0);
    CHECK(R[1] == 1024 && R[2] == 768 && R[3] == 32 && (R[4] & 0x41) == 0x41);
    CHECK(a->cur_w == 1024 && a->cur_h == 768);
    CHECK(display_set_mode(0, 3000, 768) == -1);   /* over the limit */
    CHECK(display_set_mode(0, 1021, 768) == -1);   /* width not a multiple of 8 */
    CHECK(display_set_mode(0, 32, 32) == -1);      /* too small */
    CHECK(display_set_mode(0, 2560, 1600) == -1);  /* 8 MB video memory is not enough at 32 bpp */
    CHECK(display_set_mode(1, 1024, 768) == -1);   /* basic adapter: no mode switching */
    CHECK(display_set_mode(5, 1024, 768) == -1);

    /* no DISPI id behind a Bochs PCI id: falls back to the basic driver */
    R[0] = 0;
    CHECK(display_init(0, &regs) == 2);
    CHECK(display_get(0)->kind == DISP_BASIC && !display_get(0)->active);

    CHECK(strcmp(display_driver_name(DISP_BOCHS), "Bochs VBE display driver") == 0);

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("display tests: OK\n");
    return 0;
}
