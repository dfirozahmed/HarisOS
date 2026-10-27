/* RTL8821CE: the first frame on the air (stage 5a-2). */
#include "rtl8821ce_internal.h"

/* ---- stage 5a-2: the first frame on the air ----
 * A management frame goes to the chip through the management queue's ring: one buffer descriptor (16 bytes) = two entries, the first
 * pointing at the 48 byte transmit descriptor (what to do with the frame: length, queue, rate, ...), the second at the frame itself.
 * The host writes the index of the next free entry to 0x3B0; the chip reads the entry, fetches both pieces by DMA, sends the frame and
 * moves its own read index (the upper half of the same register). The frame is given without its 4 byte FCS: the chip adds it. The
 * sequence number is filled in by the hardware (EN_HWSEQ), and the rate is fixed (no rate adaptation: USE_RATE, no fallback).
 * Used here for a probe request: "is there a network around? answer me" sent to everyone (broadcast), which every phone and laptop does
 * when it looks for networks. The access points answer with probe responses, which only reach us if the chip knows its own address
 * (written to 0x610), and which the scan then lists like beacons. */
#define R_MACID 0x0610
#define R_TXBD_IDX_MGMTQ 0x03B0
#define TX_QSEL_MGMT 18
#define TX_RATEID_B_20M 8
#define RATE_6M 4
#define RATE_1M 0
#define MGMT_RING_LEN 128

/* the host tests of the earlier stages switch the probe-request test off (it resets the receive counters they look at) */
int TXP_ENABLE = 1;

Anon_TXP TXP;

/* one frame (without FCS, at most 1500 bytes) into the management queue and wait until the chip has fetched it: 0, or -1 */
int tx_mgmt(const u8 *frame, unsigned len, unsigned channel)
{
    if (!D.ready || !D.stage || len < 24 || len > 1500)
        return -1;
    unsigned rp = (rd32(R_TXBD_IDX_MGMTQ) >> 16) & 0xFFF;
    if ((TXP.wp + 1) % MGMT_RING_LEN == rp) /* the ring is full */
        return -1;
    volatile u8 *s = D.stage;
    u8 *d = D.stage;
    zero_bytes(d, TX_DESC_SIZE);
    for (unsigned i = 0; i < len; i++)
        s[TX_DESC_SIZE + i] = frame[i];
    unsigned bmc = frame[4] & 1; /* the destination address is a group address */
    put32(d + 0, len | (TX_DESC_SIZE << 16) | (bmc << 24) | BIT(26) | BIT(31)); /* size, offset to the frame, bmc, last segment, no queue/sequence select */
    put32(d + 4, (unsigned)TX_QSEL_MGMT << 8 | (unsigned)(channel <= 14 ? TX_RATEID_B_20M : TX_RATEID_G) << 16); /* MAC id 0, queue, rate id */
    put32(d + 12, BIT(8) | BIT(10)); /* use the rate of this descriptor, no rate fallback */
    put32(d + 16, channel <= 14 ? RATE_1M : RATE_6M);
    put32(d + 32, BIT(15)); /* hardware sequence number */

    u8 *bd = D.mem + Q_MGMT * PAGE + TXP.wp * 16;
    zero_bytes(bd, 16);
    put16(bd + 0, TX_DESC_SIZE);
    put16(bd + 2, (TX_DESC_SIZE + len - 1) / 128 + 1); /* the length in 128 byte units, no OWN bit */
    put32(bd + 4, D.stage_dma);
    put16(bd + 8, len);
    put16(bd + 10, 0);
    put32(bd + 12, D.stage_dma + TX_DESC_SIZE);
    fence();
    unsigned next = (TXP.wp + 1) % MGMT_RING_LEN;
    if (TXP.kick32)
        wr32(R_TXBD_IDX_MGMTQ, next);
    else
        wr16(R_TXBD_IDX_MGMTQ, (u16)next);
    TXP.wp = next;
    for (int i = 0; i < 50000; i++) /* up to 50 ms for the chip to fetch it */
    {
        if (((rd32(R_TXBD_IDX_MGMTQ) >> 16) & 0xFFF) == next)
            return 0;
        udelay(1);
    }
    return -1;
}

/* a probe request for any network (empty SSID), asking with the basic rates of the band; returns its length */
unsigned build_probe_request(u8 *f, const u8 *mac, unsigned channel)
{
    unsigned n = 0;
    f[n++] = 0x40; /* frame control: management, probe request */
    f[n++] = 0x00;
    f[n++] = 0;    /* duration */
    f[n++] = 0;
    for (int i = 0; i < 6; i++) /* addr1: everybody */
        f[n++] = 0xFF;
    for (int i = 0; i < 6; i++) /* addr2: us */
        f[n++] = mac[i];
    for (int i = 0; i < 6; i++) /* addr3: any BSSID */
        f[n++] = 0xFF;
    f[n++] = 0; /* sequence control: filled in by the hardware */
    f[n++] = 0;
    f[n++] = 0; /* SSID element: empty = any network */
    f[n++] = 0;
    f[n++] = 1; /* supported rates */
    if (channel <= 14)
    {
        static const u8 R2[8] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24}; /* 1, 2, 5.5, 11 (basic), 6, 9, 12, 18 Mb/s */
        f[n++] = 8;
        for (int i = 0; i < 8; i++)
            f[n++] = R2[i];
        f[n++] = 50; /* extended rates: 24, 36, 48, 54 */
        f[n++] = 4;
        f[n++] = 0x30;
        f[n++] = 0x48;
        f[n++] = 0x60;
        f[n++] = 0x6C;
    }
    else
    {
        static const u8 R5[8] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C}; /* 6 (basic), 9, 12 (basic), 18, 24 (basic), 36, 48, 54 Mb/s */
        f[n++] = 8;
        for (int i = 0; i < 8; i++)
            f[n++] = R5[i];
    }
    return n;
}

