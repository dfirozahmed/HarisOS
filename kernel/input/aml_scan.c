/* ACPI AML pattern scanner. See include/aml_scan.h. */
#include "aml_scan.h"

static unsigned rd16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }

static int same(const unsigned char *p, const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i] != (unsigned char)s[i])
            return 0;
    return 1;
}

/* The four-character name of the last segment of a path such as "\_SB.PCI0.I2C1". */
static void leaf_name(const unsigned char *path, unsigned max, char out[5])
{
    unsigned n = 0, start = 0;
    while (n < max && path[n])
    {
        if (path[n] == '.')
            start = n + 1;
        n++;
    }
    if (n - start > 4)
        start = n - 4; /* "\_SB_I2C0" style without dots: keep the last four */
    for (int i = 0; i < 4; i++)
        out[i] = start + (unsigned)i < n ? (char)path[start + i] : '_';
    out[4] = 0;
}

static void add_reg(AmlI2cHid *d, unsigned v)
{
    for (int i = 0; i < d->nregs; i++)
        if (d->regs[i] == v)
            return;
    if (d->nregs < AML_MAX_REGS)
        d->regs[d->nregs++] = v;
}

/* GpioInt descriptor (0x8C) right after the I2C one */
static void read_gpio(const unsigned char *p, unsigned len, unsigned at, AmlI2cHid *d)
{
    for (unsigned j = at; j + 26 <= len && j < at + 64; j++)
    {
        if (p[j] != 0x8C)
            continue;
        unsigned dl = rd16(p + j + 1);
        if (dl < 0x14 || dl > 0x80 || p[j + 4] != 0 || j + 3 + dl > len) /* type 0 = interrupt */
            continue;
        unsigned pin_off = rd16(p + j + 14), name_off = rd16(p + j + 17);
        if (pin_off + 2 > dl + 3 || name_off >= dl + 3)
            continue;
        d->has_gpio = 1;
        d->gpio_pin = rd16(p + j + pin_off);
        d->gpio_active_low = ((p[j + 7] >> 1) & 3) == 1; /* polarity: 0 high, 1 low, 2 both */
        leaf_name(p + j + name_off, dl + 3 - name_off, d->gpio_ctrl);
        return;
    }
}

/* Values the _DSM method returns (Return (0x20), Return (One), ...): the HID descriptor register. */
static void read_dsm(const unsigned char *p, unsigned len, unsigned at, AmlI2cHid *d)
{
    static const unsigned char UUID[16] = {0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
                                           0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE};
    for (unsigned u = at; u + 16 <= len && u < at + 1200; u++)
    {
        if (p[u] != UUID[0] || !same(p + u, (const char *)UUID, 16))
            continue;
        for (unsigned k = u + 16; k + 4 <= len && k < u + 16 + 160; k++)
        {
            if (p[k] != 0xA4) /* ReturnOp */
                continue;
            if (p[k + 1] == 0x01)
                add_reg(d, 1);
            else if (p[k + 1] == 0x0A)
                add_reg(d, p[k + 2]);
            else if (p[k + 1] == 0x0B)
                add_reg(d, rd16(p + k + 2));
        }
        return;
    }
}

int aml_find_i2c_hid(const unsigned char *p, unsigned len, AmlI2cHid *out, int max)
{
    int n = 0;
    for (unsigned i = 0; i + 8 <= len && n < max; i++)
    {
        if (!(same(p + i, "PNP0C50", 7) || same(p + i, "ACPI0C50", 8)))
            continue;
        for (unsigned j = i; j + 20 <= len && j < i + 400; j++)
        {
            if (p[j] != 0x8E) /* SerialBus connection descriptor */
                continue;
            unsigned dl = rd16(p + j + 1);
            if (dl < 0x10 || dl > 0x60 || p[j + 3] != 1 || p[j + 5] != 1 || j + 3 + dl > len) /* revision 1, type I2C */
                continue;
            if (p[j + 7] & 1) /* 10-bit addressing: not handled */
                continue;
            AmlI2cHid d;
            d.speed = rd32(p + j + 12);
            d.addr = rd16(p + j + 16);
            leaf_name(p + j + 18, dl + 3 - 18, d.ctrl);
            d.has_gpio = 0;
            d.gpio_pin = 0;
            d.gpio_active_low = 1;
            d.gpio_ctrl[0] = 0;
            d.nregs = 0;
            read_gpio(p, len, j + 3 + dl, &d);
            read_dsm(p, len, j, &d);
            int dup = 0;
            for (int k = 0; k < n; k++)
                if (out[k].addr == d.addr && same((const unsigned char *)out[k].ctrl, d.ctrl, 4))
                    dup = 1; /* the same device shows up through both its _HID and its _CID */
            if (!dup)
                out[n++] = d;
            i = j; /* continue after this descriptor */
            break;
        }
    }
    return n;
}

/* ---- devices by name ---- */

static unsigned pkg_length(const unsigned char *p, unsigned *lead)
{
    unsigned l = p[0] >> 6, v = l ? (p[0] & 0x0F) : (p[0] & 0x3F);
    for (unsigned m = 1; m <= l; m++)
        v |= (unsigned)p[m] << (4 + 8 * (m - 1));
    *lead = l;
    return v;
}

int aml_find_device(const unsigned char *p, unsigned len, const char *name4, AmlDevice *out)
{
    for (unsigned k = 0; k + 12 <= len; k++)
    {
        if (p[k] != 0x5B || p[k + 1] != 0x82) /* DeviceOp */
            continue;
        unsigned lead, pk = pkg_length(p + k + 2, &lead);
        unsigned name = k + 3 + lead;
        if (name + 4 > len || !same(p + name, name4, 4))
            continue;
        unsigned end = k + 2 + pk;
        if (end > len)
            end = len;

        out->hid[0] = 0;
        out->has_mem = out->has_adr = 0;
        out->base = out->adr = out->size = 0;
        for (unsigned b = name + 4; b + 8 < end; b++)
        {
            if (p[b] == 0x08 && same(p + b + 1, "_HID", 4) && p[b + 5] == 0x0D) /* Name (_HID, "...") */
            {
                int i = 0;
                while (i < 8 && b + 6 + (unsigned)i < end && p[b + 6 + i])
                {
                    out->hid[i] = (char)p[b + 6 + i];
                    i++;
                }
                out->hid[i] = 0;
            }
            else if (p[b] == 0x08 && same(p + b + 1, "_ADR", 4))
            {
                unsigned char t = p[b + 5];
                if (t == 0x0C && b + 10 <= end)
                    out->adr = rd32(p + b + 6), out->has_adr = 1;
                else if (t == 0x0B && b + 8 <= end)
                    out->adr = rd16(p + b + 6), out->has_adr = 1;
                else if (t == 0x0A && b + 7 <= end)
                    out->adr = p[b + 6], out->has_adr = 1;
                else if (t == 0x00 || t == 0x01)
                    out->adr = t, out->has_adr = 1;
            }
            else if (p[b] == 0x86 && p[b + 1] == 9 && p[b + 2] == 0 && !out->has_mem && b + 12 <= end) /* Memory32Fixed */
            {
                out->base = rd32(p + b + 4);
                out->size = rd32(p + b + 8);
                out->has_mem = 1;
            }
        }
        return 1;
    }
    return 0;
}
