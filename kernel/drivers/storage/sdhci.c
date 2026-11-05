/* SDHCI: the standard SD Host Controller (PCI class 08:05) and the SD / SDHC / SDXC card in its slot. Polled, PIO (the data
 * goes through the 32 bit buffer port, no DMA), one command at a time. Each card is registered as block device "sd0", "sd1" ...
 *
 * eMMC chips and SDIO cards (Wi-Fi) are not handled: they have a different start-up (CMD1 / CMD5). Controllers that are only
 * "almost standard" (Realtek, O2Micro, Ricoh card readers) may not work; the standard register layout below is what QEMU's
 * sdhci-pci and most Intel / AMD / Arasan / Synopsys style controllers follow.
 *
 * Registers (SD Host Controller Simplified Specification, offsets in bytes):
 *   0x04 block size (16)   0x06 block count (16)   0x08 argument (32)   0x0C transfer mode (16)   0x0E command (16)
 *   0x10 response (4 x 32) 0x20 buffer data port   0x24 present state   0x28 host control (8)  0x29 power (8)
 *   0x2C clock control (16) 0x2F software reset (8) 0x30 interrupt status (16) 0x32 error status (16)
 *   0x34 / 0x36 which status bits are recorded (the signal enable register 0x38 stays 0: polled)
 *   0x40 capabilities   0xFE host controller version
 * A command: write argument, (transfer mode,) command; wait for "command complete" in the status register; read the response.
 * The card start-up is the SD protocol: CMD0 reset, CMD8 voltage check, ACMD41 until powered up, CMD2 CID, CMD3 address (RCA),
 * CMD9 CSD (size), CMD7 select, ACMD6 four bit bus. */
#include "blk.h"
#include "pci.h"
#include "../../storage/storage_internal.h"

#define R_BLKSIZE 0x04
#define R_BLKCNT 0x06
#define R_ARG 0x08
#define R_MODE 0x0C
#define R_CMD 0x0E
#define R_RESP 0x10
#define R_DATA 0x20
#define R_STATE 0x24
#define R_HOST 0x28
#define R_POWER 0x29
#define R_CLOCK 0x2C
#define R_RESET 0x2F
#define R_IS 0x30
#define R_ES 0x32
#define R_IS_EN 0x34
#define R_ES_EN 0x36
#define R_SIG_EN 0x38
#define R_CAPS 0x40
#define R_VERSION 0xFE

#define ST_CMD_INHIBIT 0x1u
#define ST_DAT_INHIBIT 0x2u
#define ST_CARD_IN 0x10000u

#define IS_CMD_DONE 0x0001u
#define IS_XFER_DONE 0x0002u
#define IS_WRITE_RDY 0x0010u
#define IS_READ_RDY 0x0020u
#define IS_ERROR 0x8000u

#define RESP_NONE 0x00
#define RESP_R1 0x1A /* 48 bit response, CRC and index checked */
#define RESP_R1B 0x1B /* same, and the card holds DAT0 low while busy */
#define RESP_R2 0x09  /* 136 bit response (CID / CSD), CRC checked */
#define RESP_R3 0x02  /* 48 bit response without CRC (OCR) */
#define CMD_DATA 0x20 /* a data phase follows */

#define MAX_BLOCKS 128 /* per command */
#define SD_MAX_CARDS 4

typedef struct
{
    volatile u8 *r;
    u32 base_khz;
    int version; /* 0 = spec 1.0, 1 = 2.0, 2 = 3.0 */
    int sdhc;    /* block addressed (SDHC / SDXC) */
    u16 rca;
    u32 resp[4];
} Sd;

static Sd SDS[SD_MAX_CARDS];
static int NSD;

