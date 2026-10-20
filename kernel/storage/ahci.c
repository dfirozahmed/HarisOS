/* AHCI (SATA) driver: SATA SSDs and hard disks behind a PCI class 01:06:01 controller.
 *
 * Polled, one command at a time (slot 0), DMA through a 64 KiB bounce buffer per disk. Only ATA disks
 * (not ATAPI drives, not port multipliers). Each disk found is registered as a block device "sata0", "sata1" ...
 *
 * AHCI in short: the controller (HBA) has registers at the address in PCI BAR5. For each port that has a
 * drive, we give it three areas of memory: a command list (32 slots of 32 bytes), a place where the drive's
 * answers (FIS) land, and a command table (the command itself as an ATA "register FIS" plus a list of memory
 * pieces to transfer, the PRDT). Writing bit 0 of the port's CI register starts slot 0; the drive clears
 * the bit when it is done. */
#include "blk.h"
#include "pci.h"
#include "storage_internal.h"

#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_PI 0x0C
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28

#define P_CLB 0x00
#define P_CLBU 0x04
#define P_FB 0x08
#define P_FBU 0x0C
#define P_IS 0x10
#define P_IE 0x14
#define P_CMD 0x18
#define P_TFD 0x20
#define P_SIG 0x24
#define P_SSTS 0x28
#define P_SERR 0x30
#define P_SACT 0x34
#define P_CI 0x38

#define CMD_ST 0x0001u
#define CMD_FRE 0x0010u
#define CMD_FR 0x4000u
#define CMD_CR 0x8000u
#define TFD_BUSY_DRQ 0x88u /* BSY | DRQ */
#define IS_TFES (1u << 30) /* task file error */

#define SIG_ATA 0x00000101u

#define ATA_READ_DMA 0xC8
#define ATA_WRITE_DMA 0xCA
#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH_EXT 0xEA
#define ATA_FLUSH 0xE7
#define ATA_IDENTIFY 0xEC

#define BOUNCE_PAGES 16 /* 64 KiB */
#define AHCI_MAX_DISKS 8

typedef struct
{
    volatile u8 *port; /* this port's registers */
    u8 *clb;           /* command list (page offset 0, 1 KiB) */
    u8 *ctbl;          /* command table (page offset 2048) */
    u64 clb_phys, fis_phys, ctbl_phys;
    u8 *bounce;
    u64 bounce_phys;
    unsigned sector_size;
    int lba48;
} AhciDisk;

static AhciDisk DISKS[AHCI_MAX_DISKS];
static int NDISKS;

static int port_stop(volatile u8 *p)
{
    u32 cmd = mmio_rd32(p, P_CMD);
    mmio_wr32(p, P_CMD, cmd & ~CMD_ST);
    if (!STOR_WAIT_MS(!(mmio_rd32(p, P_CMD) & CMD_CR), 500))
        return -1;
    mmio_wr32(p, P_CMD, mmio_rd32(p, P_CMD) & ~CMD_FRE);
    if (!STOR_WAIT_MS(!(mmio_rd32(p, P_CMD) & CMD_FR), 500))
        return -1;
    return 0;
}

static void port_start(volatile u8 *p)
{
    STOR_WAIT_MS(!(mmio_rd32(p, P_CMD) & CMD_CR), 500);
    mmio_wr32(p, P_CMD, mmio_rd32(p, P_CMD) | CMD_FRE);
    mmio_wr32(p, P_CMD, mmio_rd32(p, P_CMD) | CMD_ST);
}

