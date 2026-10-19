/* ============================================================
 * USB mouse and keyboard through an xHCI host controller (USB 3 / modern USB 2 ports).
 * Polled like the rest of the kernel: no interrupts.
 *
 *   usb_init()      once: find controllers on the PCI bus, start them, look for devices
 *   usb_poll()      often: mouse movement, and (about once a second) looks for newly plugged devices
 *   usb_kbd_pop()   keyboard reports (decoded by keyboard.c)
 *
 * Only HID boot-protocol devices that sit directly on a root port are supported
 * (no hubs yet). Old EHCI / UHCI / OHCI controllers are not supported.
 * ============================================================ */
#ifndef HARIS_USB_H
#define HARIS_USB_H

/* Returns 0 when at least one xHCI controller is running, -1 otherwise.
 * A mouse does not have to be plugged in yet. */
int usb_init(void);

/* Adds the movement since the last call to *dx, *dy, *wheel (wheel: + = down, like PS/2)
 * and stores the held buttons (MOUSE_* bits) in *buttons.
 * Returns 1 while a USB mouse is attached, else 0 (then *buttons is 0). */
int usb_poll(int *dx, int *dy, int *wheel, unsigned *buttons);

/* Next HID boot-protocol keyboard report (8 bytes: modifiers, reserved, six key codes)
 * from any USB keyboard. Returns 1 if there was one, 0 if not. Never blocks. */
int usb_kbd_pop(unsigned char report[8]);

#endif
