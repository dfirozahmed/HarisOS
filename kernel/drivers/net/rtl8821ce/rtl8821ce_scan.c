/* RTL8821CE: scan (stage 4c-3). */
#include "rtl8821ce_internal.h"

/* ---- stage 4c-3: scan ----
 * Passive scan: tune to each channel in turn, listen a little longer than a beacon interval (access points send a beacon about every
 * 102 ms) and note every network heard. A beacon (or a probe response) is a management frame: 24 byte header (the sender's BSSID is the
 * third address), 12 bytes of fixed fields (timestamp, beacon interval, capability bits), then a list of information elements,
 * each tag (1 byte), length (1 byte), data. Element 0 is the network name (SSID), 3 the channel the access point is on (2.4 GHz),
 * 61 (HT operation) holds it for 5 GHz, 48 is the WPA2 (RSN) security description, a vendor element with 00:50:F2 type 1 is WPA. */

Anon_SC SC;


/* one management frame (without its FCS): remember the network it belongs to */
void scan_note(const volatile u8 *f, unsigned len)
{
    if (len < 36 + 2)
        return;
    unsigned fc0 = f[0];
    if ((fc0 & 0xFC) != 0x80 && (fc0 & 0xFC) != 0x50) /* beacon or probe response */
        return;
    u8 bssid[6];
    for (int i = 0; i < 6; i++)
        bssid[i] = f[16 + i];
    unsigned cap = (unsigned)f[34] | (unsigned)f[35] << 8;
    char ssid[33];
    unsigned ssid_len = 0;
    int have_ssid = 0, named = 0;
    unsigned chan = 0, sec = (cap & 0x10) ? 1 : 0;
    u8 rsn[40];
    unsigned rsn_len = 0;
    unsigned rates = 0, ht = 0, ht_mcs0 = 0, ht_cap = 0, vht = 0, vht_map = 0, ht_sec = 0, ht_w40 = 0, vht_w = 0, vht_seg0 = 0, ht_ampdu = 0;
    u32 vht_cap32 = 0;
    for (unsigned p = 36; p + 2 <= len;)
    {
        unsigned tag = f[p], tl = f[p + 1];
        if (p + 2 + tl > len)
            break; /* an element that runs past the frame: stop reading */
        const volatile u8 *d = f + p + 2;
        if (tag == 61 && tl >= 2) /* HT operation: the primary channel, then the secondary channel offset (1 = above, 3 = below) and whether 40 MHz is in use */
        {
            ht_sec = d[1] & 3;
            ht_w40 = (d[1] >> 2) & 1;
        }
        else if (tag == 192 && tl >= 3) /* VHT operation: the width (0 = 20 or 40 MHz, 1 = 80 MHz ...) and the centre channel of the 80 MHz channel */
        {
            vht_w = d[0];
            vht_seg0 = d[1];
        }
        if (tag == 0 && !have_ssid && tl <= 32)
        {
            for (unsigned i = 0; i < tl; i++)
            {
                unsigned c = d[i];
                ssid[i] = c >= 32 && c < 127 ? (char)c : '?';
                if (c)
                    named = 1;
            }
            ssid_len = named ? tl : 0; /* a hidden network sends an empty name or only zeros */

            have_ssid = 1;
        }
        else if (tag == 1 || tag == 50) /* supported rates / extended supported rates: the rate in units of 500 kb/s, the top bit marks a basic rate */
        {
            static const u8 UNITS[12] = {2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108};
            for (unsigned i = 0; i < tl; i++)
                for (unsigned k = 0; k < 12; k++)
                    if ((d[i] & 0x7F) == UNITS[k])
                        rates |= 1u << k;
        }
        else if (tag == 45 && tl >= 26) /* HT capabilities: capability bits (2), A-MPDU parameters (1), then the receive MCS set */
        {
            ht = 1;
            ht_cap = (unsigned)d[0] | (unsigned)d[1] << 8;
            ht_mcs0 = d[3];
            ht_ampdu = d[2];
        }
        else if (tag == 191 && tl >= 12) /* VHT capabilities: capability bits (4), then the receive MCS map (2) ... */
        {
            vht = 1;
            vht_map = (unsigned)d[4] | (unsigned)d[5] << 8;
            vht_cap32 = (u32)d[0] | (u32)d[1] << 8 | (u32)d[2] << 16 | (u32)d[3] << 24;
        }
        else if (tag == 3 && tl >= 1)
            chan = d[0];
        else if (tag == 61 && tl >= 1 && !chan)
            chan = d[0];
        else if (tag == 48)
        {
            sec = 3;
            rsn_len = tl <= sizeof rsn ? tl : 0;
            for (unsigned i = 0; i < rsn_len; i++)
                rsn[i] = d[i];
        }
        else if (tag == 221 && tl >= 4 && d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xF2 && d[3] == 1 && sec < 2)
            sec = 2;
        p += 2 + tl;
    }
    if (!have_ssid)
        return;
    ssid[ssid_len] = 0;
    for (unsigned i = 0; i < SC.n; i++)
    {
        ScanAp *a = &SC.ap[i];
        int same = 1;
        for (int k = 0; k < 6; k++)
            if (a->bssid[k] != bssid[k])
                same = 0;
        if (same)
        {
            a->beacons++;
            return;
        }
    }
    if (SC.n >= SCAN_AP_MAX)
    {
        SC.dropped++;
        return;
    }
    ScanAp *a = &SC.ap[SC.n++];
    for (int k = 0; k < 6; k++)
        a->bssid[k] = bssid[k];
    for (unsigned i = 0; i <= ssid_len; i++)
        a->ssid[i] = ssid[i];
    a->channel = (u8)(chan ? chan : SC.cur_channel);
    a->sec = (u8)sec;
    a->heard_on = (u8)SC.cur_channel;
    a->beacons = 1;
    a->cap = (u16)cap;
    a->rsn_len = (u8)rsn_len;
    for (unsigned i = 0; i < rsn_len; i++)
        a->rsn[i] = rsn[i];
    a->rates = (u16)rates;
    a->ht = (u8)ht;
    a->ht_mcs0 = (u8)ht_mcs0;
    a->ht_cap = (u16)ht_cap;
    a->vht = (u8)vht;
    a->vht_map = (u16)vht_map;
    a->ht_sec = (u8)ht_sec;
    a->ht_w40 = (u8)ht_w40;
    a->ht_ampdu = (u8)ht_ampdu;
    a->vht_w = (u8)vht_w;
    a->vht_seg0 = (u8)vht_seg0;
    a->vht_cap = vht_cap32;
}

