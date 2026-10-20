/* PCI enumeration: walks every bus/device/function and records what answers.
 * A function that does not exist reads vendor id 0xFFFF. */
#include "pci.h"
#include "io.h"

static PciDevice g_devs[PCI_MAX_DEVICES];
static int g_count;

#if defined(__x86_64__) || defined(__i386__)
static unsigned int port_read(unsigned bus, unsigned dev, unsigned fn, unsigned off)
{
    io_out32(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return io_in32(0xCFC);
}
#endif

static void add_function(PciReadFn rd, unsigned bus, unsigned dev, unsigned fn)
{
    if (g_count >= PCI_MAX_DEVICES)
        return;
    unsigned int id = rd(bus, dev, fn, 0x00);
    unsigned int cls = rd(bus, dev, fn, 0x08);
    unsigned int hdr = rd(bus, dev, fn, 0x0C);
    PciDevice *d = &g_devs[g_count++];
    d->bus = (unsigned char)bus;
    d->dev = (unsigned char)dev;
    d->fn = (unsigned char)fn;
    d->vendor_id = (unsigned short)(id & 0xFFFF);
    d->device_id = (unsigned short)(id >> 16);
    d->revision = (unsigned char)(cls & 0xFF);
    d->prog_if = (unsigned char)((cls >> 8) & 0xFF);
    d->subclass = (unsigned char)((cls >> 16) & 0xFF);
    d->class_code = (unsigned char)(cls >> 24);
    d->header_type = (unsigned char)((hdr >> 16) & 0xFF);
}

int pci_scan_with(PciReadFn rd)
{
    g_count = 0;
    if (!rd)
        return 0;
    for (unsigned bus = 0; bus < 256; bus++) {
        for (unsigned dev = 0; dev < 32; dev++) {
            if ((rd(bus, dev, 0, 0) & 0xFFFF) == 0xFFFF)
                continue;
            /* Header type bit 7 = multi-function: only then check functions 1..7. */
            unsigned nfn = (rd(bus, dev, 0, 0x0C) & 0x800000) ? 8 : 1;
            for (unsigned fn = 0; fn < nfn; fn++) {
                if ((rd(bus, dev, fn, 0) & 0xFFFF) == 0xFFFF)
                    continue;
                add_function(rd, bus, dev, fn);
            }
        }
    }
    return g_count;
}

int pci_scan(void)
{
#if defined(__x86_64__) || defined(__i386__)
    return pci_scan_with(port_read);
#else
    g_count = 0;
    return 0;
#endif
}

int pci_count(void)
{
    return g_count;
}

const PciDevice *pci_get(int index)
{
    return (index >= 0 && index < g_count) ? &g_devs[index] : (const PciDevice *)0;
}

const char *pci_class_name(unsigned char c, unsigned char s)
{
    switch (c) {
    case 0x01:
        switch (s) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default: return "Storage controller";
        }
    case 0x02: return "Network controller";
    case 0x03: return "Display controller";
    case 0x04: return (s == 0x03) ? "Audio device" : "Multimedia device";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        default: return "Bridge";
        }
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device controller";
    case 0x0C:
        switch (s) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default: return "Serial bus controller";
        }
    case 0x0D: return "Wireless controller";
    default: return "Unknown device";
    }
}

const char *pci_vendor_name(unsigned short v)
{
    switch (v) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD (ATI)";
    case 0x10DE: return "NVIDIA";
    case 0x10EC: return "Realtek";
    case 0x14E4: return "Broadcom";
    case 0x168C: return "Qualcomm Atheros";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x1234: return "QEMU";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x15AD: return "VMware";
    case 0x144D: return "Samsung";
    default: return "Unknown vendor";
    }
}

/* ---- config space access for drivers --------------------------- */
unsigned int pci_cfg_read(unsigned bus, unsigned dev, unsigned fn, unsigned off)
{
#if defined(__x86_64__) || defined(__i386__)
    return port_read(bus, dev, fn, off);
#else
    (void)bus; (void)dev; (void)fn; (void)off;
    return 0xFFFFFFFFu;
#endif
}

void pci_cfg_write(unsigned bus, unsigned dev, unsigned fn, unsigned off, unsigned int value)
{
#if defined(__x86_64__) || defined(__i386__)
    io_out32(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    io_out32(0xCFC, value);
#else
    (void)bus; (void)dev; (void)fn; (void)off; (void)value;
#endif
}
