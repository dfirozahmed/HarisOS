/* RTL8821CE: the keys in the chip and the WPA2 4-way handshake (stages 6c-2, 6c-1). */
#include "rtl8821ce_internal.h"

/* ---- stage 6c-2: the keys in the chip ----
 * With the keys from the handshake the chip can encrypt and decrypt the data frames itself (AES-CCMP). It keeps the keys in a table of 32
 * entries, the "CAM": a key (16 bytes), the address it belongs to, its type, whether it is a group key, and the key id. An entry is written
 * as eight 32-bit words, each through a command register (0x670) after the data register (0x674), exactly as Linux' rtw_sec_write_cam does.
 * The first four entries are the group keys, found by their key id (the "default key search" mode Linux uses): the group key of the access
 * point (key id 1 here) goes to entry 1. The pairwise key goes to the first free entry after them (4), found by the address of the access point.
 * The security engine itself is switched on first (bit 9 of the command register, and the decrypt / default key bits of the config register). */
#define R_SEC_CMD 0x0670
#define R_SEC_WRITE 0x0674
#define R_SEC_CONFIG 0x0680
#define CAM_TYPE_AES 4
#define CAM_PAIRWISE_ENTRY 4

static void sec_enable(void)
{
    wr16(R_CR, (u16)(rd16(R_CR) | BIT(9))); /* RTW_SEC_ENGINE_EN */
    /* decrypt on receive (BIT 3), encrypt on transmit (BIT 2), and the default key search for unicast and broadcast, both directions */
    wr16(R_SEC_CONFIG, (u16)(rd16(R_SEC_CONFIG) | BIT(2) | BIT(3) | BIT(0) | BIT(1) | BIT(6) | BIT(7)));
}

/* rtw_sec_write_cam: entry `idx` = key (16 bytes) with key id `keyidx`, group or pairwise, for the address `addr` */
void cam_write(unsigned idx, const u8 *key, unsigned keyidx, int group, const u8 *addr)
{
    u32 base = idx << 3; /* eight words per entry */
    for (int i = 7; i >= 0; i--)
    {
        u32 content;
        switch (i)
        {
        case 0:
            content = (keyidx & 3) | ((u32)CAM_TYPE_AES << 2) | ((u32)(group ? 1 : 0) << 6) | BIT(15) | (u32)addr[0] << 16 | (u32)addr[1] << 24;
            break;
        case 1:
            content = (u32)addr[2] | (u32)addr[3] << 8 | (u32)addr[4] << 16 | (u32)addr[5] << 24;
            break;
        case 6:
        case 7:
            content = 0;
            break;
        default:
        {
            unsigned j = (unsigned)(i - 2) << 2;
            content = (u32)key[j] | (u32)key[j + 1] << 8 | (u32)key[j + 2] << 16 | (u32)key[j + 3] << 24;
            break;
        }
        }
        wr32(R_SEC_WRITE, content);
        wr32(R_SEC_CMD, BIT(16) | BIT(31) | (base + (u32)i)); /* write enable, poll */
    }
}

/* entries 0 .. CAM_PAIRWISE_ENTRY off again (a word 0 without the "valid" bit): before a new association, so that no key of the old one is left */
void cam_clear(void)
{
    for (unsigned idx = 0; idx <= CAM_PAIRWISE_ENTRY; idx++)
    {
        wr32(R_SEC_WRITE, 0);
        wr32(R_SEC_CMD, BIT(16) | BIT(31) | (idx << 3));
    }
}

/* ---- stage 6c-1: the WPA2 4-way handshake over the air ----
 * After the association the access point wants proof that we know the passphrase, without anybody sending the passphrase: it sends a random
 * number (message 1), we answer with ours and a signature that only someone with the same key could make (message 2), it checks, sends the
 * group key encrypted and signed (message 3), and we confirm (message 4). The messages are EAPOL frames, carried in ordinary data frames:
 * the 802.11 data header, the 8 byte LLC/SNAP header AA AA 03 00 00 00 + ethertype 88 8E, then the EAPOL packet. The key arithmetic is
 * in wpa.c; this part moves the frames. The keys are not installed in the chip yet (the next stage): no data can be sent after this one. */
#define EAPOL_LLC_LEN 8

static int mac_eq(const volatile u8 *a, const u8 *b) { return mac_is(a, b); }

Anon_EAPQ EAPQ;

static int HS_LISTEN; /* the receive path keeps EAPOL packets only while the handshake runs */

