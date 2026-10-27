/* RTL8821CE: transmit power (stage 5a-1). */
#include "rtl8821ce_internal.h"

/* ---- stage 5a-1: transmit power ----
 * Before the first frame goes out, the chip must be told how strongly to send each rate. The number is an index (about half a decibel
 * per step; a rule of Realtek's chips, not something this driver checks) and it is made of three parts, as in Linux' phy.c:
 *   - the base from the efuse: the factory calibrated value for the channel group (and the OFDM or CCK offset of that group);
 *   - the "power by rate" table: how much weaker the higher rates are sent, relative to the last rate of the section (54M, 11M);
 *   - the regulatory limit (include/rtl8821ce_txpwr.h): per country group and channel. The country of this PC is not known to the
 *     driver, so it uses the world-wide row: the lowest limit of every country group that has one for the channel.
 * The index of a rate is  base + min(by-rate offset, limit, no-limit) , at most 0x3F; the chip takes four rates per 32-bit register
 * (0x1D00 + rate / 4 * 4), one byte per rate. Only the rates of the CCK (2.4 GHz) and OFDM sections at 20 MHz are computed: they are
 * the ones used for management frames. */
#include "rtl8821ce_txpwr.h"

#define TXPWR_MAX_INDEX 0x3F
#define R_TXAGC_A 0x1D00
#define RATE_CCK_FIRST 0
#define RATE_OFDM_FIRST 4
#define RATE_OFDM_LAST 11
#define RS_CCK 0
#define RS_OFDM 1

static const u8 CH5G[49] = {36,  38,  40,  42,  44,  46,  48,  52,  54,  56,  58,  60,  62,  64,  100, 102, 104,
                            106, 108, 110, 112, 116, 118, 120, 122, 124, 126, 128, 132, 134, 136, 138, 140, 142,
                            144, 149, 151, 153, 155, 157, 159, 161, 165, 167, 169, 171, 173, 175, 177};

static int sext4(unsigned v) { return (v & 8) ? (int)(v & 0xF) - 16 : (int)(v & 0xF); }

/* rtw_get_channel_group: which of the efuse's base values a channel uses */
static int txpwr_group(unsigned channel, int cck)
{
    switch (channel)
    {
    case 1:
    case 2:
    case 36:
    case 38:
    case 40:
    case 42:
        return 0;
    case 3:
    case 4:
    case 5:
    case 44:
    case 46:
    case 48:
    case 50:
        return 1;
    case 6:
    case 7:
    case 8:
    case 52:
    case 54:
    case 56:
    case 58:
        return 2;
    case 9:
    case 10:
    case 11:
    case 60:
    case 62:
    case 64:
        return 3;
    case 12:
    case 13:
    case 100:
    case 102:
    case 104:
    case 106:
        return 4;
    case 14:
        return cck ? 5 : 4;
    case 108:
    case 110:
    case 112:
    case 114:
        return 5;
    case 116:
    case 118:
    case 120:
    case 122:
        return 6;
    case 124:
    case 126:
    case 128:
    case 130:
        return 7;
    case 132:
    case 134:
    case 136:
    case 138:
        return 8;
    case 140:
    case 142:
    case 144:
        return 9;
    case 149:
    case 151:
    case 153:
    case 155:
        return 10;
    case 157:
    case 159:
    case 161:
        return 11;
    case 165:
    case 167:
    case 169:
    case 171:
        return 12;
    case 173:
    case 175:
    case 177:
        return 13;
    default:
        return 0; /* Linux warns and uses group 0 */
    }
}


/* the centre channel of the 20 / 40 / 80 MHz channel of the width `bw` that holds the primary channel (hal->cch_by_bw) */
static unsigned chan_cch(const Chan *c, unsigned bw)
{
    if (bw == 0)
        return c->prim;
    if (bw == c->bw)
        return c->center;
    if (bw == 1 && c->bw == 2) /* the 40 MHz half of an 80 MHz channel that holds the primary channel */
        return (c->sc == 1 || c->sc == 3) ? c->center + 4 : c->center - 4;
    return c->center;
}

/* the base index of the efuse, path A (rtw_phy_get_2g_tx_power_index / rtw_phy_get_5g_tx_power_index): CCK, OFDM, HT (MCS 0-7) or VHT (one stream)
 * rate, for the width of the channel. The efuse block: 2.4 GHz cck_base[6], bw40_base[5], ht_1s_diff (low nibble OFDM, high nibble 20 MHz) ...
 * 18 bytes in all; then 5 GHz bw40_base[14] at 18, ht_1s_diff at 32 (the same nibbles), ... vht_1s_diff at 38 (low nibble 160 MHz, high nibble 80 MHz). */
