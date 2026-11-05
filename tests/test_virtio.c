/* Host test for the virtio transport (kernel/drivers/bus/virtio.c) and virtio-net (kernel/drivers/net/virtio_net.c) against a
 * pretend device: PCI config space and the BAR are plain memory, and the test plays the device by writing the used ring.
 * It proves the driver's own logic (capability walk, feature negotiation order, queue layout, descriptor/ring contents,
 * doorbell, copy in/out). It cannot prove that real hardware behaves like the model; QEMU does that (virtio-net got DHCP, virtio-blk
 * read/write/flush, see the commit notes). Limits of the model: the registers are plain bytes, so device_feature_select does
 * not switch halves (both halves show the same value) and the device never rejects FEATURES_OK. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmm.h"
#include "virtio.h"
#include "../kernel/drivers/net/virtio_net.c" /* the test needs the driver's internals; virtio.c is linked separately */

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static _Alignas(4096) unsigned char BAR[0x4000]; /* 0x0000 common, 0x1000 notify, 0x2000 isr, 0x3000 device */
static unsigned CFG[64];                         /* PCI config space as dwords */
static int RESETS;

void virtio_host_udelay(unsigned us) { (void)us; RESETS++; }
unsigned long long pmm_alloc_contiguous_tag(unsigned long long count, PmmTag tag)
{
    (void)tag;
    unsigned char *raw = malloc((size_t)count * 4096 + 4096);
    return (unsigned long long)(((size_t)raw + 4095) & ~(size_t)4095);
}
int io_dummy;
unsigned io_in8_stub;

unsigned int pci_cfg_read(unsigned bus, unsigned dev, unsigned fn, unsigned off) { (void)bus; (void)dev; (void)fn; return CFG[off / 4]; }
void pci_cfg_write(unsigned bus, unsigned dev, unsigned fn, unsigned off, unsigned int v)
{
    (void)bus; (void)dev; (void)fn;
    if (off == 4) CFG[1] = (CFG[1] & 0xFFFF0000u) | (v & 0xFFFFu); /* the status half is read only */
}

static void cap(unsigned at, unsigned next, unsigned len, unsigned type, unsigned bar, unsigned off, unsigned size)
{
    CFG[at / 4] = 0x09u | next << 8 | len << 16 | type << 24;
    CFG[at / 4 + 1] = bar;
    CFG[at / 4 + 2] = off;
    CFG[at / 4 + 3] = size;
}

static void device_init(int with_mac)
{
    memset(BAR, 0, sizeof BAR);
    memset(CFG, 0, sizeof CFG);
    CFG[1] = 0x00100000u; /* status: capability list present */
    CFG[0x34 / 4] = 0x40;
    unsigned long long a = (unsigned long long)(size_t)BAR;
    CFG[0x10 / 4] = (unsigned)a | 4u; /* BAR 0: 64 bit memory */
    CFG[0x14 / 4] = (unsigned)(a >> 32);
    cap(0x40, 0x50, 16, 1, 0, 0x0000, 0x40);
    cap(0x50, 0x64, 20, 2, 0, 0x1000, 0x100);
    CFG[0x50 / 4 + 4] = 4; /* notify_off_multiplier */
    cap(0x64, 0x74, 16, 3, 0, 0x2000, 4);
    cap(0x74, 0x00, 16, 4, 0, 0x3000, 0x20);
    unsigned feat = (1u << 16) | 1u | (with_mac ? (1u << 5) : 0); /* STATUS, VERSION_1 (bit 32 = bit 0 of the high half), MAC */
    memcpy(BAR + 4, &feat, 4);
    unsigned short qs = 256;
    memcpy(BAR + 24, &qs, 2);
    static const unsigned char mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    memcpy(BAR + 0x3000, mac, 6);
    BAR[0x3006] = 1; /* link up */
}

