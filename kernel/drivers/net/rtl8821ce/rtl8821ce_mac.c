/* RTL8821CE: rtw_mac_init, the MAC queues and protocol timing (stage 4b-1). */
#include "rtl8821ce_internal.h"

/* ---- stage 4b-1: rtw_mac_init, the MAC's queues and protocol timing (Linux rtw_mac.c + rtw8821c_mac_init) ----
 * What it does, in short: the chip's 64 KiB TX packet memory is cut into 128-byte pages; the top pages are reserved for the firmware's own
 * use (H2C queue, ...), the rest is shared by the transmit queues. The firmware commands (H2C) get a ring inside the reserved pages. Then
 * the timing constants of the 802.11 MAC (slot, SIFS, TXOP limits) and the receive filter are set. No radio is configured here. */
#define R_TX_HANG_CTRL 0x045E
#define R_AMPDU_MAX_TIME_V1 0x0455
#define R_PRECNT_CTRL 0x04E5
#define R_PROT_MODE_CTRL 0x04C8
#define R_BAR_MODE_CTRL 0x04CC
#define R_FAST_EDCA_VOVI 0x1448
#define R_FAST_EDCA_BEBK 0x144C
#define R_INIRTS_RATE_SEL 0x0480
#define R_TIMER0_SRC_SEL 0x05B4
#define R_SLOT 0x051B
#define R_PIFS 0x0512
#define R_SIFS 0x0514
#define R_EDCA_VO_PARAM 0x0500
#define R_EDCA_VI_PARAM 0x0504
#define R_RD_NAV_NXT 0x0544
#define R_RXTSF_OFFSET_CCK 0x055E
#define R_TBTT_PROHIBIT 0x0540
#define R_DRVERLYINT 0x0558
#define R_BCNDMATIM 0x0559
#define R_TX_PTCL_CTRL 0x0520
#define R_RXFLTMAP0 0x06A0
#define R_RXFLTMAP1 0x06A2
#define R_RXFLTMAP2 0x06A4
#define R_RX_PKT_LIMIT 0x060C
#define R_RX_DRVINFO_SZ 0x060F
#define R_TCR 0x0604
#define R_ACKTO_CCK 0x0639
#define R_WMAC_TRXPTCL_CTL_H 0x066C
#define R_SND_PTCL_CTRL 0x0718
#define R_WMAC_OPTION_FUNCTION 0x07D0
#define R_TRXFF_BNDY 0x0114
#define R_RXFF_BNDY 0x011C
#define R_AUTO_LLT_V1 0x0208
#define R_BCNQ_BDNY_V1 0x0424
#define R_BCNQ1_BDNY_V1 0x0456
#define R_H2C_HEAD 0x0244
#define R_H2C_TAIL 0x0248
#define R_H2C_READ_ADDR 0x024C
#define R_H2C_INFO 0x0254
#define R_H2C_PKT_READADDR 0x10D0
#define R_H2C_PKT_WRITEADDR 0x10D4
#define R_TXDMA_OFFSET_CHK 0x020C

/* the geometry of this chip (rtw8821c_hw_spec, page_table_8821c[1], rqpn_table_8821c[1]) */
#define TX_PAGE_SIZE_SHIFT 7
#define TXFF_PAGES (65536 >> TX_PAGE_SIZE_SHIFT)
#define RSVD_PAGES (8 + 24 + 8 + 8 + 0 + 4 + 0)         /* driver, H2C extra info, H2C static info, H2C queue, CPU instructions, FW TX buffer, CSI */
#define RSVD_BOUNDARY (TXFF_PAGES - RSVD_PAGES)         /* the first reserved page: 460 */
#define H2CQ_PAGE (TXFF_PAGES - 0 - 4 - 0 - 8)          /* below the CSI (0), FW TX buffer (4) and CPU instructions (0) */
#define H2CQ_SIZE (8 << TX_PAGE_SIZE_SHIFT)
#define RXFF_SIZE 16384
#define C2H_PKT_BUF 256
#define PAGES_HQ 16
#define PAGES_LQ 16
#define PAGES_NQ 16
#define PAGES_EXQ 14
#define PAGES_GAPQ 1
#define PAGES_PUBQ (RSVD_BOUNDARY - PAGES_HQ - PAGES_LQ - PAGES_NQ - PAGES_EXQ - PAGES_GAPQ)
/* queue -> DMA priority queue (0 extra, 1 low, 2 normal, 3 high), the PCIe row */
#define DMA_VO 2
#define DMA_VI 2
#define DMA_BE 1
#define DMA_BK 1
#define DMA_MG 0
#define DMA_HI 3
#define TXDMA_PQ_MAP ((DMA_HI << 14) | (DMA_MG << 12) | (DMA_BK << 10) | (DMA_BE << 8) | (DMA_VI << 6) | (DMA_VO << 4))

