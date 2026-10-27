/* RTL8821CE: the card's own data in its efuse (stage 4a). */
#include "rtl8821ce_internal.h"

/* ---- stage 4a: the card's own data in its efuse ----
 * Every chip carries a small one-time-programmable memory: 512 bytes, "physical map". It holds the data of the board in blocks, each with
 * a header (rtw88 efuse.c): the 1-byte header is block[3:0] in bits 7..4 and word_en[3:0] in bits 3..0; when the low 5 bits of the first
 * byte are 0xF it is the 2-byte form, block[2:0] in bits 7..5 of the first byte, block[6:3] in bits 7..4 of the second, word_en in the low 4
 * bits of the second. word_en has a 0 for each of the four 16-bit words of the 8-byte block that follows in the map. Blocks fill a 512 byte
 * "logical map"; the header 0xFF ends the list. In the logical map the card's MAC address is at offset 0xD0 (struct rtw8821c_efuse). */
#define R_EFUSE_CTRL 0x0030
#define R_LDO_EFUSE_CTRL 0x0034
#define EFUSE_PHYSICAL 512
#define EFUSE_PROTECT 96   /* the last bytes of the physical map are protected: no blocks there */
#define EFUSE_LOGICAL 512
#define EFUSE_MAC_AT 0xD0

Anon_EF EF;

/* rtw_dump_logical_efuse_map. 0 = ok, -1 = the blocks do not fit (corrupt map). `log` must be filled with 0xFF by the caller. */
static int efuse_logical(const u8 *phy, u8 *log)
{
    const unsigned limit = EFUSE_PHYSICAL - EFUSE_PROTECT;
    unsigned idx = 0;
    while (idx < limit)
    {
        u8 hdr1 = phy[idx], hdr2 = phy[idx + 1];
        if (hdr1 == 0xFF || ((hdr1 & 0x1F) == 0xF && hdr2 == 0xFF)) /* end of the list */
            break;
        unsigned blk, word_en;
        if ((hdr1 & 0x1F) == 0xF) /* 2 byte header */
        {
            blk = (((unsigned)hdr2 & 0xF0) >> 1) | (((unsigned)hdr1 >> 5) & 7);
            word_en = hdr2 & 0xF;
            idx += 2;
        }
        else
        {
            blk = ((unsigned)hdr1 & 0xF0) >> 4;
            word_en = hdr1 & 0xF;
            idx += 1;
        }
        for (int i = 0; i < 4; i++)
        {
            if (word_en & BIT(i)) /* this word is not written */
                continue;
            unsigned at = (blk << 3) + ((unsigned)i << 1);
            if (idx + 1 > limit || at + 1 > EFUSE_LOGICAL)
                return -1;
            log[at] = phy[idx];
            log[at + 1] = phy[idx + 1];
            idx += 2;
        }
    }
    return 0;
}

/* rtw_dump_physical_efuse_map: read the 512 bytes one by one through EFUSE_CTRL. The chip must be powered on. */
static int efuse_physical(u8 *map)
{
    wr32(R_LDO_EFUSE_CTRL, rd32(R_LDO_EFUSE_CTRL) & ~(BIT(8) | BIT(9))); /* bank 0: the WLAN part */
    wr8(R_LDO_EFUSE_CTRL + 3, (u8)(rd8(R_LDO_EFUSE_CTRL + 3) & ~BIT(7))); /* the 2.5 V supply for writing is off while reading */
    u32 ctl = rd32(R_EFUSE_CTRL);
    for (unsigned addr = 0; addr < EFUSE_PHYSICAL; addr++)
    {
        ctl &= ~(0xFFu | (0x3FFu << 8));
        ctl |= (addr & 0x3FF) << 8;
        wr32(R_EFUSE_CTRL, ctl & ~BIT(31)); /* ask for this address: BIT_EF_FLAG clear, the chip sets it when the data is there */
        int got = 0;
        for (int i = 0; i < 1000000 && !got; i++)
        {
            udelay(1);
            ctl = rd32(R_EFUSE_CTRL);
            got = (ctl & BIT(31)) != 0;
        }
        if (!got)
            return -1;
        map[addr] = (u8)(ctl & 0xFF);
    }
    return 0;
}

static int valid_mac(const u8 *m)
{
    int zero = 1;
    for (int i = 0; i < 6; i++)
        zero &= m[i] == 0;
    return !zero && !(m[0] & 1);
}

