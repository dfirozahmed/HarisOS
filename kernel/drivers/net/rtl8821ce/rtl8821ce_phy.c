/* RTL8821CE: the baseband, AGC and RF tables (stage 4b-2). */
#include "rtl8821ce_internal.h"

/* ---- stage 4b-2: the baseband, AGC and RF tables (the table part of rtw8821c_phy_set_param) ----
 * The radio is configured by long lists of register writes that Realtek ships as tables (include/rtl8821ce_phytab.h, copied from
 * Linux). A list holds several variants of the same setting for different chip cuts / board types, written as "if (conditions) ...
 * else ... endif" blocks inside the list; only the variant of THIS chip (cut, package, interface, RFE type from the efuse) is written.
 * The RF registers are not memory mapped: they are written through the "SIPI" serial interface (one 32-bit word: register number in
 * bits 27..20, data in bits 19..0 at 0xC90 for path A) and can be read back directly from a window at 0x2800 + 4 * number.
 * Not done here (software state or later stages): rtw_phy_init, the power-tracking and beamforming set-up, calibration (IQK / LCK). */

#define R_AFE_XTAL_CTRL 0x0024
#define R_AFE_PLL_CTRL 0x0028
#define R_CCK0_FAREPORT 0x0A2C
#define R_RF_SIPI_A 0x0C90

enum { TAB_MAC, TAB_AGC, TAB_BB, TAB_RF };
Anon_PH PH;

static void delay_us(unsigned us) { udelay(us); }

/* the cut, package, interface and RFE type that the "if" blocks of the tables ask about (rtw_phy_setup_phy_cond) */
static unsigned phy_cond_word(unsigned cut, unsigned pkg, unsigned rfe)
{
    return (rfe & 0xFF) | (1u << 8) /* INTF_PCIE */ | ((pkg ? pkg : 15) << 12) | (4u << 16) /* plat */ | ((cut ? cut : 15) << 24);
}

/* check_positive: a zero field in the table's condition means "any" */
static int phy_cond_matches(unsigned c)
{
    unsigned d = PH.cond;
    if (((c >> 24) & 0xF) && ((c >> 24) & 0xF) != ((d >> 24) & 0xF))
        return 0;
    if (((c >> 12) & 0xF) && ((c >> 12) & 0xF) != ((d >> 12) & 0xF))
        return 0;
    if (((c >> 8) & 0xF) && ((c >> 8) & 0xF) != ((d >> 8) & 0xF))
        return 0;
    return (c & 0xFF) == (d & 0xFF);
}

void phy_rf_write(unsigned addr, unsigned data)
{
    addr &= 0xFF;
    wr32(R_RF_SIPI_A, ((addr << 20) | (data & RFREG_MASK)) & 0x0FFFFFFF);
    delay_us(13);
    PH.rf_seen[addr] = 1;
    PH.rf_last[addr] = data & RFREG_MASK;
    PH.rf_written++;
}

static void phy_cfg(int kind, unsigned addr, unsigned data)
{
    switch (kind)
    {
    case TAB_MAC:
        wr8(addr, (u8)data);
        break;
    case TAB_AGC:
        wr32(addr, data);
        break;
    case TAB_BB:
        if (addr == 0xFE)
            delay_us(50000);
        else if (addr == 0xFD)
            delay_us(5000);
        else if (addr == 0xFC)
            delay_us(1000);
        else if (addr == 0xFB)
            delay_us(50);
        else if (addr == 0xFA)
            delay_us(5);
        else if (addr == 0xF9)
            delay_us(1);
        else
            wr32(addr, data);
        break;
    default: /* TAB_RF, path A */
        if (addr == 0xFFE)
            delay_us(50000);
        else if (addr == 0xFE)
            delay_us(100);
        else
        {
            phy_rf_write(addr, data);
            delay_us(1);
        }
        break;
    }
}

/* rtw_parse_tbl_phy_cond: bit 31 of the first word = "this is a condition" (branch in bits 29..28: 0 if, 1 elif, 2 else, 3 endif);
 * bit 30 = "the conditions are complete, decide now"; anything else is a register write while the current block is selected. */
static void phy_table_run(int kind, const unsigned *t, unsigned words)
{
    unsigned pos_cond = 0;
    int matched = 1, skipped = 0;
    for (unsigned i = 0; i + 1 < words; i += 2)
    {
        unsigned w = t[i];
        if (w & BIT(31))
        {
            unsigned branch = (w >> 28) & 3;
            if (branch == 3)
            {
                matched = 1;
                skipped = 0;
            }
            else if (branch == 2)
                matched = skipped ? 0 : 1;
            else
                pos_cond = w;
        }
        else if (w & BIT(30))
        {
            if (!skipped)
            {
                if (phy_cond_matches(pos_cond))
                {
                    matched = 1;
                    skipped = 1;
                }
                else
                {
                    matched = 0;
                    skipped = 0;
                }
            }
            else
                matched = 0;
        }
        else if (matched)
            phy_cfg(kind, w, t[i + 1]);
    }
}