static inline u8 rd8(Sd *s, u32 o) { return *(volatile u8 *)(s->r + o); }
static inline u16 rd16(Sd *s, u32 o) { return *(volatile u16 *)(s->r + o); }
static inline u32 rd32(Sd *s, u32 o) { return *(volatile u32 *)(s->r + o); }
static inline void wr8(Sd *s, u32 o, u8 v) { *(volatile u8 *)(s->r + o) = v; }
static inline void wr16(Sd *s, u32 o, u16 v) { *(volatile u16 *)(s->r + o) = v; }
static inline void wr32(Sd *s, u32 o, u32 v) { *(volatile u32 *)(s->r + o) = v; }

/* The clock control register value for a card clock of at most `khz`, given the controller's base clock. The divided clock is
 * base / (2 * N) (spec 3.0: N = 0..1023, 10 bits split over two fields) or base / D with D a power of two up to 256 (spec 1.0 / 2.0:
 * the register holds D / 2 in its upper byte). Bit 0 turns the internal clock on; the caller adds the "stable" wait and bit 2. */
static u16 clock_ctrl(u32 base_khz, u32 khz, int version)
{
    if (version >= 2)
    {
        u32 n = base_khz <= khz ? 0 : (base_khz + 2 * khz - 1) / (2 * khz);
        if (n > 1023)
            n = 1023;
        return (u16)(((n & 0xFF) << 8) | (((n >> 8) & 3) << 6) | 1u);
    }
    u32 d = 1; /* divisor, 1 means undivided; the register holds d / 2 */
    while (d < 256 && base_khz / d > khz)
        d <<= 1;
    return (u16)(((d >> 1) << 8) | 1u);
}

/* Size in 512 byte sectors from the 128 bit CSD. The response registers hold CSD bits 127..8 (the CRC byte is cut off), so
 * a CSD bit p is at response bit p - 8. 0 if the structure version is not one we know. */
static u64 csd_sectors(const u32 resp[4])
{
    u32 ver = (resp[3] >> 22) & 3; /* CSD_STRUCTURE, CSD bits 127:126 */
    if (ver == 1)                  /* CSD 2.0 (SDHC / SDXC): C_SIZE (22 bits, CSD 69:48) * 512 KiB */
        return ((u64)((resp[1] >> 8) & 0x3FFFFF) + 1) * 1024;
    if (ver == 0) /* CSD 1.0 (SDSC): (C_SIZE + 1) << (C_SIZE_MULT + 2) blocks of 2^READ_BL_LEN bytes */
    {
        u32 read_bl = (resp[2] >> 8) & 0xF;      /* CSD 83:80 */
        u32 c_size = (resp[1] >> 22) | ((resp[2] & 3) << 10); /* CSD 73:62 */
        u32 mult = (resp[1] >> 7) & 7;           /* CSD 49:47 */
        if (read_bl < 9 || read_bl > 12)
            return 0;
        return (((u64)c_size + 1) << (mult + 2)) << read_bl >> 9;
    }
    return 0;
}

static void reset_lines(Sd *s, u8 mask) /* 2 = command line, 4 = data lines */
{
    wr8(s, R_RESET, mask);
    STOR_WAIT_MS(!(rd8(s, R_RESET) & mask), 100);
}

/* Run a command. 0 = ok (response in s->resp), -1 = error or timeout (the lines are reset so the next command starts clean). */
static int sd_cmd(Sd *s, u32 idx, u32 arg, u32 flags)
{
    u32 busy = ST_CMD_INHIBIT | ((flags & RESP_R1B) == RESP_R1B || (flags & CMD_DATA) ? ST_DAT_INHIBIT : 0);
    if (!STOR_WAIT_MS(!(rd32(s, R_STATE) & busy), 1000))
        goto fail;
    wr16(s, R_IS, 0xFFFF);
    wr16(s, R_ES, 0xFFFF);
    wr32(s, R_ARG, arg);
    wr16(s, R_CMD, (u16)(idx << 8 | flags));
    if (!STOR_WAIT_MS(rd16(s, R_IS) & (IS_CMD_DONE | IS_ERROR), 1000) || (rd16(s, R_IS) & IS_ERROR))
        goto fail;
    wr16(s, R_IS, IS_CMD_DONE);
    for (int i = 0; i < 4; i++)
        s->resp[i] = rd32(s, R_RESP + 4 * i);
    if ((flags & RESP_R1B) == RESP_R1B) /* the card is busy for a while after e.g. CMD7 / CMD12 */
        STOR_WAIT_MS(!(rd32(s, R_STATE) & ST_DAT_INHIBIT), 1000);
    return 0;
fail:
    wr16(s, R_IS, 0xFFFF);
    wr16(s, R_ES, 0xFFFF);
    reset_lines(s, 2 | 4);
    return -1;
}

