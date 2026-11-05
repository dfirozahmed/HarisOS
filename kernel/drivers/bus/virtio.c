/* virtio over PCI, modern (1.0) interface, polled. See include/virtio.h for the picture.
 *
 * Common configuration structure (virtio spec 4.1.4.3), offsets in bytes:
 *   0 device_feature_select  4 device_feature   8 driver_feature_select  12 driver_feature
 *  16 msix_config (u16)     18 num_queues (u16) 20 device_status (u8)     21 config_generation (u8)
 *  22 queue_select (u16)    24 queue_size (u16) 26 queue_msix_vector      28 queue_enable (u16)
 *  30 queue_notify_off (u16) 32 queue_desc (u64) 40 queue_driver (u64)    48 queue_device (u64)
 * A feature set is 64 bits wide but the registers show 32 at a time: *_select picks the low (0) or high (1) half. */
#include "io.h"
#include "pmm.h"
#include "virtio.h"

#define C_DEV_FEAT_SEL 0
#define C_DEV_FEAT 4
#define C_DRV_FEAT_SEL 8
#define C_DRV_FEAT 12
#define C_NUM_QUEUES 18
#define C_STATUS 20
#define C_Q_SELECT 22
#define C_Q_SIZE 24
#define C_Q_ENABLE 28
#define C_Q_NOTIFY_OFF 30
#define C_Q_DESC 32
#define C_Q_DRIVER 40
#define C_Q_DEVICE 48

#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR 3
#define CAP_DEVICE 4

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

static inline u8 rd8(volatile u8 *b, u32 off) { return *(volatile u8 *)(b + off); }
static inline u16 rd16(volatile u8 *b, u32 off) { return *(volatile u16 *)(b + off); }
static inline u32 rd32(volatile u8 *b, u32 off) { return *(volatile u32 *)(b + off); }
static inline void wr8(volatile u8 *b, u32 off, u8 v) { *(volatile u8 *)(b + off) = v; }
static inline void wr16(volatile u8 *b, u32 off, u16 v) { *(volatile u16 *)(b + off) = v; }
static inline void wr32(volatile u8 *b, u32 off, u32 v) { *(volatile u32 *)(b + off) = v; }
static inline void fence(void) { __asm__ volatile("mfence" ::: "memory"); }

#ifdef HARIS_HOST
void virtio_host_udelay(unsigned us);
static void udelay(unsigned us) { virtio_host_udelay(us); }
#else
/* a read of port 0x80 takes about 1 us: a delay that needs no timer */
static void udelay(unsigned us)
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}
#endif

static void zero(void *p, unsigned long n)
{
    volatile u8 *b = (volatile u8 *)p;
    for (unsigned long i = 0; i < n; i++)
        b[i] = 0;
}

static int bad(const char **why, const char *text)
{
    if (why)
        *why = text;
    return -1;
}

/* Address of byte `off` of BAR `bar` (memory BARs only; a 64 bit BAR takes two slots). 0 if unusable. */
static u64 bar_base(const PciDevice *pd, unsigned bar)
{
    if (bar > 5)
        return 0;
    u32 v = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10 + 4 * bar);
    if (v & 1u) /* I/O ports: the modern interface does not use them */
        return 0;
    u64 base = v & ~0xFULL;
    if (((v >> 1) & 3u) == 2u) /* 64 bit BAR: the next dword is its upper half */
    {
        if (bar >= 5)
            return 0;
        base |= (u64)pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10 + 4 * (bar + 1)) << 32;
    }
    return base;
}

