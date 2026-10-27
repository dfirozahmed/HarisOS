/* RTL8821CE: block ack agreements (ADDBA / DELBA action frames). */
#include "rtl8821ce_internal.h"

/* ---- stage 10, the agreements (ADDBA / DELBA action frames) ----
 * An action frame (frame control 0xD0) of the category "block acknowledgement" (3) has an action number and its fields:
 *   0 ADDBA request   dialog token, parameters (bit 0 A-MSDU allowed, bit 1 immediate BA, bits 2..5 traffic class, bits 6..15 window in frames),
 *                     timeout, starting sequence control (sequence number << 4)
 *   1 ADDBA response  dialog token, status (0 = yes), parameters (the same), timeout
 *   2 DELBA           parameters (bit 11 the sender of the data ("initiator") ends it, bits 12..15 traffic class), reason
 * Receiving an ADDBA request from the access point: answered with yes (immediate BA, no A-MSDU, window BA_WIN) for traffic class 0, no (37, "request
 * declined") for the rest; from then on the frames of class 0 go through the reorder buffer. Sending data: once the link has carried some traffic
 * (BA_TX_TRIGGER frames) we ask for an agreement of our own; when the access point says yes, the descriptors of the data frames allow aggregation. */
#define BA_TX_TRIGGER 16     /* data frames sent before it is worth asking for an agreement */
#define BA_TX_WAIT_MS 1500   /* how long to wait for the answer, three tries */
#define BA_TX_COOL_MS 5000   /* the pause before a new request when the access point ended the agreement */

Anon_ACTQ ACTQ;

/* a management frame from the receive path (without FCS): an action frame of the access point to us? */
void act_note(const volatile u8 *f, unsigned len)
{
    if (!LINK.up || ACTQ.pending || len < 24 + 3 || !mac_is(f + 4, EF.mac) || !mac_is(f + 10, CONN.bssid))
        return;
    if (f[24] != 3) /* only the block acknowledgement category */
        return;
    unsigned n = len < sizeof ACTQ.b ? len : (unsigned)sizeof ACTQ.b;
    for (unsigned i = 0; i < n; i++)
        ACTQ.b[i] = f[i];
    ACTQ.len = n;
    ACTQ.pending = 1;
}

static int tx_action(const u8 *body, unsigned blen)
{
    u8 f[24 + 16];
    unsigned n = mgmt_header(f, 0xD0, CONN.bssid);
    for (unsigned i = 0; i < blen && n < sizeof f; i++)
        f[n++] = body[i];
    return tx_mgmt(f, n, CONN.channel);
}

void act_service(void)
{
    const u8 *b = ACTQ.b + 24; /* the category, the action number, the fields */
    unsigned blen = ACTQ.len - 24;
    unsigned action = b[1];
    if (action == 0 && blen >= 9) /* ADDBA request */
    {
        unsigned params = (unsigned)b[3] | (unsigned)b[4] << 8, tid = (params >> 2) & 0xF, buf = (params >> 6) & 0x3FF;
        unsigned ssn = ((unsigned)b[7] | (unsigned)b[8] << 8) >> 4;
        unsigned status = 0;
        if (!CFG.ampdu || !RA.on || tid != 0 || !((params >> 1) & 1)) /* only traffic class 0, immediate block acknowledgement, and only when we offered it */
            status = 37;
        if (buf == 0 || buf > BA_WIN)
            buf = BA_WIN;
        u8 r[9] = {3, 1, b[2], (u8)status, 0, 0, 0, 0, 0};
        unsigned rp = status ? params & ~0xFFC0u : 0x0002u | tid << 2 | buf << 6; /* immediate, no A-MSDU, our window */
        r[5] = (u8)rp;
        r[6] = (u8)(rp >> 8);
        if (status == 0)
            ba_rx_start(ssn); /* before the answer: the access point may send at once */
        tx_action(r, 9);
    }
    else if (action == 1 && blen >= 9) /* ADDBA response to ours */
    {
        unsigned status = (unsigned)b[3] | (unsigned)b[4] << 8, params = (unsigned)b[5] | (unsigned)b[6] << 8;
        if (BA.tx_state == 1 && b[2] == BA.tx_token)
        {
            if (status == 0 && ((params >> 2) & 0xF) == 0)
            {
                BA.tx_state = 2;
                BA.tx_buf = (params >> 6) & 0x3FF;
            }
            else
                BA.tx_state = 3; /* refused: not again on this connection */
        }
    }
    else if (action == 2 && blen >= 6) /* DELBA */
    {
        unsigned params = (unsigned)b[2] | (unsigned)b[3] << 8;
        if ((params >> 12) == 0)
        {
            if ((params >> 11) & 1) /* the access point ends the data it sends: our receive side */
                ba_rx_stop();
            else /* it ends what we send: we do not ask again at once */
            {
                BA.tx_state = 0;
                BA.tx_cool = now_ms() + BA_TX_COOL_MS;
            }
        }
    }
    ACTQ.pending = 0;
}

