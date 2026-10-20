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

#endif