int virtio_attach(const PciDevice *pd, VirtioDev *v, const char **why)
{
    zero(v, sizeof *v);
    u32 cmd = pci_cfg_read(pd->bus, pd->dev, pd->fn, 4);
    pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (cmd & 0xFFFFu) | 0x6u); /* memory space + bus master */
    if (!((pci_cfg_read(pd->bus, pd->dev, pd->fn, 4) >> 16) & 0x10u)) /* status bit 4: has a capability list */
        return bad(why, "no PCI capabilities");

    unsigned ptr = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x34) & 0xFCu;
    for (int guard = 0; ptr >= 0x40 && guard < 48; guard++)
    {
        u32 h = pci_cfg_read(pd->bus, pd->dev, pd->fn, ptr);
        unsigned id = h & 0xFFu, next = (h >> 8) & 0xFCu, len = (h >> 16) & 0xFFu, type = (h >> 24) & 0xFFu;
        if (id == 0x09 && len >= 16) /* vendor specific: a virtio_pci_cap */
        {
            unsigned bar = pci_cfg_read(pd->bus, pd->dev, pd->fn, ptr + 4) & 0xFFu;
            u32 off = pci_cfg_read(pd->bus, pd->dev, pd->fn, ptr + 8);
            u32 size = pci_cfg_read(pd->bus, pd->dev, pd->fn, ptr + 12);
            u64 base = bar_base(pd, bar);
            if (base && size && type >= CAP_COMMON && type <= CAP_DEVICE && (u64)off + size > off)
            {
                volatile u8 *p = (volatile u8 *)(base + off);
                if (type == CAP_COMMON && !v->common && size >= 56)
                    v->common = p;
                else if (type == CAP_NOTIFY && !v->notify && len >= 20)
                {
                    v->notify = p;
                    v->notify_mult = pci_cfg_read(pd->bus, pd->dev, pd->fn, ptr + 16);
                }
                else if (type == CAP_ISR && !v->isr)
                    v->isr = p;
                else if (type == CAP_DEVICE && !v->dev)
                    v->dev = p;
            }
        }
        ptr = next;
    }
    if (!v->common)
        return bad(why, "no common config capability");
    if (!v->notify)
        return bad(why, "no notify capability");
    if (rd32(v->common, C_DEV_FEAT) == 0xFFFFFFFFu && rd32(v->common, C_NUM_QUEUES) == 0xFFFFFFFFu)
        return bad(why, "registers read as all ones");
    return 0;
}

unsigned virtio_status(const VirtioDev *v) { return rd8(v->common, C_STATUS); }

void virtio_fail(VirtioDev *v) { wr8(v->common, C_STATUS, (u8)(rd8(v->common, C_STATUS) | VIRTIO_S_FAILED)); }

int virtio_negotiate(VirtioDev *v, u64 want, u64 *got, const char **why)
{
    volatile u8 *c = v->common;
    wr8(c, C_STATUS, 0); /* reset */
    int done = 0;
    for (int i = 0; i < 1000 && !done; i++) /* the device sets the status to 0 when the reset is done */
    {
        done = rd8(c, C_STATUS) == 0;
        if (!done)
            udelay(100);
    }
    if (!done)
        return bad(why, "reset did not finish");
    wr8(c, C_STATUS, VIRTIO_S_ACK);
    wr8(c, C_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER);

    u64 dev = 0;
    wr32(c, C_DEV_FEAT_SEL, 0);
    dev = rd32(c, C_DEV_FEAT);
    wr32(c, C_DEV_FEAT_SEL, 1);
    dev |= (u64)rd32(c, C_DEV_FEAT) << 32;
    if (!(dev & VIRTIO_F_VERSION_1))
    {
        virtio_fail(v);
        return bad(why, "device is not virtio 1.0 (legacy only)");
    }
    u64 use = (want & dev) | VIRTIO_F_VERSION_1;
    wr32(c, C_DRV_FEAT_SEL, 0);
    wr32(c, C_DRV_FEAT, (u32)use);
    wr32(c, C_DRV_FEAT_SEL, 1);
    wr32(c, C_DRV_FEAT, (u32)(use >> 32));
    wr8(c, C_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK);
    if (!(rd8(c, C_STATUS) & VIRTIO_S_FEATURES_OK)) /* the device refuses our combination */
    {
        virtio_fail(v);
        return bad(why, "device rejected the features");
    }
    if (got)
        *got = use;
    return 0;
}