/* ask for an agreement for what we send, when the link has seen some traffic */
void ba_tx_service(u64 now)
{
    if (!CFG.ampdu || !RA.on || BA.tx_state == 2 || BA.tx_state == 3)
        return;
    if (BA.tx_state == 0)
    {
        if (LINK.tx_frames < BA_TX_TRIGGER || now - LINK.up_since < 1000 || now < BA.tx_cool)
            return;
        BA.tx_tries = 0;
    }
    else if (now - BA.tx_req_ms < BA_TX_WAIT_MS)
        return;
    else if (BA.tx_tries >= 3)
    {
        BA.tx_state = 3;
        return;
    }
    BA.tx_token = (u8)(BA.tx_token + 1);
    if (BA.tx_token == 0)
        BA.tx_token = 1;
    unsigned ssc = (LINK.seq & 0xFFF) << 4; /* the next sequence number we use starts the window */
    u8 q[9] = {3, 0, BA.tx_token, 0x02, 0x10, 0, 0, (u8)ssc, (u8)(ssc >> 8)}; /* immediate BA, traffic class 0, window 64 (bits 6..15 = 64 -> 0x1002) */
    tx_action(q, 9);
    BA.tx_state = 1;
    BA.tx_tries++;
    BA.tx_req_ms = now;
}

