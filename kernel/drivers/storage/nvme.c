/* NVMe driver: M.2 / PCIe SSDs behind a PCI class 01:08:02 controller.
 *
 * Polled, one command at a time, one I/O queue pair, DMA through a 64 KiB bounce buffer. Every active
 * namespace of a controller becomes a block device "nvme0", "nvme1" ...
 *
 * NVMe in short: the controller has registers at the address in PCI BAR0. Commands are 64-byte entries
 * in a submission queue (SQ) in RAM; the controller answers with 16-byte entries in a completion queue (CQ).
 * We write the new SQ tail to a "doorbell" register to say "there is work", then poll the CQ: an entry is
 * new when its phase bit equals the phase we expect (the controller flips it on every lap through the queue).
 * Data is described by PRP entries - physical addresses of 4 KiB pages: PRP1 for the first page, PRP2 for the
 * second, or - when there are more - the address of a page full of further PRP entries (the PRP list). */
#include "blk.h"
#include "pci.h"
#include "../../storage/storage_internal.h"

#define R_CAP 0x00
#define R_INTMS 0x0C
#define R_CC 0x14
#define R_CSTS 0x1C
#define R_AQA 0x24
#define R_ASQ 0x28
#define R_ACQ 0x30
#define R_DOORBELL 0x1000

#define CC_EN 1u
#define CSTS_RDY 1u
#define CSTS_CFS 2u

#define QSIZE 64
#define BOUNCE_PAGES 16 /* 64 KiB */
#define NVME_MAX_CTRL 4
#define NVME_MAX_NS 8

#define OP_ADMIN_DELETE_SQ 0x00
#define OP_ADMIN_CREATE_SQ 0x01
#define OP_ADMIN_CREATE_CQ 0x05
#define OP_ADMIN_IDENTIFY 0x06
#define OP_IO_FLUSH 0x00
#define OP_IO_WRITE 0x01
#define OP_IO_READ 0x02

typedef struct
{
    u8 *sq, *cq;
    u64 sq_phys, cq_phys;
    unsigned sq_tail, cq_head;
    int phase;
    unsigned id;     /* queue id: 0 = admin */
    u16 next_cid;
} Queue;

typedef struct
{
    volatile u8 *regs;
    unsigned stride; /* doorbell stride in bytes */
    Queue admin, io;
    u8 *page;        /* scratch page for Identify */
    u64 page_phys;
    u8 *bounce;
    u64 bounce_phys;
    u8 *prp_list;    /* one page of PRP entries for transfers over two pages */
    u64 prp_list_phys;
    unsigned max_bytes; /* biggest transfer per command */
} Nvme;

typedef struct
{
    Nvme *ctrl;
    u32 nsid;
    unsigned sector_size;
} NvmeNs;

static Nvme CTRL[NVME_MAX_CTRL];
static NvmeNs NS[NVME_MAX_NS];
static int NNS;
static int NCTRL;

static void sq_doorbell(Nvme *c, Queue *q) { mmio_wr32(c->regs, R_DOORBELL + (2 * q->id) * c->stride, q->sq_tail); }
static void cq_doorbell(Nvme *c, Queue *q) { mmio_wr32(c->regs, R_DOORBELL + (2 * q->id + 1) * c->stride, q->cq_head); }

/* Send one command and wait for its completion. cdw[0..5] are command dwords 10..15. Returns 0 if the
 * controller reported success, -1 on an error status or a timeout. */