static u8 txpwr_base(const u8 *txpwr, const Chan *c, unsigned rate)
{
    int cck = rate <= 3;
    int mcs = rate >= 12; /* HT or VHT */
    unsigned ch = c->center;
    unsigned group = (unsigned)txpwr_group(ch, cck);
    u8 p;
    if (ch <= 14)
    {
        if (cck)
            return txpwr[group];                          /* cck_base[group] */
        p = txpwr[6 + group];                             /* bw40_base[group]: the 40 MHz power is the base of everything else */
        if (!mcs)
            return (u8)(p + sext4(txpwr[11]));            /* the OFDM difference */
        if (c->bw == 0)
            p = (u8)(p + sext4(txpwr[11] >> 4));          /* the 20 MHz difference */
        return p;
    }
    p = txpwr[18 + group];                                /* the 5 GHz block starts at 18: bw40_base[14], then ht_1s_diff */
    if (!mcs)
        return (u8)(p + sext4(txpwr[18 + 14]));
    if (c->bw == 0)
        return (u8)(p + sext4(txpwr[18 + 14] >> 4));
    if (c->bw == 2) /* the 80 MHz base is the mean of the two neighbouring 40 MHz bases, plus the 80 MHz difference */
        return (u8)((txpwr[18 + group] + txpwr[18 + group + 1]) / 2 + sext4(txpwr[18 + 20] >> 4));
    return p;
}

/* the register of the "power by rate" table that holds a rate, and the byte in it (the rows of rtl8821ce_tab_pg): CCK, OFDM and HT in the order
 * of the rate numbers from 0xC20, VHT (one stream) from 0xC3C */
static unsigned txpwr_pg_reg(unsigned rate) { return rate < 44 ? 0xC20 + (rate / 4) * 4 : 0xC3C + ((rate - 44) / 4) * 4; }

/* one value of the "power by rate" table: four rates per register in the BCD-like form 0x32343638 = 32, 34, 36, 38; 0 if the table has none */
static int txpwr_pg(unsigned band, unsigned rate)
{
    unsigned reg = txpwr_pg_reg(rate);
    unsigned byte = rate < 44 ? rate % 4 : (rate - 44) % 4;
    for (unsigned i = 0; i < sizeof rtl8821ce_tab_pg / sizeof rtl8821ce_tab_pg[0]; i++)
        if (rtl8821ce_tab_pg[i][0] == band && rtl8821ce_tab_pg[i][1] == 0 && rtl8821ce_tab_pg[i][3] == reg)
        {
            unsigned b = (rtl8821ce_tab_pg[i][5] >> (byte * 8)) & 0xFF; /* bcd2bin of one byte */
            return (int)((b >> 4) * 10 + (b & 0xF));
        }
    return 0;
}

/* the rate sections of the limit table, and the rate all offsets of a section are relative to (the last rate; of the 10 VHT rates the eighth) */
#define RS_HT1 2
#define RS_VHT1 4
static unsigned txpwr_rs(unsigned rate) { return rate <= 3 ? RS_CCK : rate <= 11 ? RS_OFDM : rate <= 19 ? RS_HT1 : RS_VHT1; }
static unsigned txpwr_base_rate(unsigned rs) { return rs == RS_CCK ? 3 : rs == RS_OFDM ? 11 : rs == RS_HT1 ? 19 : 51; }

/* the world-wide limit: the lowest of the entries of all country groups for this band, bandwidth, rate section and channel (63 when there is none) */
static int txpwr_limit_raw(unsigned band, unsigned bw, unsigned channel, unsigned rs)
{
    int lim = TXPWR_MAX_INDEX;
    for (unsigned i = 0; i < sizeof rtl8821ce_tab_txpwr_lmt / sizeof rtl8821ce_tab_txpwr_lmt[0]; i++)
    {
        const short *e = rtl8821ce_tab_txpwr_lmt[i];
        if ((unsigned)e[1] == band && (unsigned)e[2] == bw && (unsigned)e[3] == rs && (unsigned)e[4] == channel)
        {
            int v = e[5] > TXPWR_MAX_INDEX ? TXPWR_MAX_INDEX : e[5] < -TXPWR_MAX_INDEX ? -TXPWR_MAX_INDEX : e[5];
            if (v < lim)
                lim = v;
        }
    }
    return lim;
}

/* ... and rtw_xref_txpwr_lmt: at 5 GHz, 20 and 40 MHz, where the table has a limit for the HT rates but none for the VHT ones (or the other way
 * round), the one that exists is taken for both */