static int sd_acmd(Sd *s, u32 idx, u32 arg, u32 flags)
{
    if (sd_cmd(s, 55, (u32)s->rca << 16, RESP_R1) != 0)
        return -1;
    return sd_cmd(s, idx, arg, flags);
}

static int set_clock(Sd *s, u32 khz)
{
    wr16(s, R_CLOCK, 0); /* stop the card clock while changing it */
    u16 c = clock_ctrl(s->base_khz, khz, s->version);
    wr16(s, R_CLOCK, c);
    if (!STOR_WAIT_MS(rd16(s, R_CLOCK) & 2u, 150)) /* internal clock stable */
        return -1;
    wr16(s, R_CLOCK, (u16)(c | 4u)); /* SD clock enable */
    return 0;
}

/* Transfer `count` (1..MAX_BLOCKS) blocks through the data port. */
static int sd_xfer(Sd *s, u64 lba, unsigned count, int write, u8 *buf)
{
    u32 addr = (u32)(s->sdhc ? lba : lba * 512);
    u32 idx = write ? (count > 1 ? 25u : 24u) : (count > 1 ? 18u : 17u);
    if (!STOR_WAIT_MS(!(rd32(s, R_STATE) & (ST_CMD_INHIBIT | ST_DAT_INHIBIT)), 1000))
        goto fail;
    wr16(s, R_IS, 0xFFFF);
    wr16(s, R_ES, 0xFFFF);
    wr16(s, R_BLKSIZE, 512);
    wr16(s, R_BLKCNT, (u16)count);
    wr32(s, R_ARG, addr);
    /* transfer mode: bit 1 block count enable, bit 2 auto CMD12 (stop) after the last block, bit 4 read, bit 5 multi block */
    wr16(s, R_MODE, (u16)(0x02 | (count > 1 ? 0x24 : 0) | (write ? 0 : 0x10)));
    wr16(s, R_CMD, (u16)(idx << 8 | RESP_R1 | CMD_DATA));
    if (!STOR_WAIT_MS(rd16(s, R_IS) & (IS_CMD_DONE | IS_ERROR), 1000) || (rd16(s, R_IS) & IS_ERROR))
        goto fail;
    wr16(s, R_IS, IS_CMD_DONE);
    u32 ready = write ? IS_WRITE_RDY : IS_READ_RDY;
    for (unsigned b = 0; b < count; b++)
    {
        if (!STOR_WAIT_MS(rd16(s, R_IS) & (ready | IS_ERROR), 2000) || (rd16(s, R_IS) & IS_ERROR))
            goto fail;
        wr16(s, R_IS, (u16)ready);
        for (int i = 0; i < 128; i++, buf += 4)
        {
            if (write)
                wr32(s, R_DATA, (u32)buf[0] | (u32)buf[1] << 8 | (u32)buf[2] << 16 | (u32)buf[3] << 24);
            else
            {
                u32 v = rd32(s, R_DATA);
                buf[0] = (u8)v;
                buf[1] = (u8)(v >> 8);
                buf[2] = (u8)(v >> 16);
                buf[3] = (u8)(v >> 24);
            }
        }
    }
    if (!STOR_WAIT_MS(rd16(s, R_IS) & (IS_XFER_DONE | IS_ERROR), 2000) || (rd16(s, R_IS) & IS_ERROR))
        goto fail;
    wr16(s, R_IS, IS_XFER_DONE);
    return 0;
fail:
    wr16(s, R_IS, 0xFFFF);
    wr16(s, R_ES, 0xFFFF);
    reset_lines(s, 2 | 4);
    if (count > 1)
        sd_cmd(s, 12, 0, RESP_R1B); /* stop a multi block transfer that was cut short */
    return -1;
}