int virtio_queue_init(VirtioDev *v, Virtq *q, u16 index, u16 max_size, const char **why)
{
    volatile u8 *c = v->common;
    zero(q, sizeof *q);
    wr16(c, C_Q_SELECT, index);
    u16 size = rd16(c, C_Q_SIZE);
    if (size == 0) /* the queue does not exist */
        return bad(why, "queue not available");
    if (size > max_size)
        size = max_size;
    if (size > VIRTQ_MAX)
        size = VIRTQ_MAX;
    if (size & (size - 1)) /* the device's maximum is a power of two, so a power of two max_size keeps it one */
        return bad(why, "queue size not a power of two");

    u64 page = pmm_alloc_contiguous_tag(1, PMM_TAG_OTHER);
    if (!page)
        return bad(why, "no memory for a queue");
    zero((void *)page, PMM_PAGE_SIZE);
    /* layout in the page: descriptors (16 bytes each, 16 aligned), avail ring (2 + 2 + 2n + 2), used ring (4 + 8n + 2, 4 aligned) */
    u64 desc = page;
    u64 avail = page + 16u * VIRTQ_MAX;                  /* 1024 */
    u64 used = (avail + 6 + 2u * VIRTQ_MAX + 3) & ~3ULL; /* 1160 */
    q->v = v;
    q->index = index;
    q->size = size;
    q->desc = (VirtqDesc *)desc;
    q->avail = (volatile u16 *)avail;
    q->used = (volatile u8 *)used;

    wr16(c, C_Q_SIZE, size);
    wr32(c, C_Q_DESC, (u32)desc);
    wr32(c, C_Q_DESC + 4, (u32)(desc >> 32));
    wr32(c, C_Q_DRIVER, (u32)avail);
    wr32(c, C_Q_DRIVER + 4, (u32)(avail >> 32));
    wr32(c, C_Q_DEVICE, (u32)used);
    wr32(c, C_Q_DEVICE + 4, (u32)(used >> 32));
    q->notify_off = rd16(c, C_Q_NOTIFY_OFF);
    wr16(c, C_Q_ENABLE, 1);
    return 0;
}

void virtio_driver_ok(VirtioDev *v)
{
    wr8(v->common, C_STATUS, (u8)(rd8(v->common, C_STATUS) | VIRTIO_S_DRIVER_OK));
}

void virtq_push(Virtq *q, u16 head)
{
    q->avail[2 + (q->avail_idx & (q->size - 1))] = head;
    fence(); /* the ring entry before the index that publishes it */
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
    fence(); /* the index before the doorbell */
    wr16(q->v->notify, (u32)q->notify_off * q->v->notify_mult, q->index);
}

int virtq_pop(Virtq *q, u32 *len)
{
    fence();
    if (*(volatile u16 *)(q->used + 2) == q->last_used)
        return -1;
    volatile u32 *e = (volatile u32 *)(q->used + 4 + 8u * (q->last_used & (q->size - 1)));
    u32 id = e[0];
    if (len)
        *len = e[1];
    q->last_used++;
    if (id >= q->size)
        return -1; /* a nonsense id from the device: skip it */
    return (int)id;
}

u8 virtio_cfg8(const VirtioDev *v, u32 off) { return v->dev ? rd8(v->dev, off) : 0; }
u16 virtio_cfg16(const VirtioDev *v, u32 off) { return v->dev ? rd16(v->dev, off) : 0; }
u32 virtio_cfg32(const VirtioDev *v, u32 off) { return v->dev ? rd32(v->dev, off) : 0; }
u64 virtio_cfg64(const VirtioDev *v, u32 off) { return (u64)virtio_cfg32(v, off) | (u64)virtio_cfg32(v, off + 4) << 32; }