/* the protocol constants of rtw8821c.h */
#define WLAN_SLOT_TIME 0x09
#define WLAN_PIFS_TIME 0x19
#define WLAN_SIFS_CCK_CONT_TX 0xA
#define WLAN_SIFS_OFDM_CONT_TX 0xE
#define WLAN_SIFS_CCK_TRX 0x10
#define WLAN_SIFS_OFDM_TRX 0x10
#define WLAN_VO_TXOP_LIMIT 0x186
#define WLAN_VI_TXOP_LIMIT 0x3BC
#define WLAN_RDG_NAV 0x05
#define WLAN_TXOP_NAV 0x1B
#define WLAN_CCK_RX_TSF 0x30
#define WLAN_OFDM_RX_TSF 0x30
#define WLAN_TBTT_PROHIBIT 0x04
#define WLAN_TBTT_HOLD_TIME 0x064
#define WLAN_DRV_EARLY_INT 0x04
#define WLAN_BCN_DMA_TIME 0x02
#define WLAN_RX_FILTER0 0xFFFF
#define WLAN_RX_FILTER1 0x0FFF
#define WLAN_RX_FILTER2 0xFFFF
#define WLAN_RCR_CFG 0xE400220E
#define WLAN_RXPKT_MAX_SZ 12288
#define WLAN_RXPKT_MAX_SZ_512 (WLAN_RXPKT_MAX_SZ >> 9)
#define WLAN_AMPDU_MAX_TIME 0x70
#define WLAN_RTS_LEN_TH 0xFF
#define WLAN_RTS_TX_TIME_TH 0x08
#define WLAN_MAX_AGG_PKT_LIMIT 0x20
#define WLAN_RTS_MAX_AGG_PKT_LIMIT 0x20
#define FAST_EDCA_TH 0x06 /* VO, VI, BE and BK all use 6 */
#define WLAN_BAR_RETRY_LIMIT 0x01
#define WLAN_RA_TRY_RATE_AGG_LIMIT 0x08
#define WLAN_TX_FUNC_CFG1 0x30
#define WLAN_TX_FUNC_CFG2 0x30
#define WLAN_MAC_OPT_NORM_FUNC1 0x98
#define WLAN_MAC_OPT_FUNC2 0xb0810041
#define WLAN_PRE_TXCNT_TIME_TH 0x1E4
#define WLAN_SIFS_CFG (WLAN_SIFS_CCK_CONT_TX | (WLAN_SIFS_OFDM_CONT_TX << 8) | (WLAN_SIFS_CCK_TRX << 16) | (WLAN_SIFS_OFDM_TRX << 24))
#define WLAN_TBTT_TIME (WLAN_TBTT_PROHIBIT | (WLAN_TBTT_HOLD_TIME << 8))
#define WLAN_NAV_CFG (WLAN_RDG_NAV | (WLAN_TXOP_NAV << 16))
#define WLAN_RX_TSF_CFG (WLAN_CCK_RX_TSF | (WLAN_OFDM_RX_TSF) << 8)

Anon_MI MI;

void or32(u32 off, u32 bits) { wr32(off, rd32(off) | bits); }
void andn32(u32 off, u32 bits) { wr32(off, rd32(off) & ~bits); }

