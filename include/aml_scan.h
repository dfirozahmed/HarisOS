/* ============================================================
 * Looking things up in ACPI AML (the DSDT / SSDT byte code) WITHOUT an AML interpreter.
 *
 * The firmware describes an I2C-HID touchpad as a Device with _CID "PNP0C50" (or _HID
 * "ACPI0C50"), whose _CRS holds an I2cSerialBus descriptor (address + the controller it sits
 * on) and a GpioInt descriptor (its interrupt pin), and whose _DSM returns the register the
 * HID descriptor lives at. All of that is plain data in the byte code, so we find it by its
 * byte pattern. This is a heuristic, not a parser: anything it returns must be verified
 * against the real hardware (i2c_hid.c does that by reading the HID descriptor).
 *
 * Pure functions, no hardware access: tested on the host (tests/test_i2chid.c).
 * ============================================================ */
#ifndef HARIS_AML_SCAN_H
#define HARIS_AML_SCAN_H

#define AML_MAX_REGS 6

typedef struct
{
    unsigned addr;               /* 7-bit I2C address */
    unsigned speed;              /* bus speed in Hz the firmware asks for */
    char ctrl[5];                /* name of the I2C controller device, e.g. "I2CD" */
    int has_gpio;                /* the interrupt pin is described */
    unsigned gpio_pin;           /* pin number on that GPIO controller */
    int gpio_active_low;         /* the line is low while the device has data */
    char gpio_ctrl[5];           /* name of the GPIO controller device, e.g. "GPIO" */
    unsigned regs[AML_MAX_REGS]; /* candidate registers of the HID descriptor, most likely first */
    int nregs;
} AmlI2cHid;

/* Finds I2C-HID devices in one AML table. Returns how many were written to out[]. */
int aml_find_i2c_hid(const unsigned char *aml, unsigned len, AmlI2cHid *out, int max);

typedef struct
{
    char hid[9];                 /* _HID string, "" if none */
    int has_mem;                 /* a Memory32Fixed range is part of its _CRS */
    unsigned long long base;
    unsigned size;
    int has_adr;                 /* _ADR is present: a PCI device (device << 16 | function) */
    unsigned adr;
} AmlDevice;

/* Looks up the Device named name4 (four characters) in one AML table. Returns 1 if found. */
int aml_find_device(const unsigned char *aml, unsigned len, const char *name4, AmlDevice *out);

#endif
