/* virtio-blk: the paravirtual disk of QEMU / KVM (PCI 1AF4:1001 or 1AF4:1042). Polled, one request at a time.
 * See include/virtio.h for the transport. Each disk found is registered as a block device "vblk0", "vblk1" ...
 *
 * A request is a chain of three descriptors in queue 0:
 *   1. header (device reads):  u32 type (0 read, 1 write, 4 flush), u32 reserved, u64 sector
 *   2. data   (device reads for a write, writes for a read): n * 512 bytes; absent for a flush
 *   3. status (device writes): one byte, 0 = ok, 1 = I/O error, 2 = unsupported
 * The sector number in a request is ALWAYS in 512 byte units, even when the disk reports a bigger block size (blk_size in
 * its config); the capacity in the config is in 512 byte units too. The data goes through a 64 KiB bounce buffer per disk. */
#include "blk.h"
#include "virtio.h"
#include "../../storage/storage_internal.h"

#define F_RO (1ULL << 5)       /* the disk is read only */
#define F_BLK_SIZE (1ULL << 6) /* config has the block size */
#define F_FLUSH (1ULL << 9)    /* the disk can flush its cache */

#define CFG_CAPACITY 0 /* u64, in 512 byte sectors */
#define CFG_BLK_SIZE 20 /* u32 */

#define T_IN 0u
#define T_OUT 1u
#define T_FLUSH 4u

#define BOUNCE_PAGES 16 /* 64 KiB */
#define VBLK_MAX_DISKS 4

typedef struct
{
    VirtioDev v;
    Virtq q;
    u8 *req;       /* one page: the 16 byte header at 0, the status byte at 16 */
    u8 *bounce;
    u64 bounce_phys;
    unsigned sector_size; /* the disk's block size (512 unless the config says otherwise) */
    int has_flush;
} Vblk;

static Vblk DISKS[VBLK_MAX_DISKS];
static int NDISKS;

/* Run one request; `bytes` of data through the bounce buffer (0 for a flush). */
static int vblk_req(Vblk *d, u32 type, u64 sector512, u32 bytes)
{
    volatile u32 *h = (volatile u32 *)d->req;
    h[0] = type;
    h[1] = 0;
    h[2] = (u32)sector512;
    h[3] = (u32)(sector512 >> 32);
    volatile u8 *status = d->req + 16;
    *status = 0xFF; /* the device overwrites it */

    VirtqDesc *ds = d->q.desc;
    u64 req_phys = (u64)d->req;
    ds[0].addr = req_phys;
    ds[0].len = 16;
    ds[0].flags = VIRTQ_F_NEXT;
    ds[0].next = bytes ? 1 : 2;
    if (bytes)
    {
        ds[1].addr = d->bounce_phys;
        ds[1].len = bytes;
        ds[1].flags = (u16)(VIRTQ_F_NEXT | (type == T_IN ? VIRTQ_F_WRITE : 0));
        ds[1].next = 2;
    }
    ds[2].addr = req_phys + 16;
    ds[2].len = 1;
    ds[2].flags = VIRTQ_F_WRITE;
    ds[2].next = 0;

    virtq_push(&d->q, 0);
    int id = -1;
    int done = STOR_WAIT_MS((id = virtq_pop(&d->q, 0)) >= 0, 5000);
    if (!done)
        return -1; /* the device never answered; the queue is stuck, later requests will fail too */
    mem_fence();
    return *status == 0 ? 0 : -1;
}

static unsigned max_sectors(const Vblk *d)
{
    return (BOUNCE_PAGES * (unsigned)PMM_PAGE_SIZE) / d->sector_size;
}

static int vblk_read(BlkDev *bd, u64 lba, unsigned count, void *buf)
{
    Vblk *d = (Vblk *)bd->ctx;
    u8 *out = (u8 *)buf;
    unsigned scale = d->sector_size / 512;
    while (count)
    {
        unsigned n = count < max_sectors(d) ? count : max_sectors(d);
        if (vblk_req(d, T_IN, lba * scale, n * d->sector_size) != 0)
            return -1;
        stor_copy(out, d->bounce, (unsigned long)n * d->sector_size);
        out += (unsigned long)n * d->sector_size;
        lba += n;
        count -= n;
    }
    return 0;
}