/* Run one command in slot 0. `bytes` = data to transfer (0 = none). The data goes through the bounce buffer. */
static int ahci_cmd(AhciDisk *d, u8 cmd, u64 lba, u32 count, int write, u32 bytes)
{
    volatile u8 *p = d->port;
    if (!STOR_WAIT_MS(!(mmio_rd32(p, P_TFD) & TFD_BUSY_DRQ), 1000))
        goto fail;
    mmio_wr32(p, P_IS, 0xFFFFFFFFu);
    mmio_wr32(p, P_SERR, 0xFFFFFFFFu);

    volatile u32 *hdr = (volatile u32 *)d->clb;
    hdr[0] = 5u | (write ? (1u << 6) : 0u) | (bytes ? (1u << 16) : 0u); /* CFL = 5 dwords, W, PRDTL */
    hdr[1] = 0;                                                         /* PRDBC: the controller fills it in */
    hdr[2] = (u32)d->ctbl_phys;
    hdr[3] = (u32)(d->ctbl_phys >> 32);

    volatile u8 *t = d->ctbl;
    stor_zero((void *)t, 0x90);
    t[0] = 0x27; /* FIS type: register, host to device */
    t[1] = 0x80; /* C bit: this is a command */
    t[2] = cmd;
    if (d->lba48 || cmd == ATA_IDENTIFY || cmd == ATA_FLUSH_EXT)
    {
        t[4] = (u8)lba;
        t[5] = (u8)(lba >> 8);
        t[6] = (u8)(lba >> 16);
        t[7] = 0x40; /* device: LBA mode */
        t[8] = (u8)(lba >> 24);
        t[9] = (u8)(lba >> 32);
        t[10] = (u8)(lba >> 40);
    }
    else /* 28-bit LBA: the top four bits go into the device register */
    {
        t[4] = (u8)lba;
        t[5] = (u8)(lba >> 8);
        t[6] = (u8)(lba >> 16);
        t[7] = (u8)(0x40 | ((lba >> 24) & 0x0F));
    }
    t[12] = (u8)count;
    t[13] = (u8)(count >> 8);
    if (bytes)
    {
        volatile u32 *prd = (volatile u32 *)(t + 0x80);
        prd[0] = (u32)d->bounce_phys;
        prd[1] = (u32)(d->bounce_phys >> 32);
        prd[2] = 0;
        prd[3] = bytes - 1;
    }
    mem_fence();
    mmio_wr32(p, P_CI, 1);
    int done = STOR_WAIT_MS(!(mmio_rd32(p, P_CI) & 1u) || (mmio_rd32(p, P_IS) & IS_TFES), 5000);
    if (!done || (mmio_rd32(p, P_IS) & IS_TFES) || (mmio_rd32(p, P_TFD) & 1u))
        goto fail;
    return 0;

fail:
    /* a stuck or failed command: stop and restart the port so the next one starts clean */
    if (port_stop(p) == 0)
    {
        mmio_wr32(p, P_SERR, 0xFFFFFFFFu);
        mmio_wr32(p, P_IS, 0xFFFFFFFFu);
        port_start(p);
    }
    return -1;
}

static unsigned max_sectors(const AhciDisk *d)
{
    unsigned n = (BOUNCE_PAGES * (unsigned)PMM_PAGE_SIZE) / d->sector_size;
    return n > 128 ? 128 : n; /* 128 keeps the 28-bit count (1..255) valid too */
}