/* tune to a channel (of 20, 40 or 80 MHz) with its transmit power, and make the chip answer to our address */
void tune_chan(const Chan *c)
{
    set_channel_bw(c->center, c->bw, c->sc, PH.ch_param, EF.swing2g, EF.swing5g);
    tx_power_apply_c(EF.txpwr, c);
    for (int i = 0; i < 6; i++)
        wr8(R_MACID + (u32)i, EF.mac[i]);
}

static void tune_for_tx(unsigned channel)
{
    Chan c = {channel, channel, 0, 0};
    tune_chan(&c);
}

/* The first real card did not take a frame from the ring with the plain start (Linux' own sequence as far as it was ported). So the
 * test tries, one frame at a time, a few ways of starting the transmit DMA, each one adding to the last, and remembers the first that
 * worked. 0: the plain start. 1: the DMA interface of the PCI block is reset once more (bit 20 of the PCI control register off and on).
 * 2: the transmit pause register and the "drop / sleep" registers of the MAC ids are cleared. 3: all eight enable bits of the top
 * byte of the PCI control register (Linux sets 0xF7). 4: the ring index is written as a 32-bit register. */
#define TX_VARIANTS 5

void tx_variant_setup(int v)
{
    wr16(0x0380, MGMT_RING_LEN); /* the ring, as hci_setup() wrote it */
    wr32(0x0310, D.dma + Q_MGMT * PAGE);
    wr32(R_TXBD_RWPTR_CLR, 0xFFFFFFFFu);
    TXP.wp = 0;
    TXP.kick32 = 0;
    if (v >= 1)
    {
        u32 p = rd32(R_PCI_CTRL);
        wr32(R_PCI_CTRL, p & ~BIT(20));
        udelay(100);
        wr32(R_PCI_CTRL, p | BIT(20));
        udelay(100);
    }
    if (v >= 2)
    {
        wr16(R_TXPAUSE, 0);
        wr32(0x04D0, 0); /* REG_MACID_PKT_DROP0 */
        wr32(0x04D4, 0); /* REG_MACID_PKT_SLEEP */
    }
    if (v >= 3)
        wr8(R_PCI_CTRL + 3, (u8)(rd8(R_PCI_CTRL + 3) | 0xFF));
    if (v >= 4)
        TXP.kick32 = 1;
}

/* listen for `ms` milliseconds, taking in what arrives */
void listen_ms(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++)
    {
        udelay(1000);
        rx_poll();
    }
}

/* ask for probe responses three times on a channel, listening 100 ms after each, and count the answers. A way of starting the transmit
 * DMA counts as working if the chip says it fetched the frame (its read index moved) OR an access point answered: on the first real
 * card the index never moved, and it was not known whether the frame went out all the same. */
void tx_probe_test(unsigned channel)
{
    TXP.state = 1;
    TXP.channel = channel;
    TXP.sent = TXP.failed = 0;
    TXP.variant = -1;
    TXP.kick32 = 0;
    TXP.exq_before = rd16(0x023E); /* REG_FIFOPAGE_INFO_4 + 2: free pages of the extra queue */
    TXP.pub_before = rd16(0x0242); /* REG_FIFOPAGE_INFO_5 + 2: free pages of the public pool */
    if (!D.ready || EF.state != EF_OK)
        return;
    tune_for_tx(channel);
    rx_restart();
    wr32(R_RCR, RCR_RX_DEFAULT);
    wr8(R_CR, MAC_TRX_ENABLE);
    u8 frame[96];
    unsigned len = build_probe_request(frame, EF.mac, channel);
    for (int v = 0; v < TX_VARIANTS && TXP.variant < 0; v++) /* the first frame decides how the DMA is started */
    {
        tx_variant_setup(v);
        unsigned answers = RX.probe_resp;
        int fetched = tx_mgmt(frame, len, channel) == 0;
        listen_ms(100);
        if (fetched || RX.probe_resp > answers)
        {
            TXP.variant = v;
            TXP.sent++;
        }
    }
    if (TXP.variant < 0)
    {
        TXP.failed = 3; /* no way worked: do not try more */
        TXP.exq_after = rd16(0x023E);
        TXP.pub_after = rd16(0x0242);
    }
    else
        for (int k = 1; k < 3; k++)
        {
            if (tx_mgmt(frame, len, channel) == 0)
                TXP.sent++;
            else
                TXP.failed++;
            listen_ms(100);
        }
    TXP.replies = RX.probe_resp;
}
