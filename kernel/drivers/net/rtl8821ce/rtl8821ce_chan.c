/* RTL8821CE: firmware commands (H2C), antenna path and channel (stages 4b-3, 4c-1). */
#include "rtl8821ce_internal.h"

/* ---- stage 4b-3: the first firmware command (H2C through the mailbox registers) ----
 * A short command goes to the firmware through one of four mailboxes: two 32-bit registers each (HMEBOX0_EX = bytes 4..7, HMEBOX0 =
 * bytes 0..3, written last). The chip sets the box's bit in HMETFR when the low word is written and the firmware clears it when it has
 * read the command: that is the proof that the firmware runs and listens. The command used is "tell the Bluetooth coexistence code not
 * to ignore WLAN actions, off" (H2C 0x63), the default state: it changes nothing and the firmware answers it with no packet (the
 * receive DMA is still off). The same sequence as rtw_fw_send_h2c_command(). */
#define H2C_CMD_IGNORE_WLAN_ACTION 0x63

Anon_H2 H2;

static int h2c_box_free(void)
{
    for (int i = 0; i < 30; i++) /* 3 ms in steps of 100 us, like Linux */
    {
        if (!(rd8(R_HMETFR) & BIT(0)))
            return 1;
        udelay(100);
    }
    return 0;
}

unsigned H2C_BOX; /* the mailbox the next command goes into: the firmware takes them in turn, 0 to 3, as Linux rotates through them (see h2c_send) */

void h2c_probe(void)
{
    H2.state = H2_FAILED;
    H2C_BOX = 0; /* (a firmware just started) */
    H2.why = 0;
    if (!h2c_box_free())
    {
        H2.why = "busy";
        return;
    }
    wr32(R_HMEBOX0_EX, 0);
    wr32(R_HMEBOX0, H2C_CMD_IGNORE_WLAN_ACTION); /* bit 8 = enable = 0 */
    H2C_BOX = 1;                                 /* the first mailbox is used */
    for (int i = 0; i < 200; i++) /* up to 20 ms for the firmware to take it */
    {
        if (!(rd8(R_HMETFR) & BIT(0)))
        {
            H2.state = H2_OK;
            return;
        }
        udelay(100);
    }
    H2.why = "no ack";
}

/* ---- stage 4c-1: antenna path ("Wi-Fi only") and channel ----
 * Wi-Fi and Bluetooth share the radio front end of this card. Two things must be told to the chip before a frame can be received:
 *   - who owns the antenna: the Bluetooth coexistence block has "grant" lines (GNT_WL / GNT_BT) and a switch (DPDT, driven by the RFE
 *     control pins) that points the antenna at Wi-Fi or at Bluetooth. This is the part of Linux' rtw_coex_set_ant_path() for the phase
 *     "Wi-Fi only" (GNT_BT forced low, GNT_WL forced high, the switch to Wi-Fi), without the Bluetooth bookkeeping around it;
 *   - which channel: rtw8821c_set_channel(): baseband, RF synthesizer and MAC clock settings for the channel, 20 MHz wide.
 * The grant lines are in a register of the Bluetooth/LTE coexistence block that is only reachable through an indirect interface
 * (0x1700 control, 0x1704 data to write, 0x1708 data read). */