/* one prepared frame (802.11 header + CCMP header + LLC/SNAP + payload, no MIC, no FCS) into the best-effort queue; 0 once the chip fetched it */
static int tx_be(const u8 *frame, unsigned len, unsigned seq, int agg)
{
    if (!D.ready || !D.stage || len < 24 + 8 + 8 || len > 26 + 8 + 8 + ETH_MAX)
        return -1;
    unsigned rp = (rd32(R_TXBD_IDX_BEQ) >> 16) & 0xFFF;
    if ((LINK.be_wp + 1) % BEQ_RING_LEN == rp) /* the ring is full */
        return -1;
    volatile u8 *s = D.stage;
    u8 *d = D.stage;
    zero_bytes(d, TX_DESC_SIZE);
    for (unsigned i = 0; i < len; i++)
        s[TX_DESC_SIZE + i] = frame[i];
    put32(d + 0, len | (TX_DESC_SIZE << 16) | BIT(26));                                              /* size, offset to the frame, last segment */
    put32(d + 4, (unsigned)TX_QSEL_BE << 8 | (RA.on ? RA.rate_id : (unsigned)TX_RATEID_G) << 16 | 3u << 22); /* MAC id 0, queue, rate id (the firmware's rate table, or the fixed-rate one), security type 3 = AES */
    put32(d + 8, agg ? BIT(12) | (unsigned)((CONN.ht_ampdu >> 2) & 7) << 20 : 0); /* may be aggregated, and the least spacing the access point needs between the frames of an aggregate */
    put32(d + 12, (RA.on ? 0 : BIT(8) | BIT(10)) | (agg ? ((4u << (CONN.ht_ampdu & 3)) - 1) << 17 : 0)); /* the rate of this descriptor and no fallback, unless the firmware chooses; with an aggregate the most frames (half of that number, as Linux defines it: from the access point's longest A-MPDU) */
    put32(d + 16, RA.on ? RA.tx_rate : RATE_24M);
    put32(d + 20, RA.on ? RA.bw << 5 : 0); /* the data bandwidth: 20 / 40 / 80 MHz (bits 6..5); the primary channel was told to the MAC with the channel (DATA_SC) */
    put32(d + 36, (seq & 0xFFF) << 12);                                                              /* the sequence number we put in the header */
    u8 *bd = D.mem + Q_BE * PAGE + LINK.be_wp * 16;
    zero_bytes(bd, 16);
    put16(bd + 0, TX_DESC_SIZE);
    put16(bd + 2, (TX_DESC_SIZE + len - 1) / 128 + 1);
    put32(bd + 4, D.stage_dma);
    put16(bd + 8, len);
    put16(bd + 10, 0);
    put32(bd + 12, D.stage_dma + TX_DESC_SIZE);
    fence();
    unsigned next = (LINK.be_wp + 1) % BEQ_RING_LEN;
    wr16(R_TXBD_IDX_BEQ, (u16)next);
    LINK.be_wp = next;
    for (int i = 0; i < 50000; i++)
    {
        if (((rd32(R_TXBD_IDX_BEQ) >> 16) & 0xFFF) == next)
            return 0;
        udelay(1);
    }
    return -1;
}

/* an Ethernet frame of the network stack to the access point */
Anon_BOOT BOOT;

Anon_JOIN JOIN;

int wifi_send(const void *eth, unsigned len)
{
    if (BOOT.busy || JOIN.busy ||!LINK.up || LINK.scanning || CONN.dropped || !eth || len < 14 || len > ETH_MAX)
        return -1;
    const u8 *e = (const u8 *)eth;
    u8 f[26 + 8 + 8 + ETH_MAX];
    unsigned n = 0;
    f[n++] = RA.on ? 0x88 : 0x08; /* data (QoS data: an HT station sends those) */
    f[n++] = 0x41; /* to the distribution system, protected */
    f[n++] = 0;
    f[n++] = 0;
    for (int i = 0; i < 6; i++) /* address 1: the receiver, the access point */
        f[n++] = CONN.bssid[i];
    for (int i = 0; i < 6; i++) /* address 2: the transmitter, us */
        f[n++] = EF.mac[i];
    for (int i = 0; i < 6; i++) /* address 3: the destination of the Ethernet frame */
        f[n++] = e[i];
    unsigned seq = LINK.seq++ & 0xFFF;
    f[n++] = (u8)(seq << 4);
    f[n++] = (u8)(seq >> 4);
    if (RA.on)
    {
        f[n++] = 0; /* QoS control: traffic class 0 (best effort), normal acknowledgement, no A-MSDU */
        f[n++] = 0;
    }
    u64 pn = ++LINK.tx_pn;
    f[n++] = (u8)pn; /* the CCMP header: PN0, PN1, reserved, key id (0) with the "extended IV" bit, PN2 .. PN5 */
    f[n++] = (u8)(pn >> 8);
    f[n++] = 0;
    f[n++] = 0x20;
    f[n++] = (u8)(pn >> 16);
    f[n++] = (u8)(pn >> 24);
    f[n++] = (u8)(pn >> 32);
    f[n++] = (u8)(pn >> 40);
    for (int i = 0; i < 6; i++) /* LLC / SNAP: then the ethertype */
        f[n++] = LLC_SNAP[i];
    f[n++] = e[12];
    f[n++] = e[13];
    for (unsigned i = 14; i < len; i++)
        f[n++] = e[i];
    int agg = BA.tx_state == 2 && RA.on && !(e[12] == 0x88 && e[13] == 0x8E); /* an agreement is in use, and this is not an EAPOL frame */
    if (tx_be(f, n, seq, agg) != 0)
    {
        LINK.tx_failed++;
        return -1;
    }
    LINK.tx_frames++;
    return 0;
}

