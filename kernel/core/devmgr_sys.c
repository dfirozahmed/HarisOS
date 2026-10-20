/* Device Manager glue: collects what PCI and the input drivers know right now.
 * Kept apart from devmgr.c so that file stays free of drivers and can be tested on the host. */
#include "devmgr.h"
#include "i2c_hid.h"
#include "mouse.h"
#include "pci.h"
#include "usb.h"

static int KEYBOARD_OK;

int devmgr_refresh(void)
{
    pci_scan();

    /* No "= {0}": the struct is big and the compiler would call memset (no libc here). */
    DevmgrInputs in;
    in.keyboard_ps2 = KEYBOARD_OK;
    in.mouse_sources = mouse_sources();
    in.usb_running = (mouse_sources() & MOUSE_SRC_USB) != 0;
    usb_hid_counts(&in.usb_mice, &in.usb_keyboards);
    for (int i = 0; i < DEVMGR_MAX_USB_MICE; i++)
    {
        in.usb_mouse_rp[i] = in.usb_mouse_wheel[i] = 0;
        if (i < in.usb_mice)
            usb_mouse_info(i, &in.usb_mouse_rp[i], &in.usb_mouse_wheel[i]);
    }
    in.i2c_count = 0;
    for (int i = 0; i < i2c_hid_count() && in.i2c_count < DEVMGR_MAX_I2C; i++)
    {
        I2cHidInfo hi;
        if (i2c_hid_info(i, &hi))
            break;
        DevmgrI2c *d = &in.i2c[in.i2c_count++];
        d->addr = hi.addr;
        d->speed = hi.speed;
        for (int j = 0; j < 5; j++)
            d->ctrl[j] = hi.ctrl[j];
        d->started = hi.started;
        d->has_wheel = hi.has_wheel;
        d->ptp_fingers = hi.ptp_fingers;
        d->ptp_mode = hi.ptp_mode;
        d->reason = i2c_hid_error_text(hi.error);
    }
    return devmgr_build(&in);
}

int devmgr_init(int keyboard_ok)
{
    KEYBOARD_OK = keyboard_ok;
    return devmgr_refresh();
}