#define R_AFE_CTRL1 0x0024
#define R_SYS_CTRL 0x0000
#define R_SYS_SDIO_CTRL 0x0070
#define R_CTRL_TYPE 0x0067
#define R_DATA_SC 0x0483
#define R_USTIME_TSF 0x055C
#define R_USTIME_EDCA 0x0638
#define R_WMAC_TRXPTCL_CTL 0x0668
#define R_CCK_CHECK 0x0454
#define R_RXCCAMSK 0x0814
#define R_CCA_FLTR 0x0A20
#define R_ENTXCCK 0x0A80
#define R_ENRXCCA 0x0A84
#define R_TXSF2 0x0A24
#define R_TXSF6 0x0A28
#define R_TXFILTER 0x0AAC
#define R_CLKTRK 0x0860
#define R_ADCCLK 0x08AC
#define R_ADC160 0x08C4
#define R_CHFIR 0x08F0
#define R_RXSB 0x0A00
#define R_ACBB0 0x0948
#define R_ACBBRXFIR 0x094C
#define R_TXDFIR 0x0C20
#define R_TXSCALE_A 0x0C1C
#define R_RFECTL 0x0CB8
#define R_RFE_CTRL8 0x0CB4
#define R_DMEM_CTRL 0x1080
#define R_LTECOEX_CTRL 0x1700
#define R_LTECOEX_WDATA 0x1704
#define R_LTECOEX_RDATA 0x1708
#define LTECOEX_READY BIT(29)
#define LTE_COEX_CTRL 0x38 /* the register of the coexistence block that holds the grant lines */
#define RF_LUTDBG 0xDF
#define RF_XTALX2 0xB8
#define RF18_BAND_2G 0x0
#define RF18_BAND_5G 0x10100
#define RF18_BAND_MASK 0x10300
#define RF18_RFSI_GE 0x20000
#define RF18_RFSI_GT 0x40000
#define RF18_RFSI_MASK 0x60000
#define RF18_BW_MASK 0xC00
#define RF18_BW_20M 0xC00
#define B_BTG_SWITCH 0x10000
#define B_CTRL_SWITCH 0x40000
#define B_WL_SWITCH 0x500000
#define B_WLG_SWITCH 0x200000
#define B_WLA_SWITCH 0x800000
#define BTG_CCA 0xE
#define BTG_LNA 0xFC84
#define WLG_CCA 0x12
#define WLG_LNA 0x7532
#define DPDT_CTRL_PIN 0x77
#define PTA_CTRL_PIN 0x66
#define ANTDIC_CTRL_PIN 0x88
#define MAC_CLK_SPEED 0x50

/* the indirect interface: read or write one 32-bit register of the coexistence block; 0 = ok */
static int lte_read(u16 off, u32 *val)
{
    if (!ready32(R_LTECOEX_CTRL, LTECOEX_READY, LTECOEX_READY, 1000))
        return -1;
    wr32(R_LTECOEX_CTRL, 0x800F0000u | off);
    *val = rd32(R_LTECOEX_RDATA);
    return 0;
}

static int lte_write(u16 off, u32 val)
{
    if (!ready32(R_LTECOEX_CTRL, LTECOEX_READY, LTECOEX_READY, 1000))
        return -1;
    wr32(R_LTECOEX_WDATA, val);
    wr32(R_LTECOEX_CTRL, 0xC00F0000u | off);
    return 0;
}

static int lte_write_mask(u16 off, u32 mask, u32 val)
{
    u32 shift = 0, tmp = 0;
    while (!((mask >> shift) & 1))
        shift++;
    if (lte_read(off, &tmp))
        tmp = 0; /* Linux: "failed to read indirect register", carries on with 0 */
    tmp = (tmp & ~mask) | ((val << shift) & mask);
    return lte_write(off, tmp);
}

static void coex_gnt_bt(u32 state)
{
    lte_write_mask(LTE_COEX_CTRL, 0xC000, state);
    lte_write_mask(LTE_COEX_CTRL, 0x0C00, state);
}

static void coex_gnt_wl(u32 state)
{
    lte_write_mask(LTE_COEX_CTRL, 0x3000, state);
    lte_write_mask(LTE_COEX_CTRL, 0x0300, state);
}

/* rtw_coex_coex_ctrl_owner(true): the Wi-Fi side controls the shared path (this chip has no extra BTG register) */
static void coex_owner_wifi(void) { set8(R_SYS_SDIO_CTRL + 3, (u8)(BIT(26) >> 24)); }

enum { SW_TO_BT, SW_TO_WLG, SW_TO_WLA, SW_TO_NOCARE, SW_TO_WLG_BT };
enum { SWCTRL_BBSW, SWCTRL_PTA, SWCTRL_ANTDIV, SWCTRL_MAC, SWCTRL_BT, SWCTRL_FW };
static struct
{
    int ant_switch_exist, wlg_at_btg, polarity;
    unsigned module_type;
    unsigned cur_switch_status;
    int rfe_btg;
} CX;

