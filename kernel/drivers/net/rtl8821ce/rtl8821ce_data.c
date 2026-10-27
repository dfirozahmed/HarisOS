/* RTL8821CE: data frames, aggregation (A-MPDU) and the block acknowledgement (stages 7, 10). */
#include "rtl8821ce_internal.h"

/* ---- stage 7: data ----
 * With the keys in the chip the card is a network adapter: Ethernet frames of the network stack go out as encrypted 802.11 data frames and
 * come in again. The conversion, in short:
 *   out:  802.11 header (data, to the access point: address 1 = the BSSID, 2 = us, 3 = the real destination), then the 8 byte CCMP header (the packet
 *         number, which the receiver uses against replays, and the key id), then LLC/SNAP (AA AA 03 00 00 00 + the ethertype), then the payload.
 *         The chip does the AES: the descriptor says "security type 3" and it encrypts everything after the CCMP header and appends the 8 byte MIC.
 *   in:   the chip decrypts and checks the MIC, and leaves the frame as it was: header, CCMP header, plain LLC/SNAP + payload, MIC, FCS.
 *         Address 1 is the destination, address 3 the source (the frame comes from the access point). The Ethernet frame is destination, source,
 *         ethertype, payload. Only frames the chip says it decrypted are taken (an unprotected frame could come from anybody), from our access
 *         point, to us or to a group address, with a packet number larger than the last one seen (the replay check the chip does not do). */

Anon_LINK LINK;


int ONLINE_ENABLE = 1; /* the host tests of the earlier stages switch it off (a link that stays up changes what they look at) */


/* ---- stage 10: aggregation (A-MPDU) and the block acknowledgement ----
 * At high rates the time on the air goes mostly into the preamble and the waiting between frames, not into the data. Aggregation sends many
 * frames in one transmission (an A-MPDU) and acknowledges them together with one "block acknowledgement" (BA). The two sides agree first, per
 * traffic class (TID, here only 0, the normal one), with an "ADDBA" exchange of action frames: request (starting sequence number, how many frames
 * the receiver can hold = the window), response (yes / no, its window). The chip does the rest in hardware: it sends the BA for what it received
 * (the receive side), and for what we send it builds the aggregates from the frames that wait in its queue, collects the BA, repeats what was
 * lost (the transmit side; the descriptor of a frame says "may be aggregated", with how many and how densely, taken from the access point's HT
 * capabilities).
 * What the host has to do is the ORDER. A frame of an aggregate that was lost arrives again later, after the frames that followed it: the
 * receiver must hold those back until the gap is filled, or the access point gives up on it (it moves its window on) or 100 ms have passed, and then
 * give them to the network stack in the order of their sequence numbers (12 bits, counting up, wrapping): the "reorder buffer" below. The packet
 * number check of the encryption (a number must be larger than the last one) has to come after that, on the frames in order; a retransmitted frame
 * keeps its packet number, so in the order of the sequence numbers the packet numbers rise. Frames that are no part of an agreement (other traffic
 * classes, broadcasts) go straight through as before. */
#define BA_TIMEOUT_MS 100   /* a gap that is not filled for so long is given up (as mac80211 does) */


Anon_BA BA;

static unsigned sn_diff(unsigned a, unsigned b) { return (a - b) & 0xFFF; } /* 12 bit sequence numbers: how far a is ahead of b */

/* one frame, in its turn, to the network stack: the packet number must be larger than the last one (`last`); a frame of length 0 (a QoS null
 * frame, which only uses up a sequence number) is not a packet */
void rx_deliver(const EthFrame *e, u64 pn, u64 *last)
{
    if (e->len == 0)
        return;
    if (pn <= *last)
    {
        LINK.rx_replay++;
        return;
    }
    if ((LINK.head + 1) % RXE_SLOTS == LINK.tail)
    {
        LINK.rx_dropped++; /* the queue is full: the network stack is behind */
        return;
    }
    *last = pn;
    EthFrame *q = &LINK.q[LINK.head];
    for (unsigned i = 0; i < e->len; i++)
        q->b[i] = e->b[i];
    q->len = e->len;
    LINK.head = (LINK.head + 1) % RXE_SLOTS;
    LINK.rx_frames++;
}