int main(void)
{
    PciDevice pd = {0, 3, 0, 0x1AF4, 0x1041, 0x02, 0x00, 0x00, 1, 0};
    CHECK(NIC_VIRTIO.claims(&pd));
    pd.device_id = 0x1000;
    CHECK(NIC_VIRTIO.claims(&pd));
    pd.device_id = 0x1042; /* virtio-blk is not a NIC */
    CHECK(!NIC_VIRTIO.claims(&pd));
    pd.device_id = 0x1041;
    pd.vendor_id = 0x8086;
    CHECK(!NIC_VIRTIO.claims(&pd));
    pd.vendor_id = 0x1AF4;

    /* ---- transport: capabilities ---- */
    device_init(1);
    VirtioDev v;
    const char *why = 0;
    CHECK(virtio_attach(&pd, &v, &why) == 0);
    CHECK((CFG[1] & 6) == 6);
    CHECK(v.common == BAR && v.notify == BAR + 0x1000 && v.isr == BAR + 0x2000 && v.dev == BAR + 0x3000 && v.notify_mult == 4);
    CFG[0x34 / 4] = 0; /* no capability list pointer: nothing found */
    CHECK(virtio_attach(&pd, &v, &why) == -1 && why && strstr(why, "common"));
    device_init(1);
    CFG[1] = 0; /* the device says it has no capability list */
    CHECK(virtio_attach(&pd, &v, &why) == -1 && strstr(why, "capabilities"));
    device_init(1);
    CFG[0x10 / 4] = 1; /* BAR is I/O ports: unusable for the modern interface */
    CHECK(virtio_attach(&pd, &v, &why) == -1);

    /* ---- a whole start of virtio-net ---- */
    device_init(1);
    CHECK(NIC_VIRTIO.probe(&pd) == 1);
    CHECK(BAR[20] == (VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK | VIRTIO_S_DRIVER_OK));
    CHECK(memcmp(NIC_VIRTIO.mac(), "\x52\x54\x00\x12\x34\x56", 6) == 0);
    CHECK(NIC_VIRTIO.link_up());
    BAR[0x3006] = 0;
    CHECK(!NIC_VIRTIO.link_up());
    BAR[0x3006] = 1;
    char st[96];
    NIC_VIRTIO.status(st, sizeof st);
    CHECK(strstr(st, "virtio-net") && strstr(st, "link up") && strstr(st, "rx 0 tx 0"));

    /* the queues: sizes cut to ours, the receive queue is full of device-writable buffers, the send queue is empty */
    CHECK(N.rxq.size == RX_COUNT && N.txq.size == TX_COUNT);
    CHECK(N.rxq.avail[1] == RX_COUNT && N.txq.avail[1] == 0);
    for (unsigned i = 0; i < RX_COUNT; i++)
    {
        CHECK(N.rxq.desc[i].addr == (unsigned long long)(size_t)(N.rx_buf + (size_t)i * BUF_SIZE));
        CHECK(N.rxq.desc[i].len == BUF_SIZE && N.rxq.desc[i].flags == VIRTQ_F_WRITE);
        CHECK(N.rxq.avail[2 + i] == i);
    }
    CHECK(sizeof(VirtqDesc) == 16);
    CHECK((size_t)N.rxq.avail - (size_t)N.rxq.desc >= 16 * VIRTQ_MAX); /* the arrays do not overlap */
    CHECK((size_t)N.rxq.used >= (size_t)N.rxq.avail + 6 + 2 * VIRTQ_MAX && (((size_t)N.rxq.used) & 3) == 0);
    CHECK(BAR[0x1000] == 0 && *(unsigned short *)(BAR + 0x1000) == 0); /* last doorbell: queue 0 (the last rx buffer posted) */

    /* ---- send: header (zeros) + frame in one descriptor, doorbell of queue 1 ---- */
    unsigned char fr[60];
    for (int i = 0; i < 60; i++) fr[i] = (unsigned char)(i + 1);
    CHECK(NIC_VIRTIO.send(fr, sizeof fr) == 0);
    CHECK(N.txq.desc[0].len == HDR_LEN + 60 && N.txq.desc[0].flags == 0);
    CHECK(memcmp((void *)(size_t)N.txq.desc[0].addr + HDR_LEN, fr, 60) == 0);
    for (int i = 0; i < HDR_LEN; i++) CHECK(((unsigned char *)(size_t)N.txq.desc[0].addr)[i] == 0);
    CHECK(N.txq.avail[1] == 1 && N.txq.avail[2] == 0);
    CHECK(*(unsigned short *)(BAR + 0x1000) == 1);
    CHECK(NIC_VIRTIO.send(fr, 13) == -1 && NIC_VIRTIO.send(fr, 2000) == -1 && NIC_VIRTIO.send(0, 60) == -1);
    /* the device never returns buffers: after 8 sends all are busy */
    for (int i = 1; i < TX_COUNT; i++) CHECK(NIC_VIRTIO.send(fr, 60) == 0);
    CHECK(NIC_VIRTIO.send(fr, 60) == -1 && N.tx_busy == 1);
    /* the device returns buffer 0 and 3: used ring entries {id, len} */
    volatile unsigned *ue = (volatile unsigned *)(N.txq.used + 4);
    ue[0] = 3; ue[1] = 0; ue[2] = 0; ue[3] = 0;
    *(volatile unsigned short *)(N.txq.used + 2) = 2;
    CHECK(NIC_VIRTIO.send(fr, 60) == 0); /* reuses the first free one: buffer 0 */
    CHECK(N.txq.avail[2 + 8 % TX_COUNT] == 0);  /* avail ring slot 8 wrapped to 0 and holds buffer 0 ... */
    CHECK(N.tx_busy_mask == 0xF7u); /* 3 came back and stayed free, 0 came back and was taken again */

    /* ---- receive: the device writes a frame into buffer 2 and reports it in the used ring ---- */
    unsigned char in[64];
    CHECK(NIC_VIRTIO.recv(in, sizeof in) == 0); /* nothing yet */
    unsigned char *rb = N.rx_buf + 2 * BUF_SIZE;
    memset(rb, 0, HDR_LEN);
    for (int i = 0; i < 50; i++) rb[HDR_LEN + i] = (unsigned char)(0x80 + i);
    volatile unsigned *re = (volatile unsigned *)(N.rxq.used + 4);
    re[0] = 2; re[1] = HDR_LEN + 50;
    *(volatile unsigned short *)(N.rxq.used + 2) = 1;
    CHECK(NIC_VIRTIO.recv(in, sizeof in) == 50 && in[0] == 0x80 && in[49] == 0x80 + 49);
    CHECK(N.rxq.avail[1] == RX_COUNT + 1 && N.rxq.avail[2 + 0] == 2); /* the buffer went back (ring slot 16 wrapped to 0) */
    CHECK(NIC_VIRTIO.recv(in, sizeof in) == 0);
    /* a runt (only a header) is dropped and the buffer still goes back; a frame longer than the caller's buffer is cut */
    re[2] = 5; re[3] = HDR_LEN;
    *(volatile unsigned short *)(N.rxq.used + 2) = 2;
    CHECK(NIC_VIRTIO.recv(in, sizeof in) == 0 && N.rx_dropped == 1 && N.rxq.avail[1] == RX_COUNT + 2);
    re[4] = 7; re[5] = HDR_LEN + 50;
    *(volatile unsigned short *)(N.rxq.used + 2) = 3;
    CHECK(NIC_VIRTIO.recv(in, 10) == 10);
    /* a nonsense id from the device is skipped, not used as an index */
    re[6] = 999; re[7] = 100;
    *(volatile unsigned short *)(N.rxq.used + 2) = 4;
    CHECK(NIC_VIRTIO.recv(in, sizeof in) == 0);
    NIC_VIRTIO.status(st, sizeof st);
    CHECK(strstr(st, "bad 1"));

    /* ---- failures give a text ---- */
    device_init(0); /* no MAC feature */
    CHECK(NIC_VIRTIO.probe(&pd) == 0);
    NIC_VIRTIO.status(st, sizeof st);
    CHECK(strstr(st, "failed") && strstr(st, "MAC"));
    device_init(1);
    unsigned feat = (1u << 16) | (1u << 5); /* no VERSION_1: a legacy-only device */
    memcpy(BAR + 4, &feat, 4);
    CHECK(NIC_VIRTIO.probe(&pd) == 0);
    NIC_VIRTIO.status(st, sizeof st);
    CHECK(strstr(st, "legacy"));
    CHECK(BAR[20] & VIRTIO_S_FAILED);
    CHECK(!NIC_VIRTIO.link_up());
    CHECK(NIC_VIRTIO.send("x", 1) == -1);
    char b[64];
    CHECK(NIC_VIRTIO.recv(b, sizeof b) == 0);

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("virtio tests passed\n");
    return 0;
}
