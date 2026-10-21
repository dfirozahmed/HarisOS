/* Display adapter driver: finds the GPUs on PCI (class 03h), reads their memory windows and
 * tells which one shows the firmware (UEFI GOP) framebuffer.
 * Adapters with a register interface HarisOS knows (Bochs VBE: QEMU "std" VGA, VirtualBox VGA)
 * get native mode setting; every other adapter (Intel, AMD, NVIDIA ...) keeps the mode the firmware chose
 * (a "basic display" driver: the framebuffer works, there is no acceleration or mode switching). */
#ifndef HARIS_DISPLAY_H
#define HARIS_DISPLAY_H

#include "pci.h"

#define DISPLAY_MAX 4

typedef enum {
    DISP_BASIC, /* firmware framebuffer only */
    DISP_BOCHS  /* Bochs VBE (DISPI) registers: native mode setting */
} DisplayKind;

typedef struct {
    unsigned char bus, dev, fn;
    unsigned short vendor_id, device_id;
    DisplayKind kind;
    unsigned long long fb_base;   /* BAR0 (the video memory window) */
    unsigned long long mmio_base; /* the register window (Bochs: BAR2), 0 = none */
    int active;                   /* its BAR0 holds the framebuffer the kernel is drawing into */
    /* Bochs only (0 = unknown) */
    unsigned vram_bytes;
    unsigned cur_w, cur_h, cur_bpp;
    unsigned max_w, max_h, max_bpp;
} DisplayAdapter;

/* How the Bochs registers are reached; the default is the I/O ports 0x1CE (index) / 0x1CF (data).
 * Host tests pass a fake register file. */
typedef struct {
    unsigned short (*read)(unsigned short index);
    void (*write)(unsigned short index, unsigned short value);
    PciReadFn cfg; /* PCI config reader for the BARs; NULL = pci_cfg_read (real hardware) */
} DisplayRegs;

/* Scans pci_get() for display controllers. fb_addr = BootInfo.framebuffer (0 = unknown).
 * regs may be NULL (real hardware ports). Needs pci_scan() first. Returns the adapter count. */
int display_init(unsigned long long fb_addr, const DisplayRegs *regs);

int display_count(void);
const DisplayAdapter *display_get(int index); /* NULL if out of range */

/* Bochs adapters only: switches to w x h at 32 bits per pixel. Returns 0 on success, -1 if the adapter is
 * not a Bochs one, the size is not allowed (outside the limits, width not a multiple of 8) or the card did not accept it.
 * The framebuffer pitch changes, so the caller has to call gfx_init() again afterwards. */
int display_set_mode(int index, unsigned w, unsigned h);

const char *display_driver_name(DisplayKind kind);

#endif