static int sd_read(BlkDev *bd, u64 lba, unsigned count, void *buf)
{
    Sd *s = (Sd *)bd->ctx;
    u8 *out = (u8 *)buf;
    while (count)
    {
        unsigned n = count < MAX_BLOCKS ? count : MAX_BLOCKS;
        if (sd_xfer(s, lba, n, 0, out) != 0)
            return -1;
        out += (unsigned long)n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static int sd_write(BlkDev *bd, u64 lba, unsigned count, const void *buf)
{
    Sd *s = (Sd *)bd->ctx;
    u8 *in = (u8 *)buf; /* only read from, in sd_xfer's write direction */
    while (count)
    {
        unsigned n = count < MAX_BLOCKS ? count : MAX_BLOCKS;
        if (sd_xfer(s, lba, n, 1, in) != 0)
            return -1;
        in += (unsigned long)n * 512;
        lba += n;
        count -= n;
    }
    /* the card programs the last block after the transfer: wait until it lets go of DAT0 */
    STOR_WAIT_MS(!(rd32(s, R_STATE) & ST_DAT_INHIBIT), 2000);
    return 0;
}

static int sd_flush(BlkDev *bd)
{
    (void)bd; /* every write waited for the card to finish */
    return 0;
}

/* Start the card in the slot of controller `s` (already mapped). Returns the number of sectors, 0 = no usable card. */
static u64 card_start(Sd *s)
{
    reset_lines(s, 1); /* reset all */
    if (rd8(s, R_RESET) & 1u)
        return 0;
    u32 caps = rd32(s, R_CAPS);
    s->version = rd16(s, R_VERSION) & 0xFF;
    s->base_khz = (s->version >= 2 ? (caps >> 8) & 0xFF : (caps >> 8) & 0x3F) * 1000u;
    if (s->base_khz == 0)
        s->base_khz = 50000; /* the controller does not say (needs board data): assume 50 MHz */
    if (!(rd32(s, R_STATE) & ST_CARD_IN))
        return 0;
    /* power: pick the highest voltage the controller offers: bit 24 = 3.3 V, 25 = 3.0 V, 26 = 1.8 V */
    u8 volt = (caps & (1u << 24)) ? 0x7 : (caps & (1u << 25)) ? 0x6 : (caps & (1u << 26)) ? 0x5 : 0;
    if (!volt)
        return 0;
    wr8(s, R_POWER, 0);
    wr8(s, R_POWER, (u8)(volt << 1));
    wr8(s, R_POWER, (u8)(volt << 1 | 1));
    wr8(s, R_HOST, 0);
    wr16(s, R_IS_EN, 0x00FF); /* record command / transfer / buffer ready bits; not the card interrupt */
    wr16(s, R_ES_EN, 0xFFFF);
    wr16(s, R_SIG_EN, 0);
    wr8(s, 0x2E, 0xE); /* data timeout: the longest (2^27 clocks) */
    if (set_clock(s, 400) != 0)
        return 0;
    stor_udelay(2000); /* the card needs 74 clocks to settle */

    s->rca = 0;
    s->sdhc = 0;
    if (sd_cmd(s, 0, 0, RESP_NONE) != 0)
        return 0;
    int v2 = sd_cmd(s, 8, 0x1AA, RESP_R1) == 0 && (s->resp[0] & 0xFFF) == 0x1AA; /* a v2 card echoes voltage + pattern */
    u32 ocr = 0;
    int ready = 0;
    for (int i = 0; i < 200 && !ready; i++) /* the card needs up to a second to power up */
    {
        if (sd_acmd(s, 41, 0x00FF8000u | (v2 ? 0x40000000u : 0), RESP_R3) != 0)
            return 0; /* not an SD memory card (an eMMC or SDIO device answers differently) */
        ocr = s->resp[0];
        ready = (ocr >> 31) & 1;
        if (!ready)
            stor_udelay(5000);
    }
    if (!ready)
        return 0;
    s->sdhc = (ocr >> 30) & 1;
    if (sd_cmd(s, 2, 0, RESP_R2) != 0)
        return 0;
    if (sd_cmd(s, 3, 0, RESP_R1) != 0)
        return 0;
    s->rca = (u16)(s->resp[0] >> 16);
    if (sd_cmd(s, 9, (u32)s->rca << 16, RESP_R2) != 0)
        return 0;
    u64 sectors = csd_sectors(s->resp);
    if (sectors == 0)
        return 0;
    if (sd_cmd(s, 7, (u32)s->rca << 16, RESP_R1B) != 0)
        return 0;
    if (sd_acmd(s, 6, 2, RESP_R1) == 0) /* four bit bus: the card and then the controller */
        wr8(s, R_HOST, (u8)(rd8(s, R_HOST) | 2u));
    if (!s->sdhc && sd_cmd(s, 16, 512, RESP_R1) != 0) /* SDSC cards need the block length set */
        return 0;
    if (set_clock(s, 25000) != 0)
        return 0;
    return sectors;
}

static int bring_up(volatile u8 *regs)
{
    if (NSD >= SD_MAX_CARDS)
        return 0;
    Sd *s = &SDS[NSD];
    stor_zero(s, sizeof *s);
    s->r = regs;
    u64 sectors = card_start(s);
    if (!sectors)
        return 0;
    BlkDev bd;
    stor_zero(&bd, sizeof bd);
    bd.name[0] = 's'; bd.name[1] = 'd';
    bd.name[2] = (char)('0' + NSD % 10);
    bd.name[3] = 0;
    const char *model = s->sdhc ? "SD card (SDHC/SDXC)" : "SD card";
    for (int i = 0; model[i]; i++)
        bd.model[i] = model[i];
    bd.kind = BLK_SD;
    bd.media = BLK_MEDIA_UNKNOWN;
    bd.parent = -1;
    bd.sector_size = 512;
    bd.sectors = sectors;
    bd.writable = 1;
    bd.read = sd_read;
    bd.write = sd_write;
    bd.flush = sd_flush;
    bd.ctx = s;
    if (blk_register(&bd) < 0)
        return 0;
    NSD++;
    return 1;
}

int sdhci_init(void)
{
    int found = 0;
    for (int i = 0; i < pci_count(); i++)
    {
        const PciDevice *pd = pci_get(i);
        if (pd->class_code != 0x08 || pd->subclass != 0x05) /* SD host controller */
            continue;
        u32 cmd = pci_cfg_read(pd->bus, pd->dev, pd->fn, 4);
        pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (cmd & 0xFFFFu) | 0x6u); /* memory space + bus master */
        u32 info = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x40); /* slot information: first BAR in bits 2:0, slots - 1 in 6:4 */
        unsigned bar0 = info & 7u, slots = ((info >> 4) & 7u) + 1;
        if (bar0 > 5)
            bar0 = 0;
        for (unsigned sl = 0; sl < slots && bar0 + sl < 6; sl++)
        {
            u32 v = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10 + 4 * (bar0 + sl));
            if ((v & 1u) || !(v & ~0xFu)) /* I/O ports or not assigned */
                continue;
            u64 base = v & ~0xFULL;
            if (((v >> 1) & 3u) == 2u && bar0 + sl < 5) /* 64 bit BAR */
                base |= (u64)pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10 + 4 * (bar0 + sl + 1)) << 32;
            found += bring_up((volatile u8 *)base);
        }
    }
    return found;
}