void efuse_read_mac(void)
{
    u8 phy[EFUSE_PHYSICAL], log[EFUSE_LOGICAL];
    EF.map_ok = 0;
    EF.state = EF_FAILED;
    EF.why = 0;
    if (efuse_physical(phy))
    {
        EF.why = "read timeout";
        return;
    }
    volatile u8 *l = log;
    for (int i = 0; i < EFUSE_LOGICAL; i++)
        l[i] = 0xFF;
    if (efuse_logical(phy, log))
    {
        EF.why = "corrupt map";
        return;
    }
    EF.map_ok = 1;
    EF.rfe = log[0xCA] & 0x1F;
    EF.pkg = (log[0xCA] >> 5) & 1;
    EF.xtal = log[0xB9];
    EF.swing2g = log[0xC6];
    EF.swing5g = log[0xC7];
    for (int i = 0; i < 42; i++)
        EF.txpwr[i] = log[0x10 + i];
    for (int i = 0; i < 6; i++)
        EF.mac[i] = log[EFUSE_MAC_AT + i];
    if (!valid_mac(EF.mac))
    {
        EF.why = "no valid MAC";
        return;
    }
    EF.state = EF_OK;
}

/* the outcome of the firmware download, for the Device Manager line */
Anon_DL DL;

/* Sends one piece, already in D.stage + TX_DESC_SIZE, to the transmit buffer at page 0 through the beacon queue
 * (rtw_fw_write_data_rsvd_page, rtw_pci_write_data_rsvd_page, rtw_tx_fill_tx_desc). 0 = done. */
static int send_piece(unsigned size)
{
    u8 bcn_ctrl = rd8(R_BCN_CTRL);
    wr16(R_FIFOPAGE_CTRL_2, (u16)(0 | BIT(15)));       /* the "beacon" starts at page 0; BIT_BCN_VALID_V1 is cleared by writing 1 */
    u8 cr1 = rd8(R_CR + 1);
    wr8(R_CR + 1, (u8)(cr1 | 1));                      /* BIT_ENSWBCN >> 8 */
    wr8(R_BCN_CTRL, (u8)((bcn_ctrl & ~BIT(3)) | BIT(4))); /* beacon function off, no TSF update */
    u8 txq2 = rd8(R_FWHW_TXQ_CTRL + 2);
    wr8(R_FWHW_TXQ_CTRL + 2, (u8)(txq2 & ~BIT(6)));    /* BIT_EN_BCNQ_DL >> 16 */

    /* the transmit descriptor (48 bytes, 10 words used) */
    u8 *d = D.stage;
    zero_bytes(d, TX_DESC_SIZE);
    unsigned bmc = size > 4 ? (D.stage[TX_DESC_SIZE + 4] & 1u) : 0; /* Linux reads the "destination address" of the piece: a multicast bit */
    put32(d + 0, size | (TX_DESC_SIZE << 16) | (bmc << 24) | BIT(26) | BIT(31)); /* size, offset, bmc, last segment, no queue/sequence select */
    put32(d + 4, (16u << 8) | (7u << 16));              /* queue select 16 = beacon, rate id 7 (RTW_RATEID_G) */
    put32(d + 12, BIT(8) | BIT(10));                    /* use the rate in the descriptor, no rate fallback */
    put32(d + 16, 4);                                   /* DESC_RATE6M */
    put32(d + 32, BIT(15));                             /* hardware sequence number */

    /* the buffer descriptor of the beacon queue: entry 0 = the transmit descriptor, entry 1 = the data; OWN bit in the length field */
    u8 *bd = D.mem + Q_BCN * PAGE;
    zero_bytes(bd, 16);
    unsigned psb = ((TX_DESC_SIZE + size - 1) / 128 + 1) | BIT(TXBD_OWN_BIT);
    put16(bd + 0, TX_DESC_SIZE);
    put16(bd + 2, psb);
    put32(bd + 4, D.stage_dma);
    put16(bd + 8, size);
    put16(bd + 10, 0);
    put32(bd + 12, D.stage_dma + TX_DESC_SIZE);
    fence();
    set8(R_TXBD_BCN_WORK, BIT(4));                     /* BIT_PCI_BCNQ_FLAG: go */

    int ok = ready32(R_FIFOPAGE_CTRL_2, BIT(15), BIT(15), 1000); /* the chip says the "beacon" is in its buffer */
    wr16(R_FIFOPAGE_CTRL_2, (u16)(0 | BIT(15)));       /* rsvd_boundary is still 0 at this point of Linux' start-up */
    wr8(R_BCN_CTRL, bcn_ctrl);
    wr8(R_FWHW_TXQ_CTRL + 2, txq2);
    wr8(R_CR + 1, cr1);
    return ok ? 0 : -1;
}

