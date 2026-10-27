/* RTL8821CE: which network to join, authentication and association (stages 5b-0, 5b-1). */
#include "rtl8821ce_internal.h"

/* ---- stage 5b-0: which network to join ----
 * WIFI.CFG sits in the top folder of the boot disk (pconfig/WIFI.CFG in the repository, which git ignores): two lines, "ssid=NAME" and
 * "psk=PASSPHRASE". The passphrase is a secret: it is kept in this module's memory for the key derivation of the WPA2 handshake, and
 * nothing here prints it, logs it, or puts a part of it into a status line or a test message. */
#define CFG_NAME "WIFI.CFG"
#define CFG_FILE_MAX 512

enum { CFG_RATE_LEGACY, CFG_RATE_HT, CFG_RATE_AUTO }; /* fixed 24 Mb/s as before / HT (802.11n) / HT and VHT (802.11ac) where the access point has them */
Anon_CFG CFG;

/* the text between the end of the key and the end of the line, without a trailing carriage return or spaces; -1 if the line is not `key` */
static int cfg_value(const char *line, unsigned n, const char *key, const char **value)
{
    unsigned k = 0;
    while (key[k])
        k++;
    if (n < k + 1)
        return -1;
    for (unsigned i = 0; i < k; i++)
        if (line[i] != key[i])
            return -1;
    if (line[k] != '=')
        return -1;
    *value = line + k + 1;
    unsigned len = n - k - 1;
    while (len && (line[k + len] == '\r' || line[k + len] == ' ' || line[k + len] == '\t'))
        len--;
    return (int)len;
}

/* the lines of the file (size bytes in buf) into CFG; sets CFG.state */
static void cfg_parse(const char *buf, unsigned size)
{
    CFG.state = CFG_NONE;
    CFG.rate = CFG_RATE_AUTO;
    CFG.bw = 2;
    CFG.ampdu = 1;
    int got_ssid = 0, got_psk = 0;
    unsigned start = 0;
    for (unsigned i = 0; i <= size; i++)
    {
        if (i < size && buf[i] != '\n')
            continue;
        const char *value = 0;
        int len = cfg_value(buf + start, i - start, "ssid", &value);
        if (len >= 0)
        {
            got_ssid = 1;
            if (len < 1 || len > 32)
            {
                CFG.state = CFG_BAD;
                CFG.why = "ssid";
                return;
            }
            for (int k = 0; k < len; k++)
                CFG.ssid[k] = value[k];
            CFG.ssid[len] = 0;
            CFG.ssid_len = (unsigned)len;
        }
        len = cfg_value(buf + start, i - start, "rate", &value);
        if (len >= 0) /* anything that is not recognised stays "auto": a typo must not stop the connection */
        {
            if (len == 6 && value[0] == 'l' && value[1] == 'e' && value[2] == 'g' && value[3] == 'a' && value[4] == 'c' && value[5] == 'y')
                CFG.rate = CFG_RATE_LEGACY;
            else if (len == 2 && value[0] == 'h' && value[1] == 't')
                CFG.rate = CFG_RATE_HT;
        }
        len = cfg_value(buf + start, i - start, "ampdu", &value);
        if (len == 3 && value[0] == 'o' && value[1] == 'f' && value[2] == 'f')
            CFG.ampdu = 0;
        else if (len == 2 && value[0] == 'o' && value[1] == 'n')
            CFG.ampdu = 1;
        len = cfg_value(buf + start, i - start, "bw", &value);
        if (len == 2 && value[0] == '2' && value[1] == '0')
            CFG.bw = 0;
        else if (len == 2 && value[0] == '4' && value[1] == '0')
            CFG.bw = 1;
        else if (len == 2 && value[0] == '8' && value[1] == '0')
            CFG.bw = 2;
        len = cfg_value(buf + start, i - start, "psk", &value);
        if (len >= 0)
        {
            got_psk = 1;
            if (len < 8 || len > 63)
            {
                CFG.state = CFG_BAD;
                CFG.why = "psk";
                return;
            }
            for (int k = 0; k < len; k++)
                CFG.psk[k] = value[k];
            CFG.psk[len] = 0;
            CFG.psk_len = (unsigned)len;
        }
        start = i + 1;
    }
    if (!got_ssid)
    {
        CFG.state = CFG_BAD;
        CFG.why = "ssid";
        return;
    }
    if (!got_psk)
    {
        CFG.state = CFG_BAD;
        CFG.why = "psk";
        return;
    }
    CFG.state = CFG_OK;
}

