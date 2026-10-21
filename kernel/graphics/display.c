/* Display adapter driver. See display.h.
 *
 * Bochs VBE ("DISPI") registers are 16 bits wide, written through an index/data port pair.
 * ENABLE bit 0 turns the extended mode on, bit 1 asks for the limits instead of the
 * current values, bit 6 selects the linear framebuffer (without it the card uses 64K banks). */
#include "display.h"
#include "io.h"
#include "pci.h"

#define DISPI_ID 0
#define DISPI_XRES 1
#define DISPI_YRES 2
#define DISPI_BPP 3
#define DISPI_ENABLE 4
#define DISPI_BANK 5
#define DISPI_VIRT_WIDTH 6
#define DISPI_X_OFFSET 8
#define DISPI_Y_OFFSET 9
#define DISPI_VIDEO_MEMORY_64K 10

#define DISPI_ID_MIN 0xB0C0
#define DISPI_ID_MAX 0xB0C5
#define DISPI_ENABLED 0x01
#define DISPI_GETCAPS 0x02
#define DISPI_LFB 0x40

#define DISPI_MIN_SIZE 64u

static DisplayAdapter g_ad[DISPLAY_MAX];
static int g_count;
static const DisplayRegs *g_regs;
static PciReadFn g_cfg;

#if defined(__x86_64__) || defined(__i386__)
static unsigned short port_read(unsigned short index)
{
    io_out16(0x1CE, index);
    return io_in16(0x1CF);
}

static void port_write(unsigned short index, unsigned short value)
{
    io_out16(0x1CE, index);
    io_out16(0x1CF, value);
}

static const DisplayRegs PORT_REGS = {port_read, port_write, 0};
#endif

static unsigned short reg_read(unsigned short index)
{
    return g_regs ? g_regs->read(index) : 0xFFFF;
}

static void reg_write(unsigned short index, unsigned short value)
{
    if (g_regs)
        g_regs->write(index, value);
}

/* Bochs VBE: QEMU "std" VGA (1234:1111) and VirtualBox VGA (80EE:BEEF) */
static int is_bochs_id(unsigned short vendor, unsigned short device)
{
    return (vendor == 0x1234 && device == 0x1111) || (vendor == 0x80EE && device == 0xBEEF);
}

/* Address of memory BAR n (0..5) of a function; 64-bit BARs take two slots. 0 for an I/O BAR or an unused one. */
static unsigned long long bar_base(const PciDevice *d, int n)
{
    unsigned lo = g_cfg(d->bus, d->dev, d->fn, 0x10 + 4 * (unsigned)n);
    if (lo == 0 || lo == 0xFFFFFFFFu || (lo & 1))
        return 0;
    unsigned long long base = lo & 0xFFFFFFF0u;
    if (((lo >> 1) & 3) == 2 && n < 5)
        base |= (unsigned long long)g_cfg(d->bus, d->dev, d->fn, 0x14 + 4 * (unsigned)n) << 32;
    return base;
}

/* Reads the card's limits and current mode. Returns -1 if the id register is not a DISPI one. */
static int bochs_probe(DisplayAdapter *a)
{
    unsigned id = reg_read(DISPI_ID);
    if (id < DISPI_ID_MIN || id > DISPI_ID_MAX)
        return -1;
    unsigned short en = reg_read(DISPI_ENABLE);
    a->vram_bytes = (unsigned)reg_read(DISPI_VIDEO_MEMORY_64K) * 65536u;

    /* The limits show up in the size registers while GETCAPS is set. */
    reg_write(DISPI_ENABLE, (unsigned short)(en | DISPI_GETCAPS));
    a->max_w = reg_read(DISPI_XRES);
    a->max_h = reg_read(DISPI_YRES);
    a->max_bpp = reg_read(DISPI_BPP);
    reg_write(DISPI_ENABLE, en);

    if (en & DISPI_ENABLED) {
        a->cur_w = reg_read(DISPI_XRES);
        a->cur_h = reg_read(DISPI_YRES);
        a->cur_bpp = reg_read(DISPI_BPP);
    } else {
        a->cur_w = a->cur_h = a->cur_bpp = 0; /* the firmware has not used the extended mode */
    }
    return 0;
}

int display_init(unsigned long long fb_addr, const DisplayRegs *regs)
{
    g_count = 0;
    g_regs = regs;
    g_cfg = (regs && regs->cfg) ? regs->cfg : pci_cfg_read;
#if defined(__x86_64__) || defined(__i386__)
    if (!g_regs)
        g_regs = &PORT_REGS;
#endif
    for (int i = 0; i < pci_count() && g_count < DISPLAY_MAX; i++) {
        const PciDevice *d = pci_get(i);
        if (d->class_code != 0x03)
            continue;
        DisplayAdapter *a = &g_ad[g_count++];
        a->bus = d->bus;
        a->dev = d->dev;
        a->fn = d->fn;
        a->vendor_id = d->vendor_id;
        a->device_id = d->device_id;
        a->kind = DISP_BASIC;
        a->fb_base = bar_base(d, 0);
        a->mmio_base = 0;
        a->vram_bytes = a->cur_w = a->cur_h = a->cur_bpp = a->max_w = a->max_h = a->max_bpp = 0;
        /* The firmware framebuffer lies inside BAR0 (it can start a little past the BAR base). */
        a->active = fb_addr && a->fb_base && fb_addr >= a->fb_base && fb_addr - a->fb_base < (256ull << 20);
        if (is_bochs_id(d->vendor_id, d->device_id) && g_regs && bochs_probe(a) == 0) {
            a->kind = DISP_BOCHS;
            a->mmio_base = bar_base(d, 2);
        }
    }
    return g_count;
}

int display_count(void)
{
    return g_count;
}

const DisplayAdapter *display_get(int index)
{
    return (index >= 0 && index < g_count) ? &g_ad[index] : (const DisplayAdapter *)0;
}

int display_set_mode(int index, unsigned w, unsigned h)
{
    if (index < 0 || index >= g_count)
        return -1;
    DisplayAdapter *a = &g_ad[index];
    if (a->kind != DISP_BOCHS || !g_regs)
        return -1;
    /* Width stays a multiple of 8 (the card scans out in 8-pixel units); 32 bpp must fit in video memory. */
    if (w < DISPI_MIN_SIZE || h < DISPI_MIN_SIZE || (w & 7) || w > a->max_w || h > a->max_h || a->max_bpp < 32)
        return -1;
    if (a->vram_bytes && (unsigned long long)w * h * 4 > a->vram_bytes)
        return -1;

    reg_write(DISPI_ENABLE, 0); /* the size registers can only be changed while the mode is off */
    reg_write(DISPI_BPP, 32);
    reg_write(DISPI_XRES, (unsigned short)w);
    reg_write(DISPI_YRES, (unsigned short)h);
    reg_write(DISPI_BANK, 0);
    reg_write(DISPI_VIRT_WIDTH, (unsigned short)w);
    reg_write(DISPI_X_OFFSET, 0);
    reg_write(DISPI_Y_OFFSET, 0);
    reg_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);

    if (reg_read(DISPI_XRES) != w || reg_read(DISPI_YRES) != h || reg_read(DISPI_BPP) != 32 ||
        !(reg_read(DISPI_ENABLE) & DISPI_ENABLED))
        return -1;
    a->cur_w = w;
    a->cur_h = h;
    a->cur_bpp = 32;
    return 0;
}

const char *display_driver_name(DisplayKind kind)
{
    return kind == DISP_BOCHS ? "Bochs VBE display driver" : "Basic display driver (firmware framebuffer)";
}
