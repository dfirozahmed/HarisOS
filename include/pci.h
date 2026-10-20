/* PCI device list for the Device Manager. Scans bus 0..255 through the legacy
 * config ports (0xCF8 / 0xCFC) and keeps the result in a fixed table. */
#ifndef HARIS_PCI_H
#define HARIS_PCI_H

#define PCI_MAX_DEVICES 64

typedef struct {
    unsigned char bus, dev, fn;
    unsigned short vendor_id, device_id;
    unsigned char class_code, subclass, prog_if, revision;
    unsigned char header_type;
} PciDevice;

/* Reads one 32-bit config dword (offset is a multiple of 4). */
typedef unsigned int (*PciReadFn)(unsigned bus, unsigned dev, unsigned fn, unsigned off);

/* Scans with a custom reader (host tests use a fake config space).
 * Returns the number of devices stored (at most PCI_MAX_DEVICES). */
int pci_scan_with(PciReadFn rd);

/* Scans real hardware (x86-64 only; returns 0 elsewhere). */
int pci_scan(void);

int pci_count(void);
const PciDevice *pci_get(int index); /* NULL if out of range */

/* Human readable names; never NULL. Unknown ids give "Unknown ...". */
const char *pci_class_name(unsigned char class_code, unsigned char subclass);
const char *pci_vendor_name(unsigned short vendor_id);

#endif