static unsigned vget_be16(const volatile u8 *p) { return (unsigned)p[0] << 8 | p[1]; }

/* a data frame from the receive path, without its FCS, with word 0 of its receive descriptor: 1 if it was taken care of (kept or dropped as
 * not for us), 0 if the link is not up and the handshake code may want it */
int wifi_rx_data(const volatile u8 *f, unsigned len, u32 w0)
{
    if (!LINK.up)
        return 0;
    unsigned fc0 = f[0], fc1 = f[1];
    unsigned subtype = fc0 >> 4;
    if ((fc0 & 0x0C) != 0x08 || (subtype != 0 && subtype != 8 && subtype != 12)) /* data or QoS data (not null frames etc.), and QoS null for the reorder buffer */
        return 1;
    if ((fc1 & 3) != 2 || !mac_is(f + 10, CONN.bssid)) /* from the access point only */
        return 1;
    int group = f[4] & 1;
    if (!group && !mac_is(f + 4, EF.mac))
        return 1;
    if (subtype == 12) /* a QoS null frame carries nothing, but it uses up a sequence number of its traffic class: inside an agreement the order must go on over it */
    {
        static const EthFrame EMPTY; /* length 0 */
        if (BA.rx_on && !group && len >= 26 && (f[24] & 0x0F) == 0)
            ba_rx_frame(vget16(f + 22) >> 4, 0, &EMPTY);
        return 1;
    }
    /* protected, and decrypted by the chip without an error (descriptor word 0: type of encryption bits 22..20, "software decrypt" bit 27, ICV error bit 15) */
    if (!(fc1 & 0x40) || ((w0 >> 15) & 1) || ((w0 >> 27) & 1) || !((w0 >> 20) & 7))
    {
        LINK.rx_dropped++;
        return 1;
    }
    unsigned hdr = 24 + ((fc0 & 0x80) ? 2 : 0) + ((fc1 & 0x80) ? 4 : 0);
    if ((fc0 & 0x80) && (f[24] & 0x80)) /* QoS control: "A-MSDU present", several packets in one frame; the access point only does that after an ADDBA exchange, which is not answered */
    {
        LINK.rx_dropped++;
        return 1;
    }
    if (len < hdr + 8 + 8 + 2)
    {
        LINK.rx_dropped++;
        return 1;
    }
    const volatile u8 *c = f + hdr; /* the CCMP header */
    if (!(c[3] & 0x20))
    {
        LINK.rx_dropped++;
        return 1;
    }
    u64 pn = (u64)c[0] | (u64)c[1] << 8 | (u64)c[4] << 16 | (u64)c[5] << 24 | (u64)c[6] << 32 | (u64)c[7] << 40;
    u64 *last = group ? &LINK.rx_pn_group[c[3] >> 6] : &LINK.rx_pn_pairwise; /* (the key id: the top two bits of the fourth byte) */
    /* a frame of traffic class 0 inside an agreement is put in order first, the packet number is checked afterwards (see the stage 10 comment) */
    int ordered = BA.rx_on && !group && (fc0 & 0x80) && (f[24] & 0x0F) == 0;
    if (!ordered && pn <= *last)
    {
        LINK.rx_replay++;
        return 1;
    }
    const volatile u8 *p = f + hdr + 8; /* LLC / SNAP */
    for (int i = 0; i < 6; i++)
        if (p[i] != LLC_SNAP[i])
        {
            LINK.rx_dropped++;
            return 1;
        }
    unsigned eth_type = vget_be16(p + 6);
    if (eth_type == 0x888E) /* EAPOL (a group key renewal): not for the network stack, link_service() answers it. The 802.1X header knows its length. */
    {
        unsigned n = len - hdr - 8 - 8;
        unsigned body = n >= 4 ? vget_be16(p + 8 + 2) : 0;
        if (n >= 4 && 4 + body <= n)
            n = 4 + body;
        if (group || EAPQ.pending || n > EAPOL_MAX)
        {
            LINK.rx_dropped++;
            return 1;
        }
        if (!ordered) /* (inside an agreement the packet numbers are not in step with the arrival; the EAPOL replay counter protects these) */
            *last = pn;
        for (unsigned i = 0; i < n; i++)
            EAPQ.b[i] = p[8 + i];
        EAPQ.len = n;
        EAPQ.pending = 1;
        return 1;
    }
    unsigned avail = len - hdr - 8 - 8; /* the payload, and the 8 byte MIC after it when the chip leaves it */
    unsigned plen = avail;
    if (eth_type == 0x0800 && avail >= 20) /* IPv4: the header knows how long the packet is */
    {
        unsigned total = vget_be16(p + 8 + 2);
        if (total >= 20 && total <= avail)
            plen = total;
    }
    else if (eth_type == 0x0806 && avail >= 28) /* ARP: 28 bytes */
        plen = 28;
    else if (avail >= 8)
        plen = avail - 8; /* some other protocol: take the MIC off */
    if (plen > ETH_MAX - 14)
        plen = ETH_MAX - 14;
    if (mac_is(f + 16, EF.mac)) /* our own broadcast, sent back by the access point */
    {
        LINK.rx_dropped++;
        return 1;
    }
    static EthFrame e; /* (one frame at a time: no need to have it on the stack) */
    for (int i = 0; i < 6; i++)
    {
        e.b[i] = f[4 + i];       /* destination: address 1 */
        e.b[6 + i] = f[16 + i];  /* source: address 3 */
    }
    e.b[12] = p[6];
    e.b[13] = p[7];
    for (unsigned i = 0; i < plen; i++)
        e.b[14 + i] = p[8 + i];
    e.len = 14 + plen;
    if (ordered)
        ba_rx_frame(vget16(f + 22) >> 4, pn, &e); /* the sequence number is the upper 12 bits of the sequence control field */
    else
        rx_deliver(&e, pn, last);
    return 1;
}