static int submit(Nvme *c, Queue *q, u8 opcode, u32 nsid, u64 prp1, u64 prp2, const u32 cdw[6])
{
    volatile u32 *e = (volatile u32 *)(q->sq + (u64)q->sq_tail * 64);
    u16 cid = q->next_cid++;
    stor_zero((void *)e, 64);
    e[0] = opcode | ((u32)cid << 16);
    e[1] = nsid;
    e[6] = (u32)prp1;
    e[7] = (u32)(prp1 >> 32);
    e[8] = (u32)prp2;
    e[9] = (u32)(prp2 >> 32);
    for (int i = 0; i < 6; i++)
        e[10 + i] = cdw[i];
    mem_fence();
    q->sq_tail = (q->sq_tail + 1) % QSIZE;
    sq_doorbell(c, q);

    volatile u32 *cqe = (volatile u32 *)(q->cq + (u64)q->cq_head * 16);
    int got = STOR_WAIT_MS((((cqe[3] >> 16) & 1u) == (u32)q->phase), 5000);
    if (!got)
        return -1;
    u32 dw3 = cqe[3];
    mem_fence();
    q->cq_head++;
    if (q->cq_head == QSIZE)
    {
        q->cq_head = 0;
        q->phase ^= 1;
    }
    cq_doorbell(c, q);
    if ((dw3 & 0xFFFFu) != cid)
        return -1;
    return (dw3 >> 17) == 0 ? 0 : -1; /* status field (type + code) must be 0 */
}

static int ctrl_ready(Nvme *c, int want, unsigned timeout_ms)
{
    return STOR_WAIT_MS(((mmio_rd32(c->regs, R_CSTS) & CSTS_RDY) != 0) == (want != 0), timeout_ms);
}

static int io_xfer(NvmeNs *ns, u8 opcode, u64 lba, unsigned count, u32 bytes)
{
    Nvme *c = ns->ctrl;
    u64 prp1 = c->bounce_phys, prp2 = 0;
    unsigned pages = (bytes + 4095) / 4096;
    if (pages == 2)
        prp2 = c->bounce_phys + 4096;
    else if (pages > 2)
    {
        volatile u64 *list = (volatile u64 *)c->prp_list;
        for (unsigned i = 1; i < pages; i++)
            list[i - 1] = c->bounce_phys + (u64)i * 4096;
        prp2 = c->prp_list_phys;
    }
    u32 cdw[6] = {(u32)lba, (u32)(lba >> 32), count - 1, 0, 0, 0};
    return submit(c, &c->io, opcode, ns->nsid, prp1, prp2, cdw);
}

static unsigned max_sectors(const NvmeNs *ns) { return ns->ctrl->max_bytes / ns->sector_size; }

static int nvme_read(BlkDev *bd, u64 lba, unsigned count, void *buf)
{
    NvmeNs *ns = (NvmeNs *)bd->ctx;
    u8 *out = (u8 *)buf;
    while (count)
    {
        unsigned n = count < max_sectors(ns) ? count : max_sectors(ns);
        if (io_xfer(ns, OP_IO_READ, lba, n, n * ns->sector_size) != 0)
            return -1;
        stor_copy(out, ns->ctrl->bounce, (unsigned long)n * ns->sector_size);
        out += (unsigned long)n * ns->sector_size;
        lba += n;
        count -= n;
    }
    return 0;
}

static int nvme_write(BlkDev *bd, u64 lba, unsigned count, const void *buf)
{
    NvmeNs *ns = (NvmeNs *)bd->ctx;
    const u8 *in = (const u8 *)buf;
    while (count)
    {
        unsigned n = count < max_sectors(ns) ? count : max_sectors(ns);
        stor_copy(ns->ctrl->bounce, in, (unsigned long)n * ns->sector_size);
        if (io_xfer(ns, OP_IO_WRITE, lba, n, n * ns->sector_size) != 0)
            return -1;
        in += (unsigned long)n * ns->sector_size;
        lba += n;
        count -= n;
    }
    return 0;
}

static int nvme_flush(BlkDev *bd)
{
    NvmeNs *ns = (NvmeNs *)bd->ctx;
    u32 cdw[6] = {0, 0, 0, 0, 0, 0};
    return submit(ns->ctrl, &ns->ctrl->io, OP_IO_FLUSH, ns->nsid, 0, 0, cdw);
}

static int identify(Nvme *c, u32 cns, u32 nsid)
{
    stor_zero(c->page, 4096);
    u32 cdw[6] = {cns, 0, 0, 0, 0, 0};
    return submit(c, &c->admin, OP_ADMIN_IDENTIFY, nsid, c->page_phys, 0, cdw);
}