/* tune to the i-th channel of the scan; 0 if the synthesizer did not take it (the channel is skipped) */
int scan_chan_begin(unsigned i)
{
    SC.cur_channel = SCAN_CHANNELS[i];
    set_channel_20(SC.cur_channel, PH.ch_param, EF.swing2g, EF.swing5g);
    if ((rf_read(0x18) & RF18_CHANNEL_MASK) != SC.cur_channel)
        return 0;
    SC.channels_ok++;
    return 1;
}

void scan_start_state(void)
{
    SC.n = SC.dropped = SC.channels_ok = 0;
    for (unsigned i = 0; i < SCAN_NCH; i++)
        SC.per_channel[i] = 0;
    SC.state = 1;
    rx_restart();
    wr32(R_RCR, RCR_RX_DEFAULT);
    wr8(R_CR, MAC_TRX_ENABLE);
}

void scan_end_state(void)
{
    SC.beacons = RX.beacons;
    RX.state = RXS_OK;
}

/* the whole scan in one go: for each channel, switch, listen, collect. The receiver stays on all the time. (The Wi-Fi window uses the stepwise
 * version of stage 11 instead, which does not hold up the rest of the system.) */
void scan_run(void)
{
    scan_start_state();
    for (unsigned i = 0; i < SCAN_NCH; i++)
    {
        if (!scan_chan_begin(i))
            continue;
        unsigned before = RX.frames;
        for (unsigned ms = 0; ms < SCAN_DWELL_MS; ms++)
        {
            udelay(1000);
            rx_poll();
        }
        SC.per_channel[i] = RX.frames - before;
    }
    scan_end_state();
}

/* The result of the last scan as lines of text, for the SYS_WIFISCAN system call (net.h: net_wifi_scan_line). Line 0 is a summary, lines
 * 1 .. n one network each, line n + 1 the frames heard on every channel. Returns the length of the line, or -1 if there is no such line
 * (no scan yet, or past the end). */
static const char *const SEC_NAME[] = {"open", "WEP", "WPA", "WPA2"};

static void put_pad(NicText *t, const char *s, unsigned width)
{
    unsigned n = 0;
    while (s[n])
        n++;
    nic_puts(t, s);
    for (; n < width; n++)
        nic_putc(t, ' ');
}

int rtl8821ce_scan_line(unsigned index, char *buf, unsigned cap)
{
    if (!SC.state || !buf || cap < 2)
        return -1;
    NicText t;
    nic_text_init(&t, buf, cap);
    if (index == 0)
    {
        unsigned f2 = 0, f5 = 0;
        for (unsigned i = 0; i < SCAN_NCH; i++)
            if (SCAN_CHANNELS[i] <= 14)
                f2 += SC.per_channel[i];
            else
                f5 += SC.per_channel[i];
        nic_putd(&t, SC.n);
        nic_puts(&t, " networks, ");
        nic_putd(&t, SC.channels_ok);
        nic_putc(&t, '/');
        nic_putd(&t, SCAN_NCH);
        nic_puts(&t, " channels tuned, frames 2.4G ");
        nic_putd(&t, f2);
        nic_puts(&t, " 5G ");
        nic_putd(&t, f5);
        nic_puts(&t, ", beacons ");
        nic_putd(&t, SC.beacons);
        if (SC.dropped)
        {
            nic_puts(&t, ", not listed ");
            nic_putd(&t, SC.dropped);
        }
    }
    else if (index <= SC.n)
    {
        const ScanAp *a = &SC.ap[index - 1];
        nic_puts(&t, "ch ");
        if (a->channel < 100)
            nic_putc(&t, ' ');
        if (a->channel < 10)
            nic_putc(&t, ' ');
        nic_putd(&t, a->channel);
        nic_puts(&t, "  ");
        put_pad(&t, SEC_NAME[a->sec & 3], 4);
        nic_puts(&t, "  ");
        for (int i = 0; i < 6; i++)
        {
            nic_puth(&t, a->bssid[i], 2);
            if (i < 5)
                nic_putc(&t, ':');
        }
        nic_puts(&t, "  ");
        nic_puts(&t, a->ssid[0] ? a->ssid : "(hidden)");
        nic_puts(&t, "  x");
        nic_putd(&t, a->beacons);
    }
    else if (index == SC.n + 1)
    {
        nic_puts(&t, "frames per channel:");
        for (unsigned i = 0; i < SCAN_NCH; i++)
        {
            nic_putc(&t, ' ');
            nic_putd(&t, SCAN_CHANNELS[i]);
            nic_putc(&t, ':');
            nic_putd(&t, SC.per_channel[i]);
        }
    }
    else
        return -1;
    return (int)t.n;
}
