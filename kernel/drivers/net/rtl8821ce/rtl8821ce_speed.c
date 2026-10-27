/* RTL8821CE: speed (stage 8). */
#include "rtl8821ce_internal.h"

/* ---- stage 8: speed ----
 * Until now every data frame went out at one fixed rate, 24 Mb/s (OFDM). The chip's firmware can choose the rate itself, for each frame, from what
 * the access point can receive and how the last frames went (the "rate adaptation"): the driver tells it once, after the association,
 *   - H2C "media status report" (0x01): station number 0 is now connected (Linux: rtw_fw_media_status_report), and
 *   - H2C "RA info" (0x40): which rate table to use (rate id), whether the access point is VHT, short guard interval, the bandwidth, and the
 *     mask of rates the access point accepts: bit 0..3 CCK, 4..11 OFDM, 12..19 HT MCS 0..7, 12..21 VHT MCS 0..9 (one spatial stream; the
 *     second stream would be bits 20.. for HT and 22.. for VHT) (Linux: rtw_update_sta_info + rtw_fw_send_ra_info).
 * After that a data frame says "use rate table N" (descriptor: rate id) and no fixed rate: the firmware picks from the mask, up and down.
 * What limits the speed now: one antenna (one spatial stream) and 20 MHz, the width the radio is tuned for (set_channel_20). That is
 * 72 Mb/s (HT, MCS 7, short guard interval) or 86 Mb/s (VHT, MCS 8) at best, from the fixed 24 before. 40 / 80 MHz need the radio and the
 * baseband set up for the wider channel; not done. Frames are sent as QoS data frames then (an HT station is a QoS station): two more header bytes. */
#define H2C_CMD_MEDIA_STATUS_RPT 0x01
#define H2C_CMD_RA_INFO 0x40
#define RATEID_BGN_40M_1SS 1
#define RATEID_BGN_20M_1SS 3
#define RATEID_GN_N1SS 5
#define RATEID_ARFR1_AC_1SS 10
#define DESC_RATE_MCS7 0x13
#define DESC_RATE_VHT1SS_MCS0 0x2C
#define RA_MASK_OFDM 0x0FF0u
#define RA_MASK_HT_1SS (0xFFu << 12)
#define RA_MASK_VHT_1SS (0x3FFu << 12)


/* one command of 8 bytes into the next mailbox, once the firmware has taken the last one from it: 0, or -1 */
static int h2c_send(u32 w0, u32 w1)
{
    unsigned box = H2C_BOX;
    int ok = 0;
    for (int i = 0; i < 30 && !ok; i++) /* 3 ms in steps of 100 us, like Linux */
    {
        if (!((rd8(R_HMETFR) >> box) & 1))
            ok = 1;
        else
            udelay(100);
    }
    if (!ok)
        return -1;
    wr32(R_HMEBOX0_EX + box * 4, w1);
    wr32(R_HMEBOX0 + box * 4, w0);
    H2C_BOX = (box + 1) & 3;
    return 0;
}

Anon_RA RA;

static void media_status(int connected) { h2c_send(H2C_CMD_MEDIA_STATUS_RPT | (connected ? 1u << 8 : 0), 0); }

