/* Device Manager glue: collects what PCI and the input drivers know right now.
 * Kept apart from devmgr.c so that file stays free of drivers and can be tested on the host. */
#include "blk.h"
#include "devmgr.h"
#include "display.h"
#include "i2c_hid.h"
#include "mouse.h"
#include "net.h"
#include "pci.h"
#include "storage.h"
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
    in.storage_running = storage_controllers_started();
    in.disk_count = 0;
    for (int i = 0; i < blk_count() && in.disk_count < DEVMGR_MAX_DISKS; i++)
    {
        const BlkDev *b = blk_get(i);
        if (b->parent >= 0 || b->removed)
            continue; /* partitions are counted on their disk; an unplugged USB drive is not there any more */
        DevmgrDisk *d = &in.disks[in.disk_count++];
        for (int j = 0; j < (int)sizeof d->name; j++)
            d->name[j] = b->name[j];
        for (int j = 0; j < (int)sizeof d->model; j++)
            d->model[j] = b->model[j];
        d->bytes = b->sectors * b->sector_size;
        d->kind = b->kind;
        d->locked = b->locked;
        d->parts = 0;
        for (int j = 0; j < blk_count(); j++)
            if (blk_get(j)->parent == i && !blk_get(j)->removed)
                d->parts++;
    }
    in.display_count = 0;
    for (int i = 0; i < display_count() && in.display_count < DEVMGR_MAX_DISPLAYS; i++)
    {
        const DisplayAdapter *a = display_get(i);
        DevmgrDisplay *d = &in.displays[in.display_count++];
        d->bus = a->bus;
        d->dev = a->dev;
        d->fn = a->fn;
        d->native = a->kind == DISP_BOCHS;
        d->active = a->active;
        d->cur_w = a->cur_w;
        d->cur_h = a->cur_h;
        d->cur_bpp = a->cur_bpp;
        d->vram_mb = a->vram_bytes >> 20;
    }
    {
        unsigned char loc[3] = {0, 0, 0};
        in.net_found = net_adapter(loc, &in.net_working, in.net_status, sizeof in.net_status);
        in.net_bus = loc[0];
        in.net_dev = loc[1];
        in.net_fn = loc[2];
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