static const unsigned char NO_MAC[6];
const unsigned char *rtl8821ce_mac_up(void) { return LINK.adapter ? EF.mac : NO_MAC; }
int link_is_up(void) { return LINK.up && !CONN.dropped; }

/* the handshake worked and the keys are in the chip: from now on the card carries the traffic of the network stack */
void link_try(void)
{
    if (!ONLINE_ENABLE || !HS.state || HS.result != HS_OK || !HS.keys)
        return;
    LINK.tx_pn = LINK.rx_pn_pairwise = 0; /* new keys: both sides count packet numbers from the start */
    for (int i = 0; i < 4; i++)
        LINK.rx_pn_group[i] = 0;
    LINK.seq = 0;
    LINK.head = LINK.tail = 0;
    EAPQ.pending = 0;
    if (!LINK.adapter) /* the first connection; a later one goes on counting, and the transmit ring keeps its place (the chip's read index is where it is) */
    {
        LINK.tx_frames = LINK.tx_failed = LINK.rx_frames = LINK.rx_dropped = LINK.rx_replay = 0;
        LINK.be_wp = 0;
        LINK.reconnects = LINK.rekeys = LINK.eapol_bad = LINK.fails = 0;
    }
    LINK.last_rx = LINK.last_probe = LINK.up_since = now_ms();
    ba_reset();
    LINK.adapter = 1;
    LINK.want = 1;
    LINK.up = 1;
}