/* one namespace -> one block device */
static void add_namespace(Nvme *c, u32 nsid, const char *model, int ctrl_no)
{
    (void)ctrl_no;
    if (NNS >= NVME_MAX_NS || identify(c, 0, nsid) != 0)
        return;
    u64 nsze = mmio_rd64((volatile u8 *)c->page, 0);
    unsigned flbas = c->page[26] & 0x0F;
    if (flbas > 15)
        return;
    u32 lbaf = *(volatile u32 *)(c->page + 128 + 4 * flbas);
    unsigned ms = lbaf & 0xFFFF, lbads = (lbaf >> 16) & 0xFF;
    if (nsze == 0 || ms != 0 || lbads < 9 || lbads > 12)
        return; /* empty, or has per-sector metadata we do not handle, or an odd sector size */
    NvmeNs *ns = &NS[NNS];
    ns->ctrl = c;
    ns->nsid = nsid;
    ns->sector_size = 1u << lbads;

    BlkDev bd;
    stor_zero(&bd, sizeof bd);
    bd.name[0] = 'n'; bd.name[1] = 'v'; bd.name[2] = 'm'; bd.name[3] = 'e';
    bd.name[4] = (char)('0' + NNS % 10);
    bd.name[5] = 0;
    for (int i = 0; i < (int)sizeof bd.model - 1 && model[i]; i++)
        bd.model[i] = model[i];
    bd.kind = BLK_NVME;
    bd.media = BLK_MEDIA_SSD;
    bd.parent = -1;
    bd.sector_size = ns->sector_size;
    bd.sectors = nsze;
    bd.writable = 1;
    bd.read = nvme_read;
    bd.write = nvme_write;
    bd.flush = nvme_flush;
    bd.ctx = ns;
    if (blk_register(&bd) >= 0)
        NNS++;
}