/* after the association: tell the firmware about the access point. rtw_update_sta_info() for a station with one spatial stream at 20 MHz. */
static void ra_start(void)
{
    RA.on = 0;
    RA.ht = use_ht();
    RA.vht = use_vht();
    RA.bw = CONN.cc.bw;
    if (!RA.ht) /* an access point without HT, or "rate=legacy": the fixed rate stays */
        return;
    u32 mask = 0, bak;
    unsigned rate_id, vht_mcs = 0;
    if (RA.vht)
    {
        switch (CONN.vht_map & 3) /* the access point's receive MCS map, the first stream */
        {
        case 2:
            mask = RA_MASK_VHT_1SS;
            vht_mcs = 9;
            break;
        case 1:
            mask = 0x1FFu << 12;
            vht_mcs = 8;
            break;
        case 0:
            mask = 0x0FFu << 12;
            vht_mcs = 7;
            break;
        default:
            RA.vht = 0; /* no VHT rate: HT it is */
            break;
        }
    }
    if (!RA.vht)
        mask = (u32)CONN.ht_mcs0 << 12;
    if (CONN.channel > 14)
    {
        mask |= CONN.rates & RA_MASK_OFDM;
        bak = mask;
        if (RA.vht)
        {
            mask &= RA_MASK_VHT_1SS | 0x10; /* with VHT only the 6 Mb/s OFDM rate stays beside it */
            rate_id = RATEID_ARFR1_AC_1SS;
        }
        else
        {
            mask &= RA_MASK_HT_1SS | 0x30;
            rate_id = RATEID_GN_N1SS;
        }
    }
    else
    {
        mask |= CONN.rates & 0x0FFF;
        bak = mask;
        mask &= RA_MASK_HT_1SS | 0x05 | 0x10; /* HT at 2.4 GHz keeps CCK 1 and 5.5 and OFDM 6 beside it */
        rate_id = RA.bw == 1 ? RATEID_BGN_40M_1SS : RATEID_BGN_20M_1SS;
    }
    if ((mask & ~0x0FFFu) == 0) /* rtw_rate_mask_recover: never leave only legacy rates when the access point has more, and never nothing */
        mask |= bak & ~0x0FFFu;
    if (mask == 0)
        mask |= bak & 0x0FFFu;
    if (!RA.vht && RA.bw == 2)
        RA.bw = 1; /* (no VHT rate left: HT rates are 20 / 40 MHz rates) */
    RA.rate_id = rate_id;
    RA.mask = mask;
    RA.sgi = RA.bw == 2 && RA.vht ? (CONN.vht_cap >> 5) & 1 : RA.bw == 1 ? (CONN.ht_cap >> 6) & 1 : (CONN.ht_cap >> 5) & 1; /* its short guard interval at this width */
    RA.tx_rate = RA.vht ? DESC_RATE_VHT1SS_MCS0 + vht_mcs : DESC_RATE_MCS7;
    /* station 0, the rate table, initial level 1, short GI, 20 MHz (0), no LDPC, update the mask, VHT, no power tracking */
    u32 w0 = H2C_CMD_RA_INFO | rate_id << 16 | 1u << 21 | RA.sgi << 23 | RA.bw << 24 | (RA.vht ? 1u << 28 : 0) | 1u << 30;
    RA.on = h2c_send(w0, mask) == 0;
    if (RA.on)
        media_status(1);
}

void ra_stop(void)
{
    if (RA.on)
        media_status(0); /* (Linux: when the station is removed) */
    RA.on = 0;
}

/* authenticate, associate, 4-way handshake, keys in the chip; the chip is tuned to CONN.channel and CONN.bssid is the access point. The first
 * connection (connect_try) and every later reconnection (link_service) go through here. A link that did not come up is torn down again. */
void connect_exchange(void)
{
    CONN.state = 1;
    CONN.auth_seen = CONN.assoc_seen = 0;
    CONN.auth_status = CONN.assoc_status = CONN.aid = CONN.dropped = 0;
    CONN.auth_tries = CONN.assoc_tries = 0;
    u8 frame[256];
    unsigned len = build_auth(frame, CONN.bssid);
    CONN.auth_tries = conn_exchange(frame, len, CONN.channel, &CONN.auth_seen);
    if (CONN.auth_seen && CONN.auth_status == 0)
    {
        len = build_assoc(frame, CONN.bssid, CFG.ssid, CFG.ssid_len, CONN.channel);
        CONN.assoc_tries = conn_exchange(frame, len, CONN.channel, &CONN.assoc_seen);
        if (CONN.assoc_seen && CONN.assoc_status == 0)
        {
            ra_start(); /* (the firmware is told about the access point before the first data frame, as Linux does it when it adds the station) */
            handshake_run();
        }
        link_try(); /* a handshake that worked and put the keys in the chip: the card stays connected */
        if (!link_is_up()) /* nothing worked out: tidy up */
        {
            ra_stop();
            len = build_deauth(frame, CONN.bssid);
            if (tx_mgmt(frame, len, CONN.channel) == 0)
                listen_ms(20);
        }
    }
}