Anon_TARGET TARGET;

void pick_target(void)
{
    TARGET.found = 0;
    if (CFG.state != CFG_OK || !SC.state)
        return;
    for (unsigned i = 0; i < SC.n; i++)
    {
        const ScanAp *a = &SC.ap[i];
        if (a->sec != 3 || a->ssid[0] == 0)
            continue;
        unsigned k = 0;
        while (k < CFG.ssid_len && a->ssid[k] == CFG.ssid[k])
            k++;
        if (k != CFG.ssid_len || a->ssid[k] != 0)
            continue;
        if (!TARGET.found || a->beacons > SC.ap[TARGET.index].beacons)
        {
            TARGET.found = 1;
            TARGET.index = i;
        }
    }
}

void cfg_load(void)
{
    CFG.state = CFG_NONE;
    CFG.why = 0;
    CFG.ssid_len = CFG.psk_len = 0;
    CFG.rate = CFG_RATE_AUTO; /* (the defaults of the optional lines; also used when no file is there and a network is picked in the Wi-Fi window) */
    CFG.bw = 2;
    CFG.ampdu = 1;
    volatile char *wipe = CFG.psk;
    for (unsigned i = 0; i < sizeof CFG.psk; i++)
        wipe[i] = 0;
    int node = exec_find(CFG_NAME);
    if (node < 0)
        return;
    const VfsNode *vn = vfs_node(node);
    if (!vn || vn->is_dir || vn->size == 0 || vn->size > CFG_FILE_MAX)
        return;
    char buf[CFG_FILE_MAX];
    if (vfs_read(node, 0, buf, vn->size) == (int)vn->size)
        cfg_parse(buf, vn->size);
    volatile char *wipe_buf = buf; /* the file's text does not stay on the stack */
    for (unsigned i = 0; i < sizeof buf; i++)
        wipe_buf[i] = 0;
}

/* ---- stage 5b-1: authentication and association ----
 * Joining a network takes two short exchanges of management frames with the access point (AP), both unicast to its BSSID:
 *   1. authentication: we send "algorithm 0 (open system), sequence 1, status 0"; it answers "algorithm 0, sequence 2, status 0 = fine".
 *      (Open system is only a formality; the real proof of the passphrase comes later, in the WPA2 handshake.)
 *   2. association: we send who we are (capability bits, listen interval, network name, rates, and in the RSN element the WPA2 security
 *      we want: group and pairwise cipher CCMP (AES), key management PSK); it answers with a status (0 = fine) and the association id.
 * Every frame a station receives that is addressed to its own MAC address is acknowledged by the chip itself, and our frames are
 * retried by the chip until the AP acknowledges. After association the AP expects the WPA2 handshake (the next stage); this stage
 * ends by telling the AP that we are leaving (deauthentication), so that no half-joined station is left behind.
 * Only 5 GHz and legacy OFDM rates are offered for now: no HT / VHT elements, so the link would be 54 Mb/s at best. */
#define CAP_ESS 0x0001
#define CAP_PRIVACY 0x0010


/* Can we join with the security the RSN element of this access point describes? The element: version (2), group cipher suite (4),
 * pairwise count (2) and suites (4 each), key management count (2) and suites (4 each), capabilities (2). A suite is the OUI 00-0F-AC
 * and a number: ciphers 2 = TKIP, 4 = CCMP; key management 2 = PSK (WPA2-Personal), 8 = SAE (WPA3). Capability bit 6 = management
 * frame protection REQUIRED, which needs a key exchange this driver does not have. */
int rsn_check(const u8 *e, unsigned len)
{
    if (len < 8 || le16(e) != 1)
        return RSN_BAD_VERSION;
    unsigned p = 2;
    int group_ccmp = e[p] == 0x00 && e[p + 1] == 0x0F && e[p + 2] == 0xAC && e[p + 3] == 4;
    p += 4;
    unsigned n = le16(e + p);
    p += 2;
    int ccmp = 0;
    for (unsigned i = 0; i < n; i++, p += 4)
    {
        if (p + 4 > len)
            return RSN_BAD_VERSION;
        if (e[p] == 0x00 && e[p + 1] == 0x0F && e[p + 2] == 0xAC && e[p + 3] == 4)
            ccmp = 1;
    }
    if (p + 2 > len)
        return RSN_BAD_VERSION;
    n = le16(e + p);
    p += 2;
    int psk = 0;
    for (unsigned i = 0; i < n; i++, p += 4)
    {
        if (p + 4 > len)
            return RSN_BAD_VERSION;
        if (e[p] == 0x00 && e[p + 1] == 0x0F && e[p + 2] == 0xAC && e[p + 3] == 2)
            psk = 1;
    }
    unsigned caps = p + 2 <= len ? le16(e + p) : 0;
    if (!ccmp)
        return RSN_NO_CCMP;
    if (!psk)
        return RSN_NO_PSK;
    if (caps & 0x0040)
        return RSN_PMF_REQUIRED;
    if (!group_ccmp)
        return RSN_GROUP;
    return RSN_OK;
}