static void ba_release(BaSlot *s)
{
    rx_deliver(&s->f, s->pn, &LINK.rx_pn_pairwise);
    s->used = 0;
    BA.buffered--;
}

/* hand over the frames that follow the head without a gap */
static void ba_flush_ready(void)
{
    while (BA.slot[BA.rx_head % BA_WIN].used)
    {
        ba_release(&BA.slot[BA.rx_head % BA_WIN]);
        BA.rx_head = (BA.rx_head + 1) & 0xFFF;
    }
}

/* give up waiting for the frames before new_head: what is buffered there goes out in order, the head moves */
static void ba_skip_to(unsigned new_head)
{
    while (BA.rx_head != new_head)
    {
        BaSlot *s = &BA.slot[BA.rx_head % BA_WIN];
        if (s->used)
            ba_release(s);
        BA.rx_head = (BA.rx_head + 1) & 0xFFF;
    }
}

/* a frame of the agreement (sequence number, packet number, the Ethernet frame) */
void ba_rx_frame(unsigned seq, u64 pn, const EthFrame *e)
{
    unsigned d = sn_diff(seq, BA.rx_head);
    if (d >= 0x800) /* behind the head: a copy of what was delivered already, or too late */
    {
        BA.rx_late++;
        return;
    }
    if (d >= BA_WIN) /* beyond the window: the access point has moved on, so the window moves with it */
    {
        ba_skip_to((seq - (BA_WIN - 1)) & 0xFFF);
        ba_flush_ready();
        d = sn_diff(seq, BA.rx_head);
    }
    if (d == 0)
    {
        rx_deliver(e, pn, &LINK.rx_pn_pairwise);
        BA.rx_head = (BA.rx_head + 1) & 0xFFF;
        ba_flush_ready();
        return;
    }
    BaSlot *s = &BA.slot[seq % BA_WIN];
    if (s->used)
    {
        BA.rx_dups++;
        return;
    }
    s->used = 1;
    s->pn = pn;
    s->t = now_ms();
    for (unsigned i = 0; i < e->len; i++)
        s->f.b[i] = e->b[i];
    s->f.len = e->len;
    BA.buffered++;
}

/* a gap that has been open for BA_TIMEOUT_MS is given up: the frames behind it go out */
void ba_expire(u64 now)
{
    while (BA.buffered)
    {
        unsigned i = 0;
        while (i < BA_WIN && !BA.slot[(BA.rx_head + i) % BA_WIN].used)
            i++;
        if (i >= BA_WIN || now - BA.slot[(BA.rx_head + i) % BA_WIN].t < BA_TIMEOUT_MS)
            return;
        ba_skip_to((BA.rx_head + i) & 0xFFF);
        ba_flush_ready();
        BA.rx_timeouts++;
    }
}

/* the agreement for the receive side starts (or starts again) at sequence number ssn: what is buffered of an earlier one goes out first */
void ba_rx_start(unsigned ssn)
{
    if (BA.rx_on)
        ba_skip_to((BA.rx_head + BA_WIN) & 0xFFF);
    BA.rx_head = ssn & 0xFFF;
    BA.rx_on = 1;
    BA.rx_sessions++;
}

void ba_rx_stop(void)
{
    if (BA.rx_on)
        ba_skip_to((BA.rx_head + BA_WIN) & 0xFFF);
    BA.rx_on = 0;
}

/* the link is gone: nothing of the agreements is left (the access point forgets them as well) */
void ba_reset(void)
{
    for (unsigned i = 0; i < BA_WIN; i++)
        BA.slot[i].used = 0;
    BA.buffered = 0;
    BA.rx_on = 0;
    BA.tx_state = 0;
    BA.tx_tries = 0;
    BA.tx_cool = 0;
}