static void mask8(u32 off, u32 mask, u8 value)
{
    unsigned shift = 0;
    mask &= 0xFF;
    while (!((mask >> shift) & 1))
        shift++;
    wr8(off, (u8)((rd8(off) & ~mask) | ((value << shift) & mask)));
}

/* rtw8821c_coex_cfg_rfe_type: what the board's RFE type says about its antenna wiring */
static void coex_rfe_type(unsigned rfe)
{
    CX.module_type = rfe;
    CX.polarity = 0;
    CX.ant_switch_exist = 1;
    CX.wlg_at_btg = 0;
    switch (rfe)
    {
    case 2:
    case 10:
    case 7:
    case 15: /* BTG: the 2.4 GHz Wi-Fi signal goes in at the Bluetooth port */
        CX.wlg_at_btg = 1;
        break;
    case 3:
    case 11:
        CX.polarity = 1;
        break;
    case 4:
    case 12:
        CX.wlg_at_btg = 1;
        CX.polarity = 1;
        break;
    case 5:
    case 13:
    case 6:
    case 14: /* no antenna switch on the board */
        CX.ant_switch_exist = 0;
        break;
    default:
        break;
    }
    CX.rfe_btg = rfe == 2 || rfe == 4 || rfe == 7 || rfe == 0xA || rfe == 0xC || rfe == 0xF; /* hal->rfe_btg */
}

/* rtw8821c_coex_cfg_ant_switch */
static void coex_ant_switch(unsigned ctrl_type, unsigned pos_type)
{
    unsigned switch_status = ((ctrl_type << 8) & 0xFF00) | pos_type;
    unsigned regval = 0;
    if (switch_status == CX.cur_switch_status)
        return;
    if (CX.wlg_at_btg)
    {
        ctrl_type = SWCTRL_BBSW;
        pos_type = CX.polarity ? SW_TO_WLA : SW_TO_WLG_BT;
    }
    CX.cur_switch_status = switch_status;
    int polarity_inverse = CX.polarity == 1;
    switch (ctrl_type)
    {
    default:
    case SWCTRL_BBSW:
        andn32(R_LED_CFG, BIT(23)); /* BIT_DPDT_SEL_EN */
        or32(R_LED_CFG, BIT(24));   /* BIT_DPDT_WL_SEL */
        mask8(R_RFE_CTRL8, 0xFF, DPDT_CTRL_PIN); /* the DPDT is driven by RFE pins 8 and 9 */
        if (pos_type == SW_TO_WLG_BT)
        {
            if (CX.module_type != 0x4 && CX.module_type != 0x2)
                regval = 0x3;
            else
                regval = !polarity_inverse ? 0x2 : 0x1;
        }
        else if (pos_type == SW_TO_WLG)
            regval = !polarity_inverse ? 0x2 : 0x1;
        else
            regval = !polarity_inverse ? 0x1 : 0x2;
        mask32(R_RFE_CTRL8, 0xF0000000u, regval);
        break;
    case SWCTRL_PTA:
        andn32(R_LED_CFG, BIT(23));
        or32(R_LED_CFG, BIT(24));
        mask8(R_RFE_CTRL8, 0xFF, PTA_CTRL_PIN);
        regval = !polarity_inverse ? 0x2 : 0x1;
        mask32(R_RFE_CTRL8, 0xF0000000u, regval);
        break;
    case SWCTRL_ANTDIV:
        andn32(R_LED_CFG, BIT(23));
        or32(R_LED_CFG, BIT(24));
        mask8(R_RFE_CTRL8, 0xFF, ANTDIC_CTRL_PIN);
        break;
    case SWCTRL_MAC:
        or32(R_LED_CFG, BIT(23));
        regval = !polarity_inverse ? 0x0 : 0x1;
        mask8(R_PAD_CTRL1, BIT(0), (u8)regval); /* BIT_SW_DPDT_SEL_DATA */
        break;
    case SWCTRL_FW:
        andn32(R_LED_CFG, BIT(23));
        or32(R_LED_CFG, BIT(24));
        break;
    case SWCTRL_BT:
        andn32(R_LED_CFG, BIT(23));
        andn32(R_LED_CFG, BIT(24));
        break;
    }
    if (ctrl_type == SWCTRL_BT)
    {
        clr8(R_CTRL_TYPE, BIT(5));
        clr8(R_CTRL_TYPE, BIT(4));
    }
    else
    {
        set8(R_CTRL_TYPE, BIT(5));
        set8(R_CTRL_TYPE, BIT(4));
    }
}