/* iddma_download_firmware: copy `len` bytes from the transmit buffer (page 0, behind the descriptor) to `dst` in the chip's memory. */
static int ddma_piece(u32 dst, unsigned len, int first)
{
    if (!ready32(R_DDMA_CH0CTRL, BIT(31), 0, 1000))
        return -1;
    u32 ctrl = BIT(29) | BIT(31) | (len & 0x3FFFFu) | (first ? 0 : BIT(24)); /* checksum on, start, [continue the checksum] */
    wr32(R_DDMA_CH0SA, OCP_TXBUF + TX_DESC_SIZE);
    wr32(R_DDMA_CH0DA, dst);
    wr32(R_DDMA_CH0CTRL, ctrl);
    return ready32(R_DDMA_CH0CTRL, BIT(31), 0, 1000) ? 0 : -1;
}

/* check_fw_checksum: the DDMA engine adds up what it copied; the result goes into MCUFW_CTRL as "downloaded" / "checksum ok" bits. */
static int check_checksum(u32 dst)
{
    u8 fw_ctrl = rd8(R_MCUFW_CTRL);
    int imem = dst < OCP_DMEM;
    if (rd32(R_DDMA_CH0CTRL) & BIT(27)) /* BIT_DDMACH0_CHKSUM_STS: a mismatch */
    {
        fw_ctrl = (u8)(fw_ctrl | (imem ? BIT(3) : BIT(5)));
        fw_ctrl = (u8)(fw_ctrl & ~(imem ? BIT(4) : BIT(6)));
        wr8(R_MCUFW_CTRL, fw_ctrl);
        return 0;
    }
    fw_ctrl = (u8)(fw_ctrl | (imem ? (BIT(3) | BIT(4)) : (BIT(5) | BIT(6))));
    wr8(R_MCUFW_CTRL, fw_ctrl);
    return 1;
}

/* download_firmware_to_mem: one part of the file, in pieces */
static int download_part(int part, unsigned file_off, unsigned size, u32 dst)
{
    DL.part = part;
    DL.piece = 1;
    wr32(R_DDMA_CH0CTRL, rd32(R_DDMA_CH0CTRL) | BIT(25)); /* reset the checksum status */
    int first = 1;
    for (unsigned off = 0; off < size;)
    {
        unsigned n = size - off < CHUNK_MAX ? size - off : CHUNK_MAX;
        DL.off = off;
        if (vfs_read(FW.node, file_off + off, D.stage + TX_DESC_SIZE, n) != (int)n)
        {
            DL.why = "file read";
            return -1;
        }
        if (send_piece(n))
        {
            DL.why = "send timeout";
            return -1;
        }
        if (ddma_piece(dst + off, n, first))
        {
            DL.why = "ddma timeout";
            return -1;
        }
        first = 0;
        off += n;
    }
    if (!check_checksum(dst))
    {
        DL.why = "checksum";
        return -1;
    }
    return 0;
}

typedef struct
{
    unsigned len, reg;
    u32 val;
} Backup;

static void backup_restore(const Backup *b, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (b[i].len == 1)
            wr8(b[i].reg, (u8)b[i].val);
        else if (b[i].len == 2)
            wr16(b[i].reg, (u16)b[i].val);
        else
            wr32(b[i].reg, b[i].val);
    }
}

static void wlan_cpu_enable(int enable)
{
    if (enable)
    {
        set8(R_RSV_CTRL + 1, BIT(0));       /* the CPU's I/O interface */
        set8(R_SYS_FUNC_EN + 1, BIT(2));    /* the CPU itself (BIT_FEN_CPUEN) */
    }
    else
    {
        clr8(R_SYS_FUNC_EN + 1, BIT(2));
        clr8(R_RSV_CTRL + 1, BIT(0));
    }
}

