/* xHCI: stubs for builds that are not x86-64 (no PCI ports). */
#include "usb.h"

#if !defined(__x86_64__)
/* not x86-64: no PCI ports */

void usb_hid_counts(int *mice, int *keyboards) { *mice = *keyboards = 0; }
int usb_mouse_info(int index, int *report_protocol, int *has_wheel)
{
    (void)index; (void)report_protocol; (void)has_wheel;
    return -1;
}
int usb_init(void) { return -1; }
int usb_kbd_pop(unsigned char report[8])
{
    (void)report;
    return 0;
}
int usb_poll(int *dx, int *dy, int *wheel, unsigned *buttons)
{
    (void)dx; (void)dy; (void)wheel;
    *buttons = 0;
    return 0;
}

#endif
