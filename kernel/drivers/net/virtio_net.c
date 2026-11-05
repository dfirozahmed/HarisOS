/* virtio-net: the paravirtual network card of QEMU / KVM (PCI 1AF4:1000 or 1AF4:1041). Polled, one adapter. See kernel/net/nic.h
 * and include/virtio.h for the transport.
 *
 * Two virtqueues: 0 = receive, 1 = send. Every buffer starts with a 12 byte virtio_net_hdr (flags, gso_type, hdr_len, gso_size,
 * csum_start, csum_offset, num_buffers). We negotiate no offloads, so on send the header is all zeros ("a plain frame") and
 * on receive it is skipped. Each buffer is one descriptor: header and frame in one piece of memory.
 *   receive: all RX_COUNT buffers are given to the device (WRITE descriptors); a finished one is copied out and given back.
 *   send:    a free buffer is filled and pushed; the device returns it through the used ring, which we read before the next send. */
#include "../../net/nic.h"
#include "pci.h"
#include "pmm.h"
#include "virtio.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define F_MAC (1ULL << 5)     /* the device has a MAC address in its config */
#define F_STATUS (1ULL << 16) /* the config has a link status field */

#define CFG_MAC 0
#define CFG_STATUS 6 /* u16, bit 0 = link up */

#define HDR_LEN 12 /* sizeof(virtio_net_hdr) with VERSION_1 (it includes num_buffers) */
#define RX_COUNT 16
#define TX_COUNT 8
#define BUF_SIZE 2048
#define MAX_FRAME 1514 /* without CRC; header + frame fit a buffer */

typedef struct
{
    VirtioDev v;
    Virtq rxq, txq;
    u8 *rx_buf, *tx_buf;
    u32 tx_busy_mask; /* bit i = send buffer i is with the device */
    u8 mac[6];
    int has_status;
    int ok;
    unsigned rx_frames, rx_dropped, tx_frames, tx_busy;
    const char *fail;
} Vnet;

static Vnet N;

static void zero(void *p, unsigned long n)
{
    volatile u8 *b = (volatile u8 *)p;
    for (unsigned long i = 0; i < n; i++)
        b[i] = 0;
}

static void copy(void *dst, const void *src, unsigned long n)
{
    volatile u8 *d = (volatile u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)src;
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
}

static int vnet_claims(const PciDevice *pd)
{
    return pd->vendor_id == VIRTIO_VENDOR && (pd->device_id == VIRTIO_DEV_NET || pd->device_id == VIRTIO_DEV_NET_LEGACY);
}

static int fail(const char *why)
{
    N.fail = why;
    N.ok = 0;
    return 0;
}

static void rx_post(unsigned i)
{
    VirtqDesc *d = &N.rxq.desc[i];
    d->addr = (u64)(N.rx_buf + (u64)i * BUF_SIZE);
    d->len = BUF_SIZE;
    d->flags = VIRTQ_F_WRITE;
    d->next = 0;
    virtq_push(&N.rxq, (u16)i);
}

static int vnet_probe(const PciDevice *pd)
{
    zero(&N, sizeof N);
    const char *why = 0;
    if (virtio_attach(pd, &N.v, &why) != 0)
        return fail(why);
    u64 got = 0;
    if (virtio_negotiate(&N.v, F_MAC | F_STATUS, &got, &why) != 0)
        return fail(why);
    if (!(got & F_MAC)) /* a device without an address would need a made-up one: not worth it */
        return fail("device has no MAC address");
    N.has_status = (got & F_STATUS) != 0;
    if (virtio_queue_init(&N.v, &N.rxq, 0, RX_COUNT, &why) != 0 || virtio_queue_init(&N.v, &N.txq, 1, TX_COUNT, &why) != 0)
    {
        virtio_fail(&N.v);
        return fail(why);
    }
    if (N.rxq.size < RX_COUNT || N.txq.size < TX_COUNT) /* a device with smaller queues than ours: not handled */
    {
        virtio_fail(&N.v);
        return fail("queues too small");
    }
    unsigned pages = (RX_COUNT + TX_COUNT) * BUF_SIZE / PMM_PAGE_SIZE;
    u64 mem = pmm_alloc_contiguous_tag(pages, PMM_TAG_OTHER);
    if (!mem)
    {
        virtio_fail(&N.v);
        return fail("no memory for the buffers");
    }
    zero((void *)mem, (unsigned long)pages * PMM_PAGE_SIZE);
    N.rx_buf = (u8 *)mem;
    N.tx_buf = N.rx_buf + (u64)RX_COUNT * BUF_SIZE;
    for (int i = 0; i < 6; i++)
        N.mac[i] = virtio_cfg8(&N.v, CFG_MAC + i);
    for (unsigned i = 0; i < RX_COUNT; i++)
        rx_post(i);
    virtio_driver_ok(&N.v);
    N.ok = 1;
    return 1;
}