static int bring_up(const PciDevice *pd)
{
    if (NCTRL >= NVME_MAX_CTRL)
        return 0;
    u32 cmd = pci_cfg_read(pd->bus, pd->dev, pd->fn, 4);
    pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (cmd & 0xFFFFu) | 0x6u); /* memory space + bus master */
    u32 bar0 = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10);
    if (bar0 & 1u)
        return 0;
    u64 base = bar0 & ~0xFULL;
    if (((bar0 >> 1) & 3u) == 2u)
        base |= (u64)pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x14) << 32;
    if (!base)
        return 0;

    Nvme *c = &CTRL[NCTRL];
    stor_zero(c, sizeof *c);
    c->regs = (volatile u8 *)base;
    u64 cap = mmio_rd64(c->regs, R_CAP);
    c->stride = 4u << ((cap >> 32) & 0xF);
    unsigned timeout_ms = (unsigned)(((cap >> 24) & 0xFF) + 1) * 500u;
    if (timeout_ms > 10000)
        timeout_ms = 10000;
    if (!((cap >> 37) & 1u) || ((cap >> 48) & 0xF) != 0) /* needs the NVM command set and 4 KiB pages */
        return 0;

    /* stop the controller (the firmware may have been using it), then set up the admin queues */
    mmio_wr32(c->regs, R_CC, mmio_rd32(c->regs, R_CC) & ~CC_EN);
    if (!ctrl_ready(c, 0, timeout_ms))
        return 0;

    u64 pages = stor_dma_alloc(5, 0); /* admin SQ, admin CQ, I/O SQ, I/O CQ, scratch */
    u64 bounce = stor_dma_alloc(BOUNCE_PAGES, 0);
    u64 list = stor_dma_alloc(1, 0);
    if (!pages || !bounce || !list)
    {
        if (pages)
            pmm_free_contiguous(pages, 5);
        if (bounce)
            pmm_free_contiguous(bounce, BOUNCE_PAGES);
        if (list)
            pmm_free_contiguous(list, 1);
        return 0;
    }
    c->admin.id = 0;
    c->admin.sq_phys = pages;
    c->admin.cq_phys = pages + 4096;
    c->io.id = 1;
    c->io.sq_phys = pages + 2 * 4096;
    c->io.cq_phys = pages + 3 * 4096;
    c->page_phys = pages + 4 * 4096;
    c->admin.sq = (u8 *)c->admin.sq_phys;
    c->admin.cq = (u8 *)c->admin.cq_phys;
    c->io.sq = (u8 *)c->io.sq_phys;
    c->io.cq = (u8 *)c->io.cq_phys;
    c->page = (u8 *)c->page_phys;
    c->admin.phase = c->io.phase = 1;
    c->bounce = (u8 *)bounce;
    c->bounce_phys = bounce;
    c->prp_list = (u8 *)list;
    c->prp_list_phys = list;
    c->max_bytes = BOUNCE_PAGES * 4096;

    mmio_wr32(c->regs, R_INTMS, 0xFFFFFFFFu); /* polled: no interrupts */
    mmio_wr32(c->regs, R_AQA, ((QSIZE - 1) << 16) | (QSIZE - 1));
    mmio_wr64(c->regs, R_ASQ, c->admin.sq_phys);
    mmio_wr64(c->regs, R_ACQ, c->admin.cq_phys);
    /* enable: 64-byte SQ entries (2^6), 16-byte CQ entries (2^4), 4 KiB pages, NVM command set */
    mmio_wr32(c->regs, R_CC, CC_EN | (6u << 16) | (4u << 20));
    if (!ctrl_ready(c, 1, timeout_ms) || (mmio_rd32(c->regs, R_CSTS) & CSTS_CFS))
        return 0;

    /* controller identity: model and the biggest transfer it accepts */
    if (identify(c, 1, 0) != 0)
        return 0;
    char model[41];
    int n = 0;
    for (int i = 0; i < 40; i++)
        model[n++] = (char)c->page[24 + i];
    while (n > 0 && (model[n - 1] == ' ' || model[n - 1] == 0))
        n--;
    model[n] = 0;
    unsigned mdts = c->page[77];
    if (mdts && (4096u << mdts) < c->max_bytes)
        c->max_bytes = 4096u << mdts;
    if (c->max_bytes < 4096)
        return 0;

    /* one I/O queue pair: the CQ first, then the SQ that reports into it */
    u32 cq_cdw[6] = {((QSIZE - 1) << 16) | 1u, 1u /* physically contiguous, interrupts off */, 0, 0, 0, 0};
    if (submit(c, &c->admin, OP_ADMIN_CREATE_CQ, 0, c->io.cq_phys, 0, cq_cdw) != 0)
        return 0;
    u32 sq_cdw[6] = {((QSIZE - 1) << 16) | 1u, (1u << 16) | 1u /* completes into CQ 1, contiguous */, 0, 0, 0, 0};
    if (submit(c, &c->admin, OP_ADMIN_CREATE_SQ, 0, c->io.sq_phys, 0, sq_cdw) != 0)
        return 0;

    NCTRL++;
    int before = NNS;
    /* the active namespaces (Identify, list of active NSIDs); a controller too old for that has namespace 1 */
    u32 ids[16];
    int nids = 0;
    if (identify(c, 2, 0) == 0)
    {
        for (int i = 0; i < 16; i++)
        {
            u32 id = *(volatile u32 *)(c->page + 4 * i);
            if (!id)
                break;
            ids[nids++] = id;
        }
    }
    else
        ids[nids++] = 1;
    for (int i = 0; i < nids; i++)
        add_namespace(c, ids[i], model, NCTRL - 1);
    return NNS - before;
}

int nvme_init(void)
{
    int found = 0;
    for (int i = 0; i < pci_count(); i++)
    {
        const PciDevice *pd = pci_get(i);
        if (pd->class_code == 0x01 && pd->subclass == 0x08 && pd->prog_if == 0x02)
            found += bring_up(pd);
    }
    return found;
}
