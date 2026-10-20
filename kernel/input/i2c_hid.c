/* I2C-HID touchpad. See include/i2c_hid.h and the "HID over I2C" protocol specification. */
#include "i2c_hid.h"
#include "acpi.h"
#include "aml_scan.h"
#include "hid_desc.h"
#include "i2c_dw.h"
#include "io.h"
#include "timer.h"

#if defined(__x86_64__)

#define MAX_CAND 8
#define RDESC_MAX 4096
#define MAX_INPUT 256

static struct
{
    int active;
    DwI2c bus;
    HidMouse mouse;
    unsigned max_in;           /* wMaxInputLength */
    unsigned cmd_reg;
    volatile unsigned *gpio;   /* pad register of the interrupt pin, 0 = unknown */
    int gpio_low, gpio_miss;
    unsigned long long last, last_full;
    unsigned spin, errs;
    int dx, dy, wheel;
    unsigned buttons;
} S;

static unsigned char RDESC[RDESC_MAX];
static unsigned char RBUF[MAX_INPUT];

static void udelay(unsigned us) /* a read of port 0x80 takes about 1 us */
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}

static unsigned le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* ---- PCI (for controllers that are PCI devices: Intel serial I/O) ---- */

static unsigned pci_rd(unsigned dev, unsigned fn, unsigned off)
{
    io_out32(0xCF8, 0x80000000u | (dev << 11) | (fn << 8) | (off & 0xFC));
    return io_in32(0xCFC);
}

static void pci_wr(unsigned dev, unsigned fn, unsigned off, unsigned v)
{
    io_out32(0xCF8, 0x80000000u | (dev << 11) | (fn << 8) | (off & 0xFC));
    io_out32(0xCFC, v);
}

/* BAR 0 of a device on bus 0; also switches on memory decoding. 0 if there is no such device. */
static unsigned long long pci_bar0(unsigned adr, unsigned *vendor)
{
    unsigned dev = (adr >> 16) & 31, fn = adr & 7;
    unsigned id = pci_rd(dev, fn, 0);
    if ((id & 0xFFFF) == 0xFFFF)
        return 0;
    *vendor = id & 0xFFFF;
    pci_wr(dev, fn, 4, (pci_rd(dev, fn, 4) & 0xFFFF) | 6);
    unsigned bar = pci_rd(dev, fn, 0x10);
    if (bar & 1)
        return 0;
    unsigned long long base = bar & ~0xFULL;
    if (((bar >> 1) & 3) == 2)
        base |= (unsigned long long)pci_rd(dev, fn, 0x14) << 32;
    return base;
}

/* ---- ACPI lookups ---- */

static int find_device(const char *name4, AmlDevice *d)
{
    for (int i = 0; i < acpi_aml_count(); i++)
    {
        unsigned len;
        const unsigned char *t = acpi_aml(i, &len);
        if (t && aml_find_device(t, len, name4, d))
            return 1;
    }
    return 0;
}

/* ---- the HID over I2C protocol ---- */

/* Reads `n` bytes from device register `reg` */
static int read_reg(unsigned reg, unsigned char *out, unsigned n)
{
    unsigned char w[2] = {(unsigned char)reg, (unsigned char)(reg >> 8)};
    return dw_i2c_xfer(&S.bus, w, 2, out, n);
}

static int send_cmd(unsigned low, unsigned opcode)
{
    unsigned char w[4] = {(unsigned char)S.cmd_reg, (unsigned char)(S.cmd_reg >> 8), (unsigned char)low,
                          (unsigned char)opcode};
    return dw_i2c_xfer(&S.bus, w, 4, 0, 0);
}

/* The 30-byte HID descriptor at `reg`: returns 0 if it looks like one. */
static int read_hid_desc(unsigned reg, unsigned char d[30])
{
    if (read_reg(reg, d, 30))
        return -1;
    if (le16(d) != 30 || le16(d + 2) != 0x0100) /* wHIDDescLength, bcdVersion */
        return -1;
    unsigned rdlen = le16(d + 4), rdreg = le16(d + 6), maxin = le16(d + 10);
    if (rdlen < 16 || rdlen > RDESC_MAX || rdreg == 0 || maxin < 3 || le16(d + 16) == 0)
        return -1;
    return 0;
}