/* a data frame from the receive path (without FCS): an EAPOL packet from our access point? */
void eapol_note(const volatile u8 *f, unsigned len)
{
    if (!HS_LISTEN || len < 24 + EAPOL_LLC_LEN + 4)
        return;
    unsigned fc0 = f[0], fc1 = f[1];
    if ((fc0 & 0x0C) != 0x08 || (fc1 & 3) != 2) /* a data frame, from the distribution system (the access point) */
        return;
    if (!mac_eq(f + 4, EF.mac) || !mac_eq(f + 10, CONN.bssid))
        return;
    unsigned hdr = 24 + ((fc0 & 0x80) ? 2 : 0); /* the QoS control field of a QoS data frame */
    if (fc1 & 0x80)
        hdr += 4; /* an HT control field */
    static const u8 LLC[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    if (len < hdr + EAPOL_LLC_LEN + 4)
        return;
    for (int i = 0; i < 8; i++)
        if (f[hdr + (unsigned)i] != LLC[i])
            return;
    unsigned n = len - hdr - EAPOL_LLC_LEN;
    if (EAPQ.pending || n > EAPOL_MAX)
    {
        EAPQ.dropped++;
        return;
    }
    for (unsigned i = 0; i < n; i++)
        EAPQ.b[i] = f[hdr + EAPOL_LLC_LEN + i];
    EAPQ.len = n;
    EAPQ.pending = 1;
}

/* random bytes for our nonce. On the PC: RDRAND when the processor has it, mixed with the time stamp counter read at many unequal moments; the
 * result is hashed. (Not a certified random generator: the nonce must only be unpredictable and different every time.) */
#ifdef HARIS_HOST
void rtl8821ce_host_random(u8 *out, unsigned n);
static void wpa_random(u8 *out, unsigned n) { rtl8821ce_host_random(out, n); }
#else
static u64 read_tsc(void)
{
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (u64)hi << 32 | lo;
}

static int have_rdrand(void)
{
    unsigned a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (c >> 30) & 1;
}

static void wpa_random(u8 *out, unsigned n)
{
    Sha1 s;
    u8 digest[20];
    unsigned counter = 0;
    int rd = have_rdrand();
    while (n)
    {
        sha1_init(&s);
        for (int i = 0; i < 16; i++)
        {
            u64 v = read_tsc();
            sha1_update(&s, &v, 8);
            for (unsigned k = 0; k < (read_tsc() & 0x3F); k++) /* the time of the next read depends on how long this loop ran */
                io_in8(0x80);
        }
        if (rd)
            for (int i = 0; i < 4; i++)
            {
                u64 r = 0;
                unsigned char ok = 0;
                for (int tries = 0; tries < 10 && !ok; tries++)
                    __asm__ volatile("rdrand %0; setc %1" : "=r"(r), "=qm"(ok));
                sha1_update(&s, &r, 8);
            }
        sha1_update(&s, EF.mac, 6);
        sha1_update(&s, &counter, sizeof counter);
        sha1_final(&s, digest);
        counter++;
        unsigned take = n < 20 ? n : 20;
        for (unsigned i = 0; i < take; i++)
            out[i] = digest[i];
        out += take;
        n -= take;
    }
}
#endif

/* an EAPOL packet to the access point in a data frame: sent through the management queue (no security, hardware sequence number) */
static int tx_eapol(const u8 *eapol, unsigned len, unsigned channel)
{
    u8 frame[24 + EAPOL_LLC_LEN + EAPOL_MAX];
    unsigned n = mgmt_header(frame, 0x08, CONN.bssid); /* data, no flags yet */
    frame[1] = 0x01;                                   /* to the distribution system: it goes to the access point, address 3 is the destination */
    static const u8 LLC[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    for (int i = 0; i < 8; i++)
        frame[n++] = LLC[i];
    for (unsigned i = 0; i < len; i++)
        frame[n++] = eapol[i];
    return tx_mgmt(frame, n, channel);
}

Anon_HS HS;
Wpa4 W4;
#define HS_WAIT_MS 6000
#define HS_DECRYPT_MS 1500

void handshake_run(void)
{
    HS.state = 1;
    HS.result = 0;
    HS.keys = 0;
    HS.dec = HS.icv = 0;
    HS.rx_msgs = HS.tx_msgs = 0;
    EAPQ.pending = 0;
    EAPQ.dropped = 0;
    wpa_pmk(CFG.psk, CFG.psk_len, CFG.ssid, CFG.ssid_len, HS.pmk);
    static const u8 RSN[22] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0}; /* as in the association request */
    wpa4_init(&W4, HS.pmk, CONN.bssid, EF.mac, RSN, 22);
    HS_LISTEN = 1;
    u8 snonce[32], reply[EAPOL_MAX];
    for (unsigned ms = 0; ms < HS_WAIT_MS; ms++)
    {
        udelay(1000);
        rx_poll();
        if (!EAPQ.pending)
            continue;
        HS.rx_msgs++;
        wpa_random(snonce, 32);
        unsigned rl = 0;
        int r = wpa4_rx(&W4, EAPQ.b, EAPQ.len, snonce, reply, &rl);
        EAPQ.pending = 0;
        if (r == WPA4_SEND_MSG2 || r == WPA4_SEND_MSG4)
        {
            if (tx_eapol(reply, rl, CONN.channel) == 0)
                HS.tx_msgs++;
            if (r == WPA4_SEND_MSG4)
            {
                HS.result = HS_OK;
                break;
            }
        }
        else if (r == WPA4_E_MIC || r == WPA4_E_NONCE || r == WPA4_E_KEYDATA || r == WPA4_E_NO_GTK || r == WPA4_E_FORMAT)
        {
            HS.result = r; /* the access point's message 3 does not fit what we derived: no use waiting for more */
            break;
        }
        /* WPA4_IGNORED, a replayed counter, a short frame: wait for the next one */
    }
    if (HS.result == HS_OK)
    {
        /* message 4 is out: the keys go into the chip, and for a while we listen to see whether it can decrypt what the access point sends */
        static const u8 BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        sec_enable();
        cam_write(CAM_PAIRWISE_ENTRY, W4.ptk + 32, 0, 0, CONN.bssid);
        cam_write(W4.gtk_id, W4.gtk, W4.gtk_id, 1, BCAST);
        HS.keys = 1;
        RX.decrypted = RX.icv_err = 0;
        listen_ms(HS_DECRYPT_MS);
        HS.dec = RX.decrypted;
        HS.icv = RX.icv_err;
    }
    HS_LISTEN = 0;
    for (unsigned i = 0; i < sizeof HS.pmk; i++) /* the master key is not kept around */
        ((volatile u8 *)HS.pmk)[i] = 0;
}