/* rtw_coex_set_ant_path(COEX_SET_ANT_WONLY) */
static void coex_wifi_only(unsigned rfe)
{
    CX.cur_switch_status = 0;
    coex_rfe_type(rfe);
    coex_gnt_bt(1); /* COEX_GNT_SET_SW_LOW: Bluetooth gets no grant */
    coex_gnt_wl(3); /* COEX_GNT_SET_SW_HIGH: Wi-Fi always has it */
    coex_owner_wifi();
    if (CX.ant_switch_exist)
        coex_ant_switch(SWCTRL_BBSW, SW_TO_WLG);
}

unsigned rf_read(unsigned addr) { return rd32(R_RF_BASE_A + ((addr & 0xFF) << 2)) & RFREG_MASK; }
static void rf_write_mask(unsigned addr, unsigned mask, unsigned data)
{
    mask &= RFREG_MASK;
    if (mask != RFREG_MASK)
    {
        unsigned shift = 0;
        while (!((mask >> shift) & 1))
            shift++;
        data = (rf_read(addr) & ~mask) | (data << shift);
    }
    phy_rf_write(addr, data); /* the write path of the tables: SIPI, then 13 us */
}

/* rtw8821c_switch_rf_set: which RF input the receiver listens to (0 BTG, 1 WLG, 2 WLA) */
static void switch_rf_set(int to)
{
    or32(R_DMEM_CTRL, BIT(16)); /* BIT_WL_RST */
    or32(R_SYS_CTRL, BIT(26));  /* BIT_FEN_EN */
    u32 reg = rd32(R_RFECTL);
    switch (to)
    {
    case 0:
        reg |= B_BTG_SWITCH;
        reg &= ~(B_CTRL_SWITCH | B_WL_SWITCH | B_WLG_SWITCH | B_WLA_SWITCH);
        mask32(R_ENRXCCA, 0xFF0000, BTG_CCA);
        mask32(R_ENTXCCK, 0xFFFF, BTG_LNA);
        break;
    case 1:
        reg |= B_WL_SWITCH | B_WLG_SWITCH;
        reg &= ~(B_BTG_SWITCH | B_CTRL_SWITCH | B_WLA_SWITCH);
        mask32(R_ENRXCCA, 0xFF0000, WLG_CCA);
        mask32(R_ENTXCCK, 0xFFFF, WLG_LNA);
        break;
    case 2:
        reg |= B_WL_SWITCH | B_WLA_SWITCH;
        reg &= ~(B_BTG_SWITCH | B_CTRL_SWITCH | B_WLG_SWITCH);
        break;
    default:
        break;
    }
    wr32(R_RFECTL, reg);
}

/* rtw8821c_set_channel: baseband, bb swing, MAC, RF, RX filter. ch_param are the three filter values that rtw8821c_phy_set_param() reads back
 * after the tables; bb_swing_* come from the efuse (0xC6 and 0xC7). `channel` is the centre channel of the (20 / 40 / 80 MHz) channel, `bw` 0 / 1 / 2,
 * `sc` the position of the primary 20 MHz channel in it (1 = the upper half of 40 MHz, 2 = the lower half, 3 / 4 = the topmost / lowest of 80 MHz; 0 at 20 MHz) */