static int txpwr_limit_ww(unsigned band, unsigned bw, unsigned channel, unsigned rs)
{
    int lim = txpwr_limit_raw(band, bw, channel, rs);
    if (band == 1 && bw <= 1 && (rs == RS_HT1 || rs == RS_VHT1))
    {
        int ht = txpwr_limit_raw(band, bw, channel, RS_HT1), vht = txpwr_limit_raw(band, bw, channel, RS_VHT1);
        if (ht != vht)
        {
            if (ht == TXPWR_MAX_INDEX)
                ht = vht;
            else if (vht == TXPWR_MAX_INDEX)
                vht = ht;
        }
        lim = rs == RS_HT1 ? ht : vht;
    }
    return lim;
}

static int txpwr_channel_known(unsigned channel)
{
    if (channel >= 1 && channel <= 14)
        return 1;
    for (unsigned i = 0; i < sizeof CH5G; i++)
        if (CH5G[i] == channel)
            return 1;
    return 0;
}

/* rtw_phy_get_tx_power_limit: the lowest of the limits of the 20 MHz channel up to the current width (CCK and OFDM only at 20 MHz, HT at most 40 MHz),
 * each for its own centre channel, relative to the base of the rate section; an unknown channel: Linux gives up and takes no limit */
static int txpwr_limit(const Chan *c, unsigned rate, unsigned rs, int base_pg)
{
    unsigned band = c->center <= 14 ? 0 : 1;
    unsigned bw = c->bw;
    if (rs == RS_CCK || rs == RS_OFDM)
        bw = 0;
    else if (rs == RS_HT1 && bw > 1)
        bw = 1;
    (void)rate;
    int lim = TXPWR_MAX_INDEX;
    for (unsigned cur = 0; cur <= bw; cur++)
    {
        unsigned ch = chan_cch(c, cur);
        if (!txpwr_channel_known(ch))
            return TXPWR_MAX_INDEX;
        int l = txpwr_limit_ww(band, cur, ch, rs) - base_pg;
        if (l < lim)
            lim = l;
    }
    return lim;
}

/* the power index of one rate (CCK 0-3, OFDM 4-11, HT MCS 0-7 = 12-19, VHT one stream MCS 0-9 = 44-53) for path A: rtw_phy_get_tx_power_index
 * with the world-wide limit and no SAR limit */
static u8 tx_power_index_c(const u8 *txpwr, const Chan *c, unsigned rate)
{
    unsigned band = c->center <= 14 ? 0 : 1;
    unsigned rs = txpwr_rs(rate);
    int base_pg = txpwr_pg(band, txpwr_base_rate(rs));
    int offset = txpwr_pg(band, rate) - base_pg;
    int limit = txpwr_limit(c, rate, rs, base_pg);
    int m = offset < limit ? offset : limit;
    if (m > TXPWR_MAX_INDEX)
        m = TXPWR_MAX_INDEX;
    u8 p = (u8)(txpwr_base(txpwr, c, rate) + m); /* an unsigned byte, as in Linux: a negative sum becomes a large number and is capped */
    return p > TXPWR_MAX_INDEX ? TXPWR_MAX_INDEX : p;
}

/* write the indices of all rates the card sends with: CCK on 2.4 GHz, OFDM, HT MCS 0-7 and VHT MCS 0-9 of the first stream (4 rates per register,
 * byte = rate % 4; the last register of the VHT rates holds MCS 8 and 9) in the order Linux writes them (rtw8821c_set_tx_power_index) */
void tx_power_apply_c(const u8 *txpwr, const Chan *c)
{
    static const u8 FIRST[4] = {0, 4, 12, 44}, LAST[4] = {3, 11, 19, 53};
    u32 word = 0;
    for (unsigned s = c->center <= 14 ? 0 : 1; s < 4; s++)
        for (unsigned rate = FIRST[s]; rate <= LAST[s]; rate++)
        {
            word |= (u32)tx_power_index_c(txpwr, c, rate) << ((rate & 3) * 8);
            if ((rate & 3) == 3 || rate == 53)
            {
                wr32(R_TXAGC_A + (rate & 0xFC), word);
                word = 0;
            }
        }
}

/* the 20 MHz channel `channel` */
#ifdef HARIS_HOST /* (the host test checks the 20 MHz values; the driver itself works with a Chan) */
static u8 tx_power_index(const u8 *txpwr, unsigned channel, unsigned rate)
{
    Chan c = {channel, channel, 0, 0};
    return tx_power_index_c(txpwr, &c, rate);
}

static void tx_power_apply(const u8 *txpwr, unsigned channel)
{
    Chan c = {channel, channel, 0, 0};
    tx_power_apply_c(txpwr, &c);
}
#endif