Anon_CONN CONN;

/* what we offer in the association request, for the access point to choose the rates from (stage 8): HT with one spatial stream when the access
 * point has HT and the configuration allows it, VHT as well on 5 GHz. Nothing that the card is not ready for is claimed: no aggregation (see
 * "stage 9"), no beamforming, no STBC, no LDPC. */
int use_ht(void) { return CFG.rate >= CFG_RATE_HT && CONN.ht; }
int use_vht(void) { return CFG.rate >= CFG_RATE_AUTO && CONN.ht && CONN.vht && CONN.channel > 14; }

/* the channel to use (stage 9): the width the access point uses, but not wider than the configuration says (bw=). The access point says in its
 * HT operation element whether it uses 40 MHz and on which side of the primary channel the other half is, and in the VHT operation element
 * whether it uses 80 MHz and which channel is the centre of that (the primary channel is one of the four 20 MHz channels in it). Anything that does
 * not fit together (a centre that is not 2 or 6 channel numbers from the primary channel) is not used: then the narrower width stays. */
static void chan_plan(void)
{
    Chan *c = &CONN.cc;
    c->prim = c->center = CONN.channel;
    c->bw = c->sc = 0;
    if (!use_ht() || CFG.bw == 0)
        return;
    if (CONN.ht_w40 && (CONN.ht_sec == 1 || CONN.ht_sec == 3))
    {
        c->bw = 1;
        c->center = CONN.ht_sec == 1 ? CONN.channel + 2 : CONN.channel - 2;
        c->sc = CONN.ht_sec == 1 ? 2 : 1; /* the other half above: the primary channel is the lower one */
    }
    if (CFG.bw >= 2 && c->bw == 1 && use_vht() && CONN.vht_w == 1)
    {
        int d = (int)CONN.channel - (int)CONN.vht_seg0;
        unsigned sc = d == 2 ? 1 : d == -2 ? 2 : d == 6 ? 3 : d == -6 ? 4 : 0;
        if (sc)
        {
            c->bw = 2;
            c->center = CONN.vht_seg0;
            c->sc = sc;
        }
    }
}

void conn_set_caps(const ScanAp *ap)
{
    CONN.rates = ap->rates;
    CONN.ht = ap->ht;
    CONN.ht_mcs0 = ap->ht_mcs0;
    CONN.ht_cap = ap->ht_cap;
    CONN.vht = ap->vht;
    CONN.vht_map = ap->vht_map;
    CONN.vht_cap = ap->vht_cap;
    CONN.ht_sec = ap->ht_sec;
    CONN.ht_w40 = ap->ht_w40;
    CONN.vht_w = ap->vht_w;
    CONN.vht_seg0 = ap->vht_seg0;
    CONN.ht_ampdu = ap->ht_ampdu;
    chan_plan();
}

