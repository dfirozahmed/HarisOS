/* ============================================================
 * I2C-HID touchpad (the pointing device of most laptops made since about 2015), polled.
 *
 *   i2c_hid_init()   once, after acpi_init(): find the touchpad in the ACPI tables, talk to it
 *                    over its I2C controller, read its HID report descriptor and take the
 *                    mouse part of it
 *   i2c_hid_poll()   often: pointer movement and buttons
 *
 * The touchpad is used in its default "mouse mode" (relative movement, click buttons, taps
 * handled by the touchpad itself), not as a precision touchpad with gestures.
 *
 * Supported: DesignWare I2C controllers (AMD AMDI0010, Intel LPSS). The interrupt line is read
 * through the AMD GPIO controller when there is one, otherwise the device is polled.
 * ============================================================ */
#ifndef HARIS_I2C_HID_H
#define HARIS_I2C_HID_H

/* Returns 0 when a touchpad was found and started, -1 otherwise. */
int i2c_hid_init(void);

/* Adds the movement since the last call to *dx, *dy, *wheel (wheel: + = down, like PS/2) and
 * stores the held buttons (MOUSE_* bits) in *buttons. Returns 1 while a touchpad is active. */
int i2c_hid_poll(int *dx, int *dy, int *wheel, unsigned *buttons);

/* What i2c_hid_init() found in the ACPI tables, for the Device Manager. */
typedef struct
{
    unsigned addr;  /* 7-bit I2C address */
    unsigned speed; /* bus speed in Hz the firmware asks for */
    char ctrl[5];   /* ACPI name of the I2C controller, e.g. "I2CD" */
    int started;    /* 1 = this is the touchpad the driver is using */
} I2cHidInfo;

/* Number of I2C-HID devices the firmware describes (0 before i2c_hid_init) and the i-th one's info.
 * i2c_hid_info() returns 0 on success, -1 if i is out of range. */
int i2c_hid_count(void);
int i2c_hid_info(int i, I2cHidInfo *out);

#endif