void set_channel_bw(unsigned channel, unsigned bw, unsigned sc, const u32 ch_param[3], unsigned bb_swing_2g, unsigned bb_swing_5g)
{
    /* rtw8821c_set_channel_bb */
    if (channel <= 14)
    {
        mask32(R_RXPSEL, BIT(28), 1);
        mask32(R_CCK_CHECK, BIT(7), 0);
        mask32(R_ENTXCCK, BIT(18), 0);
        mask32(R_RXCCAMSK, 0x0000FC00, 15);
        mask32(R_TXSCALE_A, 0xF00, 0);
        mask32(R_CLKTRK, 0x1FFE0000, 0x96A);
        if (channel == 14) /* rtw_regd_srrc() is false here: the default CCK filter */
        {
            mask32(R_TXSF2, 0xFFFFFFFFu, 0x0000B81C);
            mask32(R_TXSF6, 0xFFFF, 0x0000);
            mask32(R_TXFILTER, 0xFFFFFFFFu, 0x00003667);
        }
        else
        {
            mask32(R_TXSF2, 0xFFFFFFFFu, ch_param[0]);
            mask32(R_TXSF6, 0xFFFF, ch_param[1] & 0xFFFF);
            mask32(R_TXFILTER, 0xFFFFFFFFu, ch_param[2]);
        }
    }
    else if (channel > 35)
    {
        mask32(R_ENTXCCK, BIT(18), 1);
        mask32(R_CCK_CHECK, BIT(7), 1);
        mask32(R_RXPSEL, BIT(28), 0);
        mask32(R_RXCCAMSK, 0x0000FC00, 15);
        if (channel >= 36 && channel <= 64)
            mask32(R_TXSCALE_A, 0xF00, 1);
        else if (channel >= 100 && channel <= 144)
            mask32(R_TXSCALE_A, 0xF00, 2);
        else if (channel >= 149)
            mask32(R_TXSCALE_A, 0xF00, 3);
        if (channel >= 36 && channel <= 48)
            mask32(R_CLKTRK, 0x1FFE0000, 0x494);
        else if (channel >= 52 && channel <= 64)
            mask32(R_CLKTRK, 0x1FFE0000, 0x453);
        else if (channel >= 100 && channel <= 116)
            mask32(R_CLKTRK, 0x1FFE0000, 0x452);
        else if (channel >= 118 && channel <= 177)
            mask32(R_CLKTRK, 0x1FFE0000, 0x412);
    }
    /* the bandwidth setting: the ADC clock / filter selection of the baseband (and for 40 MHz which side the primary channel is on) */
    u32 val32;
    if (bw == 1)
    {
        if (sc == 1)
            wr32(R_RXSB, rd32(R_RXSB) | BIT(4));
        else
            wr32(R_RXSB, rd32(R_RXSB) & ~BIT(4));
        val32 = rd32(R_ADCCLK);
        val32 &= 0xFF3FF300u;
        val32 |= 0x20020000u | ((sc & 0xF) << 2) | 1;
    }
    else if (bw == 2)
    {
        val32 = rd32(R_ADCCLK);
        val32 &= 0xFCFFCF00u;
        val32 |= 0x40040000u | ((sc & 0xF) << 2) | 2;
    }
    else
    {
        val32 = rd32(R_ADCCLK);
        val32 &= 0xFFCFFC00u;
        val32 |= 0x10010000u;
    }
    mask32(R_ADCCLK, 0xFFFFFFFFu, val32);
    mask32(R_ADC160, BIT(30), 1);

    /* rtw8821c_set_channel_bb_swing */
    static const u32 SWING2SETTING[4] = {0x200, 0x16A, 0x101, 0x0B6};
    unsigned swing = channel <= 14 ? bb_swing_2g : bb_swing_5g;
    if (swing > 9)
        swing = 0;
    mask32(R_TXSCALE_A, 0xFFE00000u, SWING2SETTING[swing / 3]);

    /* rtw_set_channel_mac */
    unsigned txsc40 = 0;
    if (bw == 2)
        txsc40 = (sc == 1 || sc == 3) ? 9 : 10; /* RTW_SC_40_UPPER / RTW_SC_40_LOWER: which 40 MHz half of the 80 MHz the primary channel is in */
    wr8(R_DATA_SC, (u8)((sc & 0xF) | txsc40 << 4)); /* BIT_TXSC_20M | BIT_TXSC_40M: where the primary channel sits */
    u32 v = rd32(R_WMAC_TRXPTCL_CTL);
    v &= ~0x180u; /* BIT_RFMOD */
    if (bw == 2)
        v |= BIT(8); /* BIT_RFMOD_80M */
    else if (bw == 1)
        v |= BIT(7); /* BIT_RFMOD_40M */
    wr32(R_WMAC_TRXPTCL_CTL, v);
    v = rd32(R_AFE_CTRL1) & ~0x300000u; /* BIT_MAC_CLK_SEL, set to MAC_CLK_HW_DEF_80M = 0 */
    wr32(R_AFE_CTRL1, v);
    wr8(R_USTIME_TSF, MAC_CLK_SPEED);
    wr8(R_USTIME_EDCA, MAC_CLK_SPEED);
    u8 value8 = (u8)(rd8(R_CCK_CHECK) & ~0x80);
    if (channel >= 36 && channel <= 177) /* IS_CH_5G_BAND */
        value8 |= 0x80;
    wr8(R_CCK_CHECK, value8);

    /* rtw8821c_set_channel_rf */
    u32 rf18 = rf_read(0x18);
    rf18 &= ~(RF18_BAND_MASK | RF18_CHANNEL_MASK | RF18_RFSI_MASK | RF18_BW_MASK);
    rf18 |= channel <= 14 ? RF18_BAND_2G : RF18_BAND_5G;
    rf18 |= channel & RF18_CHANNEL_MASK;
    if (channel >= 100 && channel <= 140)
        rf18 |= RF18_RFSI_GE;
    else if (channel > 140)
        rf18 |= RF18_RFSI_GT;
    rf18 |= bw == 2 ? 0x400u : bw == 1 ? 0x800u : RF18_BW_20M; /* RF18_BW_80M / RF18_BW_40M / RF18_BW_20M */
    if (channel <= 14)
    {
        switch_rf_set(CX.rfe_btg ? 0 : 1);
        rf_write_mask(RF_LUTDBG, BIT(6), 1);
        rf_write_mask(0x64, 0xF, 0xF);
    }
    else
    {
        switch_rf_set(2);
        rf_write_mask(RF_LUTDBG, BIT(6), 0);
    }
    rf_write_mask(0x18, RFREG_MASK, rf18);
    rf_write_mask(RF_XTALX2, BIT(19), 0);
    rf_write_mask(RF_XTALX2, BIT(19), 1);

    /* rtw8821c_set_channel_rxdfir: the receive / transmit filters for the width */
    mask32(R_ACBB0, BIT(29) | BIT(28), 0x2);
    mask32(R_ACBBRXFIR, BIT(29) | BIT(28), bw == 2 ? 0x1 : 0x2);
    mask32(R_TXDFIR, BIT(31), bw == 0 ? 0x1 : 0x0);
    mask32(R_CHFIR, BIT(31), bw == 2 ? 0x1 : 0x0);
}

/* rtw8821c_set_channel() for a 20 MHz channel: baseband, bb swing, MAC, RF, RX filter. ch_param are the three filter values that
 * rtw8821c_phy_set_param() reads back after the tables; bb_swing_* come from the efuse (0xC6 and 0xC7). */
void set_channel_20(unsigned channel, const u32 ch_param[3], unsigned bb_swing_2g, unsigned bb_swing_5g)
{
    set_channel_bw(channel, 0, 0, ch_param, bb_swing_2g, bb_swing_5g);
}

Anon_CHN CHN;

/* tune to a channel (20 MHz) with the antenna given to Wi-Fi, and check by reading RF register 0x18 back */
void channel_probe(unsigned channel)
{
    coex_wifi_only(EF.rfe);
    set_channel_20(channel, PH.ch_param, EF.swing2g, EF.swing5g);
    CHN.channel = channel;
    CHN.rf18 = rf_read(0x18);
    CHN.state = (CHN.rf18 & RF18_CHANNEL_MASK) == channel && (CHN.rf18 & RF18_BW_MASK) == RF18_BW_20M ? CH_OK : CH_FAILED;
}