/* rtw_init_trx_cfg: txdma_queue_mapping + priority_queue_cfg + init_h2c. 0 = ok, else the reason. */
static const char *init_trx_cfg(void)
{
    /* txdma_queue_mapping: which transmit queue feeds which priority queue; then the DMA engines and the MAC switch on */
    wr16(R_TXDMA_PQ_MAP, TXDMA_PQ_MAP);
    wr8(R_CR, 0);
    wr8(R_CR, MAC_TRX_ENABLE);
    wr32(R_H2CQ_CSR, BIT(31)); /* BIT_H2CQ_FULL, the CPU is the 3081 kind */

    /* priority_queue_cfg: how many pages each queue owns, where the reserved ones begin, and the chip builds its page chain itself */
    wr16(R_FIFOPAGE_INFO_1, PAGES_HQ);
    wr16(R_FIFOPAGE_INFO_1 + 4, PAGES_LQ);
    wr16(R_FIFOPAGE_INFO_1 + 8, PAGES_NQ);
    wr16(R_FIFOPAGE_INFO_1 + 12, PAGES_EXQ);
    wr16(R_FIFOPAGE_INFO_1 + 16, PAGES_PUBQ);
    or32(R_RQPN_CTRL_2, BIT(31)); /* BIT_LD_RQPN: load the numbers */
    wr16(R_FIFOPAGE_CTRL_2, RSVD_BOUNDARY);
    set8(R_FWHW_TXQ_CTRL + 2, (u8)(BIT(20) >> 16)); /* BIT_EN_WR_FREE_TAIL */
    wr16(R_BCNQ_BDNY_V1, RSVD_BOUNDARY);
    wr16(R_FIFOPAGE_CTRL_2 + 2, RSVD_BOUNDARY);
    wr16(R_BCNQ1_BDNY_V1, RSVD_BOUNDARY);
    wr32(R_RXFF_BNDY, RXFF_SIZE - C2H_PKT_BUF - 1);
    set8(R_AUTO_LLT_V1, BIT(0)); /* BIT_AUTO_INIT_LLT_V1: the chip links its pages, then clears the bit */
    if (!ready32(R_AUTO_LLT_V1, BIT(0), 0, 1000))
        return "llt";
    wr8(R_CR + 3, 0);

    /* init_h2c: the ring of firmware commands lives in the reserved pages */
    u32 head = H2CQ_PAGE << TX_PAGE_SIZE_SHIFT;
    wr32(R_H2C_HEAD, (rd32(R_H2C_HEAD) & 0xFFFC0000u) | head);
    wr32(R_H2C_READ_ADDR, (rd32(R_H2C_READ_ADDR) & 0xFFFC0000u) | head);
    wr32(R_H2C_TAIL, (rd32(R_H2C_TAIL) & 0xFFFC0000u) | (head + H2CQ_SIZE));
    wr8(R_H2C_INFO, (u8)((rd8(R_H2C_INFO) & 0xFC) | 0x01));
    wr8(R_H2C_INFO, (u8)((rd8(R_H2C_INFO) & 0xFB) | 0x04));
    wr8(R_TXDMA_OFFSET_CHK + 1, (u8)((rd8(R_TXDMA_OFFSET_CHK + 1) & 0x7F) | 0x80));
    u32 wp = rd32(R_H2C_PKT_WRITEADDR) & 0x3FFFF, rp = rd32(R_H2C_PKT_READADDR) & 0x3FFFF;
    u32 free_bytes = wp >= rp ? H2CQ_SIZE - (wp - rp) : rp - wp;
    if (free_bytes != H2CQ_SIZE) /* the ring must start empty */
        return "h2c";
    return 0;
}