static int vblk_write(BlkDev *bd, u64 lba, unsigned count, const void *buf)
{
    Vblk *d = (Vblk *)bd->ctx;
    const u8 *in = (const u8 *)buf;
    unsigned scale = d->sector_size / 512;
    while (count)
    {
        unsigned n = count < max_sectors(d) ? count : max_sectors(d);
        stor_copy(d->bounce, in, (unsigned long)n * d->sector_size);
        if (vblk_req(d, T_OUT, lba * scale, n * d->sector_size) != 0)
            return -1;
        in += (unsigned long)n * d->sector_size;
        lba += n;
        count -= n;
    }
    return 0;
}

static int vblk_flush(BlkDev *bd)
{
    Vblk *d = (Vblk *)bd->ctx;
    if (!d->has_flush) /* no write cache that needs flushing: writes are already done */
        return 0;
    return vblk_req(d, T_FLUSH, 0, 0);
}

static int bring_up(const PciDevice *pd, int number)
{
    Vblk *d = &DISKS[NDISKS];
    stor_zero(d, sizeof *d);
    const char *why = 0;
    u64 got = 0;
    if (virtio_attach(pd, &d->v, &why) != 0)
        return 0;
    if (virtio_negotiate(&d->v, F_RO | F_BLK_SIZE | F_FLUSH, &got, &why) != 0)
        return 0;
    if (virtio_queue_init(&d->v, &d->q, 0, 4, &why) != 0 || d->q.size < 4) /* three descriptors per request, one request at a time */
    {
        virtio_fail(&d->v);
        return 0;
    }
    u64 req = stor_dma_alloc(1, 0);
    u64 bounce = stor_dma_alloc(BOUNCE_PAGES, 0);
    if (!req || !bounce)
    {
        if (req)
            pmm_free_contiguous(req, 1);
        if (bounce)
            pmm_free_contiguous(bounce, BOUNCE_PAGES);
        virtio_fail(&d->v);
        return 0;
    }
    d->req = (u8 *)req;
    d->bounce = (u8 *)bounce;
    d->bounce_phys = bounce;
    d->has_flush = (got & F_FLUSH) != 0;
    d->sector_size = 512;
    if (got & F_BLK_SIZE)
    {
        u32 bs = virtio_cfg32(&d->v, CFG_BLK_SIZE);
        if (bs == 512 || bs == 1024 || bs == 2048 || bs == 4096)
            d->sector_size = bs;
    }
    u64 cap512 = virtio_cfg64(&d->v, CFG_CAPACITY);
    u64 sectors = cap512 / (d->sector_size / 512);
    if (sectors == 0)
    {
        virtio_fail(&d->v);
        return 0;
    }
    virtio_driver_ok(&d->v);

    BlkDev bd;
    stor_zero(&bd, sizeof bd);
    bd.name[0] = 'v'; bd.name[1] = 'b'; bd.name[2] = 'l'; bd.name[3] = 'k';
    bd.name[4] = (char)('0' + number % 10);
    bd.name[5] = 0;
    const char *model = "virtio-blk";
    for (int i = 0; model[i]; i++)
        bd.model[i] = model[i];
    bd.kind = BLK_VIRTIO;
    bd.media = BLK_MEDIA_UNKNOWN; /* a virtual disk is neither */
    bd.parent = -1;
    bd.sector_size = d->sector_size;
    bd.sectors = sectors;
    bd.writable = (got & F_RO) ? 0 : 1;
    bd.read = vblk_read;
    bd.write = vblk_write;
    bd.flush = vblk_flush;
    bd.ctx = d;
    if (blk_register(&bd) < 0)
        return 0;
    NDISKS++;
    return 1;
}

int virtio_blk_init(void)
{
    int found = 0;
    for (int i = 0; i < pci_count() && NDISKS < VBLK_MAX_DISKS; i++)
    {
        const PciDevice *pd = pci_get(i);
        if (pd->vendor_id != VIRTIO_VENDOR || (pd->device_id != VIRTIO_DEV_BLK && pd->device_id != VIRTIO_DEV_BLK_LEGACY))
            continue;
        found += bring_up(pd, NDISKS);
    }
    return found;
}
