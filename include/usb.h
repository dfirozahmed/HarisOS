/* ============================================================
 * USB mouse and keyboard through an xHCI host controller (USB 3 / modern USB 2 ports).
 * Polled like the rest of the kernel: no interrupts.
 *
 *   usb_init()      once: find controllers on the PCI bus, start them, look for devices
 *   usb_poll()      often: mouse movement, and (about once a second) looks for newly plugged devices
 *   usb_kbd_pop()   keyboard reports (decoded by keyboard.c)
 *
 * HID mice / keyboards and mass-storage drives (pen drives, external disks: bulk-only transport, see
 * usb_msc.h) are supported, also behind hubs. A mass-storage drive becomes a block device ("usb0") when it is
 * plugged in and disappears when it is pulled out. Old EHCI / UHCI / OHCI controllers and UAS are not supported.
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

/* How many HID boot-protocol mice / keyboards are attached right now (for the Device Manager). */
void usb_hid_counts(int *mice, int *keyboards);

/* About the i-th attached mouse (0-based, same order as the Device Manager lists them):
 * *report_protocol = 1 when its report descriptor was read (fields decoded from it), 0 = boot protocol
 * (3-byte reports: no wheel); *has_wheel = 1 when the descriptor has a wheel. Returns -1 if no such mouse. */
int usb_mouse_info(int index, int *report_protocol, int *has_wheel);

#endif