static int ahci_read(BlkDev *bd, u64 lba, unsigned count, void *buf)
{
    AhciDisk *d = (AhciDisk *)bd->ctx;
    u8 *out = (u8 *)buf;
    while (count)
    {
        unsigned n = count < max_sectors(d) ? count : max_sectors(d);
        if (ahci_cmd(d, d->lba48 ? ATA_READ_DMA_EXT : ATA_READ_DMA, lba, n, 0, n * d->sector_size) != 0)
            return -1;
        stor_copy(out, d->bounce, (unsigned long)n * d->sector_size);
        out += (unsigned long)n * d->sector_size;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ahci_write(BlkDev *bd, u64 lba, unsigned count, const void *buf)
{
    AhciDisk *d = (AhciDisk *)bd->ctx;
    const u8 *in = (const u8 *)buf;
    while (count)
    {
        unsigned n = count < max_sectors(d) ? count : max_sectors(d);
        stor_copy(d->bounce, in, (unsigned long)n * d->sector_size);
        if (ahci_cmd(d, d->lba48 ? ATA_WRITE_DMA_EXT : ATA_WRITE_DMA, lba, n, 1, n * d->sector_size) != 0)
            return -1;
        in += (unsigned long)n * d->sector_size;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ahci_flush(BlkDev *bd)
{
    AhciDisk *d = (AhciDisk *)bd->ctx;
    return ahci_cmd(d, d->lba48 ? ATA_FLUSH_EXT : ATA_FLUSH, 0, 0, 0, 0);
}

/* IDENTIFY DEVICE: size, sector size and model */
static int identify(AhciDisk *d, BlkDev *out)
{
    if (ahci_cmd(d, ATA_IDENTIFY, 0, 0, 0, 512) != 0)
        return -1;
    u16 w[256];
    stor_copy(w, d->bounce, 512);
    d->lba48 = (w[83] & (1u << 10)) != 0;
    u64 sectors = d->lba48 ? ((u64)w[100] | (u64)w[101] << 16 | (u64)w[102] << 32 | (u64)w[103] << 48)
                           : ((u64)w[60] | (u64)w[61] << 16);
    out->media = w[217] == 1 ? BLK_MEDIA_SSD : (w[217] > 0x0400 && w[217] < 0xFFFF) ? BLK_MEDIA_HDD : BLK_MEDIA_UNKNOWN; /* nominal rotation rate */
    unsigned ss = 512;
    if ((w[106] & 0xC000) == 0x4000 && (w[106] & (1u << 12))) /* valid, and the logical sector is not 256 words */
        ss = 2u * ((u32)w[117] | (u32)w[118] << 16);
    if (sectors == 0 || (ss != 512 && ss != 1024 && ss != 2048 && ss != 4096))
        return -1;
    d->sector_size = ss;
    out->sectors = sectors;
    out->sector_size = ss;
    int n = 0;
    for (int i = 27; i <= 46 && n < (int)sizeof out->model - 1; i++) /* model: ASCII, two characters per word, swapped */
    {
        out->model[n++] = (char)(w[i] >> 8);
        out->model[n++] = (char)(w[i] & 0xFF);
    }
    while (n > 0 && (out->model[n - 1] == ' ' || out->model[n - 1] == 0))
        n--;
    out->model[n] = 0;
    return 0;
}

static int bring_up_port(volatile u8 *hba, int port, int s64a, int sata_no)
{
    (void)hba;
    volatile u8 *p = hba + 0x100 + 0x80 * port;
    if (NDISKS >= AHCI_MAX_DISKS)
        return 0;
    u32 ssts = mmio_rd32(p, P_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)
        return 0; /* no device, or the link is not up */
    if (mmio_rd32(p, P_SIG) != SIG_ATA)
        return 0; /* ATAPI, port multiplier, enclosure: not handled */

    if (port_stop(p) != 0)
        return 0;
    AhciDisk *d = &DISKS[NDISKS];
    stor_zero(d, sizeof *d);
    u64 page = stor_dma_alloc(1, !s64a);
    u64 bounce = stor_dma_alloc(BOUNCE_PAGES, !s64a);
    if (!page || !bounce)
    {
        if (page)
            pmm_free_contiguous(page, 1);
        if (bounce)
            pmm_free_contiguous(bounce, BOUNCE_PAGES);
        return 0;
    }
    d->port = p;
    d->clb = (u8 *)page;
    d->clb_phys = page;
    d->fis_phys = page + 1024;
    d->ctbl = (u8 *)(page + 2048);
    d->ctbl_phys = page + 2048;
    d->bounce = (u8 *)bounce;
    d->bounce_phys = bounce;
    d->sector_size = 512;
    mmio_wr32(p, P_CLB, (u32)d->clb_phys);
    mmio_wr32(p, P_CLBU, (u32)(d->clb_phys >> 32));
    mmio_wr32(p, P_FB, (u32)d->fis_phys);
    mmio_wr32(p, P_FBU, (u32)(d->fis_phys >> 32));
    mmio_wr32(p, P_SERR, 0xFFFFFFFFu);
    mmio_wr32(p, P_IS, 0xFFFFFFFFu);
    mmio_wr32(p, P_IE, 0); /* polled */
    port_start(p);

    BlkDev bd;
    stor_zero(&bd, sizeof bd);
    if (identify(d, &bd) != 0)
    {
        port_stop(p);
        pmm_free_contiguous(page, 1);
        pmm_free_contiguous(bounce, BOUNCE_PAGES);
        return 0;
    }
    bd.name[0] = 's'; bd.name[1] = 'a'; bd.name[2] = 't'; bd.name[3] = 'a';
    bd.name[4] = (char)('0' + sata_no % 10);
    bd.name[5] = 0;
    bd.kind = BLK_SATA;
    bd.parent = -1;
    bd.writable = 1;
    bd.read = ahci_read;
    bd.write = ahci_write;
    bd.flush = ahci_flush;
    bd.ctx = d;
    if (blk_register(&bd) < 0)
        return 0;
    NDISKS++;
    return 1;
}

int ahci_init(void)
{
    int found = 0;
    for (int i = 0; i < pci_count(); i++)
    {
        const PciDevice *pd = pci_get(i);
        if (pd->class_code != 0x01 || pd->subclass != 0x06 || pd->prog_if != 0x01)
            continue;
        u32 cmd = pci_cfg_read(pd->bus, pd->dev, pd->fn, 4);
        pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (cmd & 0xFFFFu) | 0x6u); /* memory space + bus master */
        u32 bar5 = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x24);
        if ((bar5 & 1u) || !(bar5 & ~0xFu))
            continue;
        volatile u8 *hba = (volatile u8 *)(u64)(bar5 & ~0xFu);

        mmio_wr32(hba, HBA_GHC, mmio_rd32(hba, HBA_GHC) | (1u << 31)); /* AHCI enable */
        if (mmio_rd32(hba, HBA_CAP2) & 1u) /* BIOS/OS handoff supported: take ownership */
        {
            mmio_wr32(hba, HBA_BOHC, mmio_rd32(hba, HBA_BOHC) | 2u);
            STOR_WAIT_MS(!(mmio_rd32(hba, HBA_BOHC) & 0x10u), 1000);
        }
        int s64a = (mmio_rd32(hba, HBA_CAP) >> 31) & 1;
        u32 pi = mmio_rd32(hba, HBA_PI);
        for (int port = 0; port < 32; port++)
            if (pi & (1u << port))
                found += bring_up_port(hba, port, s64a, NDISKS);
    }
    return found;
}