static const unsigned char *vnet_mac(void) { return N.mac; }

static int vnet_link_up(void)
{
    if (!N.ok)
        return 0;
    return N.has_status ? (virtio_cfg16(&N.v, CFG_STATUS) & 1u) != 0 : 1; /* no status field: the link is always up */
}

static void tx_reclaim(void)
{
    int id;
    while ((id = virtq_pop(&N.txq, 0)) >= 0)
        N.tx_busy_mask &= ~(1u << id);
}

static int vnet_send(const void *frame, unsigned len)
{
    if (!N.ok || !frame || len < 14 || len > MAX_FRAME)
        return -1;
    tx_reclaim();
    unsigned i = 0;
    while (i < TX_COUNT && (N.tx_busy_mask & (1u << i)))
        i++;
    if (i == TX_COUNT)
    {
        N.tx_busy++;
        return -1;
    }
    u8 *buf = N.tx_buf + (u64)i * BUF_SIZE;
    zero(buf, HDR_LEN);
    copy(buf + HDR_LEN, frame, len);
    VirtqDesc *d = &N.txq.desc[i];
    d->addr = (u64)buf;
    d->len = HDR_LEN + len;
    d->flags = 0; /* the device only reads it */
    d->next = 0;
    N.tx_busy_mask |= 1u << i;
    N.tx_frames++;
    virtq_push(&N.txq, (u16)i);
    return 0;
}

static int vnet_recv(void *buf, unsigned max)
{
    if (!N.ok)
        return 0;
    u32 len = 0;
    int id = virtq_pop(&N.rxq, &len);
    if (id < 0)
        return 0;
    int got = 0;
    if (len > HDR_LEN && len <= BUF_SIZE)
    {
        unsigned n = len - HDR_LEN;
        got = (int)(n < max ? n : max);
        copy(buf, N.rx_buf + (u64)id * BUF_SIZE + HDR_LEN, (unsigned long)got);
        N.rx_frames++;
    }
    else
        N.rx_dropped++;
    rx_post((unsigned)id);
    return got;
}

static void vnet_status(char *buf, unsigned cap)
{
    NicText t;
    nic_text_init(&t, buf, cap);
    if (!N.ok)
    {
        nic_puts(&t, "virtio-net failed: ");
        nic_puts(&t, N.fail ? N.fail : "not started");
        return;
    }
    nic_puts(&t, "virtio-net, ");
    nic_puts(&t, vnet_link_up() ? "link up" : "link down");
    nic_puts(&t, ", rx ");
    nic_putd(&t, N.rx_frames);
    nic_puts(&t, " tx ");
    nic_putd(&t, N.tx_frames);
    if (N.rx_dropped)
    {
        nic_puts(&t, " bad ");
        nic_putd(&t, N.rx_dropped);
    }
}

const NicDriver NIC_VIRTIO = {
    .name = "virtio-net",
    .claims = vnet_claims,
    .probe = vnet_probe,
    .mac = vnet_mac,
    .link_up = vnet_link_up,
    .send = vnet_send,
    .recv = vnet_recv,
    .status = vnet_status,
};