/* rtw8821c_mac_init: protocol parameters, EDCA timing, beacon timing and the receive filter */
static void init_wmac(void)
{
    wr8(R_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
    set8(R_TX_HANG_CTRL, BIT(2)); /* BIT_EN_EOF_V1 */
    u16 pre_txcnt = WLAN_PRE_TXCNT_TIME_TH | BIT(11); /* BIT_EN_PRECNT */
    wr8(R_PRECNT_CTRL, (u8)(pre_txcnt & 0xFF));
    wr8(R_PRECNT_CTRL + 1, (u8)(pre_txcnt >> 8));
    wr32(R_PROT_MODE_CTRL, WLAN_RTS_LEN_TH | (WLAN_RTS_TX_TIME_TH << 8) | (WLAN_MAX_AGG_PKT_LIMIT << 16) | (WLAN_RTS_MAX_AGG_PKT_LIMIT << 24));
    wr16(R_BAR_MODE_CTRL + 2, WLAN_BAR_RETRY_LIMIT | WLAN_RA_TRY_RATE_AGG_LIMIT << 8);
    wr8(R_FAST_EDCA_VOVI, FAST_EDCA_TH);
    wr8(R_FAST_EDCA_VOVI + 2, FAST_EDCA_TH);
    wr8(R_FAST_EDCA_BEBK, FAST_EDCA_TH);
    wr8(R_FAST_EDCA_BEBK + 2, FAST_EDCA_TH);
    set8(R_INIRTS_RATE_SEL, BIT(5));

    clr8(R_TIMER0_SRC_SEL, BIT(4) | BIT(5) | BIT(6)); /* BIT_TSFT_SEL_TIMER0 */
    wr16(R_TXPAUSE, 0);
    wr8(R_SLOT, WLAN_SLOT_TIME);
    wr8(R_PIFS, WLAN_PIFS_TIME);
    wr32(R_SIFS, WLAN_SIFS_CFG);
    wr16(R_EDCA_VO_PARAM + 2, WLAN_VO_TXOP_LIMIT);
    wr16(R_EDCA_VI_PARAM + 2, WLAN_VI_TXOP_LIMIT);
    wr32(R_RD_NAV_NXT, WLAN_NAV_CFG);
    wr16(R_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);

    set8(R_BCN_CTRL, BIT(3)); /* BIT_EN_BCN_FUNCTION: the TSF timer runs */
    wr32(R_TBTT_PROHIBIT, WLAN_TBTT_TIME);
    wr8(R_DRVERLYINT, WLAN_DRV_EARLY_INT);
    wr8(R_BCNDMATIM, WLAN_BCN_DMA_TIME);
    clr8(R_TX_PTCL_CTRL + 1, (u8)(BIT(12) >> 8)); /* BIT_SIFS_BK_EN */

    wr16(R_RXFLTMAP0, WLAN_RX_FILTER0);
    wr16(R_RXFLTMAP1, WLAN_RX_FILTER1);
    wr16(R_RXFLTMAP2, WLAN_RX_FILTER2);
    wr32(R_RCR, WLAN_RCR_CFG);
    wr8(R_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
    wr8(R_TCR + 2, WLAN_TX_FUNC_CFG2);
    wr8(R_TCR + 1, WLAN_TX_FUNC_CFG1);
    wr8(R_ACKTO_CCK, 0x40);
    set8(R_WMAC_TRXPTCL_CTL_H, BIT(1));
    set8(R_SND_PTCL_CTRL, BIT(6)); /* BIT_DIS_CHK_VHTSIGB_CRC */
    wr32(R_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
    wr8(R_WMAC_OPTION_FUNCTION + 4, WLAN_MAC_OPT_NORM_FUNC1);
}

/* rtw_drv_info_cfg: every received frame carries a 4 * 8 byte PHY status in front of it */
static void init_drv_info(void)
{
    wr8(R_RX_DRVINFO_SZ, 4);
    wr8(R_TRXFF_BNDY + 1, (u8)((rd8(R_TRXFF_BNDY + 1) & 0xF0) | 0xF)); /* "for the rxdesc len = 0 issue" */
    or32(R_RCR, BIT(28));                                              /* BIT_APP_PHYSTS */
    andn32(R_WMAC_OPTION_FUNCTION + 4, BIT(8) | BIT(9));
}

void mac_init(void)
{
    MI.state = MI_FAILED;
    MI.why = init_trx_cfg();
    if (MI.why)
        return;
    init_wmac();
    init_drv_info();
    MI.cr = rd8(R_CR);
    MI.state = MI_OK;
}