int mac_is(const volatile u8 *a, const u8 *b)
{
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

unsigned vget16(const volatile u8 *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

/* a management frame from the receive path (without FCS): is it an answer from the access point we talk to? */
void conn_note(const volatile u8 *f, unsigned len)
{
    unsigned fc0 = f[0];
    /* (an access point that goes down says goodbye to everybody: a deauthentication / disassociation may be addressed to a group address) */
    int to_us = len >= 10 && mac_is(f + 4, EF.mac);
    int goodbye = len >= 10 && (f[4] & 1) && (fc0 == 0xC0 || fc0 == 0xA0);
    if (!CONN.state || len < 26 || (!to_us && !goodbye) || !mac_is(f + 10, CONN.bssid))
        return;
    if (fc0 == 0xB0 && len >= 30 && vget16(f + 24) == 0 && vget16(f + 26) == 2) /* authentication: open system, second message */
    {
        CONN.auth_status = vget16(f + 28);
        CONN.auth_seen = 1;
    }
    else if (fc0 == 0x10 && len >= 30) /* association response: status, then the association id (the top two bits are always set) */
    {
        CONN.assoc_status = vget16(f + 26);
        CONN.aid = len >= 30 ? vget16(f + 28) & 0x3FFF : 0;
        CONN.assoc_seen = 1;
    }
    else if (fc0 == 0xC0 || fc0 == 0xA0) /* deauthentication / disassociation: the reason code */
        CONN.dropped = vget16(f + 24) + 1;
}

/* the 24 byte management header to a unicast address: frame control, duration 0, receiver, us, BSSID, sequence (filled in by the chip) */
unsigned mgmt_header(u8 *f, unsigned fc0, const u8 *bssid)
{
    unsigned n = 0;
    f[n++] = (u8)fc0;
    f[n++] = 0x00;
    f[n++] = 0;
    f[n++] = 0;
    for (int i = 0; i < 6; i++)
        f[n++] = bssid[i];
    for (int i = 0; i < 6; i++)
        f[n++] = EF.mac[i];
    for (int i = 0; i < 6; i++)
        f[n++] = bssid[i];
    f[n++] = 0;
    f[n++] = 0;
    return n;
}

static unsigned put_u16(u8 *f, unsigned n, unsigned v)
{
    f[n++] = (u8)v;
    f[n++] = (u8)(v >> 8);
    return n;
}

unsigned build_auth(u8 *f, const u8 *bssid)
{
    unsigned n = mgmt_header(f, 0xB0, bssid);
    n = put_u16(f, n, 0); /* algorithm: open system */
    n = put_u16(f, n, 1); /* transaction sequence number 1 */
    n = put_u16(f, n, 0); /* status: success (the field is reserved in a request) */
    return n;
}

unsigned build_deauth(u8 *f, const u8 *bssid)
{
    unsigned n = mgmt_header(f, 0xC0, bssid);
    return put_u16(f, n, 3); /* reason 3: the station is leaving the network */
}

/* the association request; the security it asks for is what rsn_check() accepted: CCMP for both ciphers and PSK */
unsigned build_assoc(u8 *f, const u8 *bssid, const char *ssid, unsigned ssid_len, unsigned channel)
{
    unsigned n = mgmt_header(f, 0x00, bssid);
    n = put_u16(f, n, CAP_ESS | CAP_PRIVACY | (channel <= 14 ? 0x0400 : 0) | (use_ht() ? 0x0200 : 0)); /* an infrastructure station, with privacy (and short slot time at 2.4 GHz, QoS when it is HT) */
    n = put_u16(f, n, 10);                                                    /* listen interval, in beacon intervals */
    f[n++] = 0;                                                               /* SSID */
    f[n++] = (u8)ssid_len;
    for (unsigned i = 0; i < ssid_len; i++)
        f[n++] = (u8)ssid[i];
    f[n++] = 1; /* supported rates: the legacy OFDM rates, 6, 12 and 24 Mb/s basic (the AP's own basic rates must be among ours) */
    f[n++] = 8;
    static const u8 R5[8] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
    static const u8 R2[8] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24};
    for (int i = 0; i < 8; i++)
        f[n++] = channel <= 14 ? R2[i] : R5[i];
    if (channel <= 14)
    {
        f[n++] = 50; /* extended supported rates: 24, 36, 48, 54 Mb/s */
        f[n++] = 4;
        f[n++] = 0x30;
        f[n++] = 0x48;
        f[n++] = 0x60;
        f[n++] = 0x6C;
    }
    static const u8 RSN[22] = {48, 20, 1, 0,                  /* the RSN element: version 1 */
                               0x00, 0x0F, 0xAC, 4,           /* group cipher CCMP */
                               1, 0, 0x00, 0x0F, 0xAC, 4,     /* one pairwise cipher: CCMP */
                               1, 0, 0x00, 0x0F, 0xAC, 2,     /* one key management: PSK */
                               0, 0};                         /* no capabilities: no management frame protection */
    for (int i = 0; i < 22; i++)
        f[n++] = RSN[i];
    if (use_ht())
    {
        unsigned bw = CONN.cc.bw;
        f[n++] = 45; /* HT capabilities: spatial multiplexing power save off, short guard interval; 40 MHz (and short GI at 40 MHz) when that is the channel */
        f[n++] = 26;
        n = put_u16(f, n, bw >= 1 ? 0x006E : 0x002C);
        f[n++] = CFG.ampdu ? 0x1E : 0; /* A-MPDU parameters: the longest aggregate we take (exponent 2: 32 KB) and the least spacing between its frames (16 us) */
        f[n++] = 0xFF; /* the receive MCS set: MCS 0 to 7 (one spatial stream) */
        for (int i = 0; i < 9; i++)
            f[n++] = 0;
        n = put_u16(f, n, bw >= 1 ? 150 : 72); /* the highest rate we receive: 72 Mb/s at 20 MHz, 150 at 40 MHz */
        for (int i = 0; i < 11; i++) /* the transmit MCS set, extended capabilities, beamforming, antenna selection: none */
            f[n++] = 0;
    }
    if (use_vht())
    {
        unsigned bw = CONN.cc.bw;
        f[n++] = 191; /* VHT capabilities: nothing special (no 160 MHz, no beamforming, no STBC); short guard interval at 80 MHz; MCS 0 to 8 (to 9 when the channel is wider than 20 MHz) on one stream, none on the others */
        f[n++] = 12;
        n = put_u16(f, n, bw == 2 ? 0x0020 : 0);
        n = put_u16(f, n, CFG.ampdu ? 0x0100 : 0); /* ... the high half: the longest aggregate we take, exponent 2 (bits 23-25): 32 KB */
        n = put_u16(f, n, bw >= 1 ? 0xFFFE : 0xFFFD); /* receive MCS map */
        n = put_u16(f, n, bw == 2 ? 433 : bw == 1 ? 200 : 87); /* the highest receive rate (short guard interval): 86.7 / 200 / 433 Mb/s */
        n = put_u16(f, n, bw >= 1 ? 0xFFFE : 0xFFFD); /* transmit MCS map */
        n = put_u16(f, n, bw == 2 ? 433 : bw == 1 ? 200 : 87);
        if (bw < 2)
        {
            f[n++] = 199; /* operating mode notification: a VHT station must take 80 MHz, so it says up front that it listens on 20 (or 40) MHz and on one stream only */
            f[n++] = 1;
            f[n++] = (u8)bw;
        }
    }
    if (use_ht())
    {
        f[n++] = 221; /* WMM (QoS) information element: an HT station is a QoS station */
        f[n++] = 7;
        f[n++] = 0x00;
        f[n++] = 0x50;
        f[n++] = 0xF2;
        f[n++] = 2;
        f[n++] = 0;
        f[n++] = 1;
        f[n++] = 0;
    }
    return n;
}

#define CONN_TRIES 3
#define CONN_WAIT_MS 120

/* send a frame until the wanted answer has arrived (up to CONN_TRIES times); returns how many frames were sent */
unsigned conn_exchange(const u8 *frame, unsigned len, unsigned channel, int *flag)
{
    unsigned sent = 0;
    for (int t = 0; t < CONN_TRIES && !*flag; t++)
    {
        if (tx_mgmt(frame, len, channel) == 0)
            sent++;
        for (int ms = 0; ms < CONN_WAIT_MS && !*flag; ms++)
        {
            udelay(1000);
            rx_poll();
        }
    }
    return sent;
}

void handshake_run(void);
void connect_exchange(void);

/* the whole attempt: authenticate, associate, leave */
static int CONN_ENABLE = 1; /* the host tests of the earlier stages switch it off */

void connect_try(void)
{
    CONN.state = 0;
    if (!CONN_ENABLE || !TARGET.found || TXP.variant < 0 || !TXP.sent || SC.ap[TARGET.index].rsn_len == 0)
        return;
    const ScanAp *ap = &SC.ap[TARGET.index];
    if (rsn_check(ap->rsn, ap->rsn_len) != RSN_OK)
        return;
    for (int i = 0; i < 6; i++)
        CONN.bssid[i] = ap->bssid[i];
    CONN.channel = ap->channel;
    conn_set_caps(ap);
    tx_variant_setup(TXP.variant < 0 ? 0 : TXP.variant); /* the way the DMA was found to start, and fresh ring pointers */
    tune_chan(&CONN.cc);
    rx_restart();
    wr32(R_RCR, RCR_RX_DEFAULT);
    wr8(R_CR, MAC_TRX_ENABLE);
    connect_exchange();
}
