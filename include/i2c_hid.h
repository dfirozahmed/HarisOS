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
    int error;      /* I2C_HID_ERR_*: why it was not started (0 when started or not tried) */
} I2cHidInfo;

/* Where bring-up of one described device stopped. */
enum
{
    I2C_HID_OK = 0,
    I2C_HID_ERR_NO_CONTROLLER, /* the I2C controller named by the firmware is not in the ACPI tables */
    I2C_HID_ERR_NO_BASE,       /* the controller has no memory range / PCI BAR */
    I2C_HID_ERR_CTRL_PROBE,    /* the controller registers do not look like a DesignWare I2C block */
    I2C_HID_ERR_CTRL_SETUP,    /* the controller refused address / speed setup */
    I2C_HID_ERR_NO_HID_DESC,   /* the device gave no valid HID descriptor at any candidate register */
    I2C_HID_ERR_RDESC_READ,    /* the HID report descriptor could not be read */
    I2C_HID_ERR_NO_MOUSE       /* it is not a mouse-mode pointing device (touchscreen, precision-only pad) */
};

/* Short English text for an I2C_HID_ERR_* value ("" for I2C_HID_OK). */
const char *i2c_hid_error_text(int error);

/* Number of I2C-HID devices the firmware describes (0 before i2c_hid_init) and the i-th one's info.
 * i2c_hid_info() returns 0 on success, -1 if i is out of range. */
int i2c_hid_count(void);
int i2c_hid_info(int i, I2cHidInfo *out);

#endif
