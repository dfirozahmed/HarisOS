/* USB mass storage (pen drives, external disks): the SCSI commands inside the Bulk-Only Transport.
 *
 * This file knows nothing about USB controllers. The xHCI driver (usb_xhci.c) hands in a few functions that move
 * bytes over the two bulk endpoints (UsbMscIo); usb_msc.c builds the command / status wrappers (CBW / CSW) and the
 * SCSI commands (INQUIRY, READ CAPACITY, READ, WRITE ...), and registers the drive as a block device ("usb0").
 * The file systems then see it like any other disk (blk.h). */
#ifndef HARIS_USB_MSC_H
#define HARIS_USB_MSC_H

#include "blk.h"

/* results of a transport function */
#define MSC_OK 0
#define MSC_STALL 1 /* the endpoint answered STALL (the driver has already reset the endpoint) */
#define MSC_ERR (-1)

typedef struct
{
    /* Send / receive `len` bytes on the bulk OUT / IN endpoint. `len` is at most max_xfer. */
    int (*bulk_out)(void *ctx, const void *buf, unsigned len);
    int (*bulk_in)(void *ctx, void *buf, unsigned len, unsigned *got);
    /* CLEAR_FEATURE(ENDPOINT_HALT) on the IN (1) or OUT (0) endpoint, and reset the controller's endpoint state. */
    int (*clear_halt)(void *ctx, int in);
    /* Bulk-Only Mass Storage Reset, then clear both halts ("reset recovery"). */
    int (*reset)(void *ctx);
    void (*delay_ms)(void *ctx, unsigned ms);
    int (*alive)(void *ctx); /* 0 once the device has been unplugged: stop trying */
    unsigned max_xfer;       /* most bytes one bulk_in / bulk_out call can move (a multiple of 4096) */
    void *ctx;
} UsbMscIo;

typedef struct UsbMsc
{
    UsbMscIo io;
    unsigned tag;
    unsigned sector_size;
    unsigned long long sectors;
    int read_only; /* the drive says it is write protected (a SD card's lock switch ...) */
    int blk;       /* index of the block device, -1 until usb_msc_attach() succeeded */
    char vendor[9], product[17];
} UsbMsc;

/* Talk to the drive: INQUIRY, wait until it is ready, READ CAPACITY. Fills in everything but `blk`.
 * Returns 0, or -1 (not a disk, no medium, would not get ready ...). */
int usb_msc_probe(UsbMsc *m, const UsbMscIo *io);

/* usb_msc_probe(), then register the drive as block device "usb<n>" and read its partition table.
 * Returns the block device index or -1. `m` must stay valid until usb_msc_detach(). */
int usb_msc_attach(UsbMsc *m, const UsbMscIo *io);

/* The drive was unplugged: its block device (and partitions) fail from now on. See blk_remove(). */
void usb_msc_detach(UsbMsc *m);

#endif