/* __rtw_download_firmware. 0 = the firmware runs; else DL.why says what went wrong. (The Bluetooth coexistence register that Linux saves
 * and restores around this is left alone.) */
int fw_download(void)
{
    Backup bk[6];
    wlan_cpu_enable(0);

    /* download_firmware_reg_backup: use only the high priority queue, give it pages, switch the beacon functions off */
    bk[0] = (Backup){1, R_TXDMA_PQ_MAP + 1, rd8(R_TXDMA_PQ_MAP + 1)};
    wr8(R_TXDMA_PQ_MAP + 1, 3u << 6);                 /* RTW_DMA_MAPPING_HIGH << 6 */
    bk[1] = (Backup){1, R_CR, rd8(R_CR)};
    bk[2] = (Backup){4, R_H2CQ_CSR, BIT(31)};          /* BIT_H2CQ_FULL (not read: this is the value to leave behind) */
    wr8(R_CR, BIT(0) | BIT(2));                        /* BIT_HCI_TXDMA_EN | BIT_TXDMA_EN */
    wr32(R_H2CQ_CSR, BIT(31));
    bk[3] = (Backup){2, R_FIFOPAGE_INFO_1, rd16(R_FIFOPAGE_INFO_1)};
    bk[4] = (Backup){4, R_RQPN_CTRL_2, rd32(R_RQPN_CTRL_2) | BIT(31)}; /* BIT_LD_RQPN */
    wr16(R_FIFOPAGE_INFO_1, 0x200);
    wr32(R_RQPN_CTRL_2, bk[4].val);
    u8 bcn = rd8(R_BCN_CTRL);
    bk[5] = (Backup){1, R_BCN_CTRL, bcn};
    wr8(R_BCN_CTRL, (u8)((bcn & ~BIT(3)) | BIT(4)));

    /* download_firmware_reset_platform */
    clr8(R_CPU_DMEM_CON + 2, BIT(0));
    clr8(R_SYS_CLK_CTRL + 1, BIT(6));
    set8(R_CPU_DMEM_CON + 2, BIT(0));
    set8(R_SYS_CLK_CTRL + 1, BIT(6));

    /* start_download_firmware: DMEM, IMEM, then EMEM when there is one */
    wr16(R_MCUFW_CTRL, (u16)((rd16(R_MCUFW_CTRL) & 0x3800) | BIT(0))); /* BIT_MCUFWDL_EN */
    unsigned dmem = FW.dmem + FW_CHKSUM_SIZE, imem = FW.imem + FW_CHKSUM_SIZE, emem = FW.emem ? FW.emem + FW_CHKSUM_SIZE : 0;
    int bad = download_part(0, FW_HDR_SIZE, dmem, FW.dmem_addr & ~BIT(31));
    if (!bad)
        bad = download_part(1, FW_HDR_SIZE + dmem, imem, FW.imem_addr & ~BIT(31));
    if (!bad && emem)
        bad = download_part(2, FW_HDR_SIZE + dmem + imem, emem, FW.emem_addr & ~BIT(31));
    if (bad)
    {
        clr8(R_MCUFW_CTRL, BIT(0));
        set8(R_SYS_FUNC_EN + 1, BIT(2));
        backup_restore(bk, 6);
        return -1;
    }
    backup_restore(bk, 6);

    /* download_firmware_end_flow */
    wr32(R_TXDMA_STATUS, BIT(2));                      /* BTI_PAGE_OVF */
    u16 ctrl = rd16(R_MCUFW_CTRL);
    if ((ctrl & (BIT(4) | BIT(6))) == (BIT(4) | BIT(6)))
        wr16(R_MCUFW_CTRL, (u16)((ctrl | BIT(14)) & ~BIT(0))); /* BIT_FW_DW_RDY, download finished */
    wlan_cpu_enable(1);

    /* download_firmware_validate: the CPU runs the firmware, which says so (FW_READY) */
    DL.piece = 0;
    if (!ready32(R_MCUFW_CTRL, 0xCFFF, 0xC078, 3000))
    {
        DL.why = "firmware not ready";
        clr8(R_MCUFW_CTRL, BIT(0));
        set8(R_SYS_FUNC_EN + 1, BIT(2));
        return -1;
    }
    hci_setup(); /* reset the descriptors and indexes */
    return 0;
}
