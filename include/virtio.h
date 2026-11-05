/* virtio over PCI ("modern", virtio 1.0): the transport shared by virtio-net and virtio-blk.
 *
 * virtio is the paravirtual hardware of QEMU / KVM: a device that exists only to be driven by a guest, so it is simple.
 * Everything a device needs is found through vendor specific PCI capabilities, each pointing into a BAR:
 *   common cfg  - status, feature bits, and the settings of each virtqueue
 *   notify      - one doorbell per queue ("the queue has new buffers")
 *   device cfg  - what is specific to the device (the MAC address of a NIC, the capacity of a disk)
 * Data moves through virtqueues: three arrays in RAM that the driver and the device both see:
 *   descriptors - (address, length, flags, next) of each buffer; chained with `next` for a request in several pieces
 *   avail ring  - the driver writes the first descriptor of each request it makes available
 *   used ring   - the device writes the first descriptor of each request it has finished, with the number of bytes it wrote
 * This file does the polled part only (no interrupts). A driver fills the descriptors itself, then virtq_push()es a head and
 * virtq_pop()s finished ones. See kernel/drivers/bus/virtio.c. */
#ifndef HARIS_VIRTIO_H
#define HARIS_VIRTIO_H

#include "pci.h"

#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_DEV_NET_LEGACY 0x1000 /* "transitional" ids: they also have the modern capabilities, QEMU's default */
#define VIRTIO_DEV_BLK_LEGACY 0x1001
#define VIRTIO_DEV_NET 0x1041 /* 0x1040 + device type 1 */
#define VIRTIO_DEV_BLK 0x1042 /* 0x1040 + device type 2 */

/* feature bits (64 bit number: 0..31 in the low half) */
#define VIRTIO_F_VERSION_1 (1ULL << 32) /* the device follows virtio 1.0: required */

/* device status bits */
#define VIRTIO_S_ACK 1u
#define VIRTIO_S_DRIVER 2u
#define VIRTIO_S_DRIVER_OK 4u
#define VIRTIO_S_FEATURES_OK 8u
#define VIRTIO_S_FAILED 128u

/* descriptor flags */
#define VIRTQ_F_NEXT 1u  /* the chain goes on with `next` */
#define VIRTQ_F_WRITE 2u /* the device writes into this buffer (otherwise it only reads it) */

#define VIRTQ_MAX 64 /* most entries per queue we use (one page holds all three arrays up to this size) */

typedef struct
{
    unsigned long long addr;
    unsigned len;
    unsigned short flags, next;
} VirtqDesc;

typedef struct
{
    volatile unsigned char *common, *notify, *isr, *dev; /* the four register windows; isr/dev may be NULL */
    unsigned notify_mult;                                /* bytes between two queues' doorbells */
} VirtioDev;

typedef struct
{
    VirtioDev *v;
    unsigned short index;      /* queue number */
    unsigned short size;       /* entries (power of two, <= VIRTQ_MAX) */
    unsigned short notify_off; /* doorbell of this queue: notify + notify_off * notify_mult */
    unsigned short last_used;  /* how many used entries we have taken so far (wraps at 65536) */
    unsigned short avail_idx;  /* how many heads we have made available (wraps at 65536) */
    VirtqDesc *desc;
    volatile unsigned short *avail; /* [0] flags, [1] idx, [2..] ring */
    volatile unsigned char *used;   /* u16 flags, u16 idx, then {u32 id, u32 len} per entry */
} Virtq;

/* Find the capabilities of a virtio PCI function, turn on memory space and bus mastering. 0 = ok, else -1 and *why says why. */
int virtio_attach(const PciDevice *pd, VirtioDev *v, const char **why);

/* Reset the device and negotiate features: `want` = the features the driver can use (must not include VERSION_1, it is added).
 * `*got` = the ones both sides have. 0 = ok (status FEATURES_OK), else -1 and *why. */
int virtio_negotiate(VirtioDev *v, unsigned long long want, unsigned long long *got, const char **why);

/* Set up queue `index` (page of RAM for the three arrays, at most `max_size` entries). 0 = ok, else -1. */
int virtio_queue_init(VirtioDev *v, Virtq *q, unsigned short index, unsigned short max_size, const char **why);

/* Tell the device the driver is ready (status DRIVER_OK); call after every queue is set up. */
void virtio_driver_ok(VirtioDev *v);
/* Give up: status FAILED. */
void virtio_fail(VirtioDev *v);
unsigned virtio_status(const VirtioDev *v);

/* Make a request (the chain starting at descriptor `head`) available to the device and ring the doorbell. */
void virtq_push(Virtq *q, unsigned short head);
/* The next finished request: its head descriptor, and *len = bytes the device wrote. -1 if none is done yet. */
int virtq_pop(Virtq *q, unsigned *len);

/* Byte / word reads of the device specific config. */
unsigned char virtio_cfg8(const VirtioDev *v, unsigned off);
unsigned short virtio_cfg16(const VirtioDev *v, unsigned off);
unsigned virtio_cfg32(const VirtioDev *v, unsigned off);
unsigned long long virtio_cfg64(const VirtioDev *v, unsigned off);

#endif