static int bring_up(const AmlI2cHid *c)
{
    AmlDevice cd;
    if (!find_device(c->ctrl, &cd))
        return -1;

    unsigned long long base = 0;
    unsigned vendor = 0;
    if (cd.has_mem)
        base = cd.base;
    else if (cd.has_adr)
        base = pci_bar0(cd.adr, &vendor);
    if (!base)
        return -1;

    unsigned clk = str_eq(cd.hid, "AMDI0010") ? 150000000u : 133000000u; /* controller input clock */
    unsigned hz = c->speed >= 100000 && c->speed <= 400000 ? c->speed : 400000;
    if (clk != 150000000u && hz > 300000) /* clock not known for sure: stay well below 400 kHz */
        hz = 300000;

    if (dw_i2c_probe(&S.bus, base))
    {
        if (vendor != 0x8086)
            return -1;
        *(volatile unsigned *)(base + 0x204) = 3; /* Intel LPSS: take the controller out of reset */
        udelay(1000);
        if (dw_i2c_probe(&S.bus, base))
            return -1;
    }
    if (dw_i2c_setup(&S.bus, c->addr, clk, hz))
        return -1;

    /* the register of the HID descriptor: what the _DSM says, then the usual suspects */
    unsigned regs[AML_MAX_REGS + 2], nr = 0;
    for (int i = 0; i < c->nregs; i++)
        regs[nr++] = c->regs[i];
    regs[nr++] = 1;
    regs[nr++] = 0x20;
    unsigned char d[30];
    int ok = 0;
    for (unsigned i = 0; i < nr && !ok; i++)
        ok = read_hid_desc(regs[i], d) == 0;
    if (!ok)
        return -1;

    unsigned rdlen = le16(d + 4), rdreg = le16(d + 6);
    S.max_in = le16(d + 10) > MAX_INPUT ? MAX_INPUT : le16(d + 10);
    S.cmd_reg = le16(d + 16);

    send_cmd(0x00, 0x08); /* SET_POWER: on */
    send_cmd(0x00, 0x01); /* RESET */
    for (int i = 0; i < 50; i++) /* it answers with an empty report (length 0) when it is ready */
    {
        udelay(10000);
        unsigned char b[2];
        if (dw_i2c_xfer(&S.bus, 0, 0, b, 2) == 0 && b[0] == 0 && b[1] == 0)
            break;
    }

    if (read_reg(rdreg, RDESC, rdlen))
        return -1;
    if (!hid_find_mouse(RDESC, rdlen, &S.mouse)) /* a touchscreen, or a pad without a mouse mode */
        return -1;

    /* interrupt line through the AMD GPIO controller, when we know how */
    S.gpio = 0;
    S.gpio_miss = 0;
    if (c->has_gpio)
    {
        AmlDevice gd;
        if (find_device(c->gpio_ctrl, &gd) && gd.has_mem && (str_eq(gd.hid, "AMDI0030") || str_eq(gd.hid, "AMD0030")) &&
            c->gpio_pin < gd.size / 4)
        {
            volatile unsigned *reg = (volatile unsigned *)(gd.base + 4ULL * c->gpio_pin);
            if (*reg != 0xFFFFFFFFu)
            {
                S.gpio = reg;
                S.gpio_low = c->gpio_active_low;
            }
        }
    }
    return 0;
}

int i2c_hid_init(void)
{
    S.active = 0;
    AmlI2cHid cand[MAX_CAND];
    int nc = 0;
    for (int i = 0; i < acpi_aml_count() && nc < MAX_CAND; i++)
    {
        unsigned len;
        const unsigned char *t = acpi_aml(i, &len);
        if (t)
            nc += aml_find_i2c_hid(t, len, cand + nc, MAX_CAND - nc);
    }
    for (int i = 0; i < nc; i++)
        if (bring_up(&cand[i]) == 0)
        {
            S.dx = S.dy = S.wheel = 0;
            S.buttons = 0;
            S.errs = 0;
            S.last = S.last_full = 0;
            S.active = 1;
            return 0;
        }
    return -1;
}

/* the interrupt pin: is the touchpad holding data for us? (AMD pad register: bit 16 = pin level) */
static int irq_asserted(void)
{
    int level = (int)((*S.gpio >> 16) & 1);
    return S.gpio_low ? !level : level;
}

/* One report from the touchpad. Returns 1 if it carried data. */
static int read_report(void)
{
    if (dw_i2c_xfer(&S.bus, 0, 0, RBUF, S.max_in))
    {
        if (++S.errs > 300)
            S.active = 0;
        return 0;
    }
    S.errs = 0;
    unsigned len = le16(RBUF);
    if (len < 3 || len > S.max_in)
        return 0;
    unsigned at = 2;
    if (S.mouse.uses_ids)
    {
        if ((int)RBUF[2] != S.mouse.report_id)
            return 1; /* some other report (not the mouse one) */
        at = 3;
    }
    const unsigned char *data = RBUF + at;
    unsigned dlen = len - at;
    const HidMouse *m = &S.mouse;
    S.buttons = (unsigned)hid_value(data, dlen, m->btn_off, m->btn_n > 3 ? 3 : m->btn_n, 0);
    S.dx += (int)hid_value(data, dlen, m->x_off, m->x_size, m->x_signed);
    S.dy += (int)hid_value(data, dlen, m->y_off, m->y_size, m->y_signed);
    if (m->has_wheel)
        S.wheel -= (int)hid_value(data, dlen, m->w_off, m->w_size, m->w_signed); /* HID: + = up */
    return 1;
}

int i2c_hid_poll(int *dx, int *dy, int *wheel, unsigned *buttons)
{
    if (!S.active)
    {
        *buttons = 0;
        return 0;
    }
    unsigned long long now = timer_ready() ? timer_ms() : 0;
    int due = 0, sanity = 0;
    if (S.gpio)
    {
        due = irq_asserted();
        if (!due && now - S.last_full >= 250) /* now and then ask anyway: is the pin reading right? */
            due = sanity = 1;
    }
    else if (timer_ready())
        due = now - S.last >= 8;
    else
        due = (++S.spin & 63) == 0;

    if (due)
    {
        S.last = now;
        if (!S.gpio || sanity)
            S.last_full = now;
        int got = read_report();
        if (sanity && got && ++S.gpio_miss >= 3)
            S.gpio = 0; /* it had data while the pin said "nothing": do not trust the pin */
        if (S.gpio && !sanity)
            S.gpio_miss = 0;
    }

    *dx += S.dx;
    *dy += S.dy;
    *wheel += S.wheel;
    S.dx = S.dy = S.wheel = 0;
    *buttons = S.buttons;
    return S.active;
}

#else

int i2c_hid_init(void) { return -1; }
int i2c_hid_poll(int *dx, int *dy, int *wheel, unsigned *buttons)
{
    (void)dx; (void)dy; (void)wheel;
    *buttons = 0;
    return 0;
}

#endif