#define TAB_WORDS(t) ((unsigned)(sizeof(t) / sizeof((t)[0])))

void mask32(u32 off, u32 mask, u32 value)
{
    unsigned shift = 0;
    while (!((mask >> shift) & 1))
        shift++;
    wr32(off, (rd32(off) & ~mask) | ((value << shift) & mask));
}

void phy_init(unsigned cut)
{
    PH.state = PH_SKIPPED;
    PH.why = 0;
    PH.rf_diff = PH.rf_written = 0;
    PH.rf_regs = 0;
    for (int i = 0; i < 256; i++)
        PH.rf_seen[i] = 0;
    if (!EF.map_ok)
    {
        PH.why = "efuse";
        return;
    }
    if (EF.rfe != 0 && EF.rfe != 2 && EF.rfe != 4 && EF.rfe != 6) /* the only board types rtw8821c_rfe_defs has: Linux refuses the others */
    {
        PH.why = "rfe";
        return;
    }
    PH.cond = phy_cond_word(cut, EF.pkg, EF.rfe);

    /* power on the baseband and RF domains, then pulse the baseband reset */
    u8 val = rd8(R_SYS_FUNC_EN);
    val |= BIT(6); /* BIT_FEN_PCIEA */
    wr8(R_SYS_FUNC_EN, val);
    val |= BIT(0) | BIT(1); /* BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST */
    wr8(R_SYS_FUNC_EN, val);
    val &= (u8) ~(BIT(0) | BIT(1));
    wr8(R_SYS_FUNC_EN, val);
    val |= BIT(0) | BIT(1);
    wr8(R_SYS_FUNC_EN, val);
    wr8(R_RF_CTRL, BIT(0) | BIT(1) | BIT(2)); /* BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB */
    delay_us(10);
    wr8(R_WLRF1 + 3, BIT(0) | BIT(1) | BIT(2));
    delay_us(10);

    andn32(R_RXPSEL, BIT(28) | BIT(29)); /* BIT_RX_PSEL_RST: held while the tables are written */

    phy_table_run(TAB_MAC, rtl8821ce_tab_mac, TAB_WORDS(rtl8821ce_tab_mac));
    phy_table_run(TAB_BB, rtl8821ce_tab_bb, TAB_WORDS(rtl8821ce_tab_bb));
    phy_table_run(TAB_AGC, rtl8821ce_tab_agc, TAB_WORDS(rtl8821ce_tab_agc));
    if (EF.rfe == 2 || EF.rfe == 4) /* rtw8821c_rfe_defs: these board types carry an extra AGC table (the BTG antenna switch) */
        phy_table_run(TAB_AGC, rtl8821ce_tab_agc_btg2, TAB_WORDS(rtl8821ce_tab_agc_btg2));
    phy_table_run(TAB_RF, rtl8821ce_tab_rf_a, TAB_WORDS(rtl8821ce_tab_rf_a));

    unsigned xtal = EF.xtal & 0x3F;
    mask32(R_AFE_XTAL_CTRL, 0x7E000000, xtal);
    mask32(R_AFE_PLL_CTRL, 0x7E, xtal);
    mask32(R_CCK0_FAREPORT, BIT(18) | BIT(22), 0);
    or32(R_RXPSEL, BIT(28) | BIT(29));
    PH.ch_param[0] = rd32(0x0A24); /* REG_TXSF2 */
    PH.ch_param[1] = rd32(0x0A28); /* REG_TXSF6 */
    PH.ch_param[2] = rd32(0x0AAC); /* REG_TXFILTER */

    /* the RF registers answer at 0x2800 + 4 * number: do they hold what was last written? */
    for (int a = 0; a < 256; a++)
        if (PH.rf_seen[a] && (rd32(R_RF_BASE_A + ((unsigned)a << 2)) & RFREG_MASK) != PH.rf_last[a])
        {
            if (PH.rf_diff < sizeof PH.diff_addr)
            {
                PH.diff_addr[PH.rf_diff] = (unsigned char)a;
                PH.diff_xor[PH.rf_diff] = (rd32(R_RF_BASE_A + ((unsigned)a << 2)) & RFREG_MASK) ^ PH.rf_last[a];
            }
            PH.rf_diff++;
        }
    for (int a = 0; a < 256; a++)
        PH.rf_regs += PH.rf_seen[a];
    PH.state = PH_OK;
}

/* Some RF bits are driven by the chip itself (mode, channel and bandwidth, bias settings) and read back differently from what was
 * written, even on a healthy card: 26 of about 90 registers on the real one. Only if more than half differ is the write path suspect. */
int phy_rf_suspect(void) { return PH.rf_diff * 2 > PH.rf_regs; }
