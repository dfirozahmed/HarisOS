/* RTL8821CE: receive (stage 4c-2). */
#include "rtl8821ce_internal.h"

/* ---- stage 4c-2: receive ----
 * The chip writes every frame it receives into the next buffer of the receive ring (the 32 descriptors set up in dma_setup(), each
 * pointing at its own 12 KiB buffer) and moves its write index, which the host can read at 0x3B4 (bits 27..16). The host reads the
 * frames from the buffers and gives the index it has reached back (bits 15..0), so that the chip may reuse the buffers behind it.
 * Each buffer holds a 24 byte receive descriptor, then 32 bytes of PHY status (signal strength etc., the "driver info" of
 * rtw_drv_info_cfg), then the 802.11 frame including its 4 byte FCS. Descriptor word 0: bits 13..0 frame length, 19..16 driver info
 * size in units of 8 bytes, 25..24 extra shift; word 2 bit 28 marks a message from the firmware (C2H) instead of a received frame.
 * What this stage does: switch the receiver on with Linux' default filter and count what arrives for a while, without answering
 * anything. The receive filter accepts beacons of every network (the "associated BSSID" check is off), so on a busy channel the beacons
 * of all access points around are received. */
#define R_RXBD_IDX_MPDUQ 0x03B4
#define RX_DESC_SIZE 24

Anon_RX RX;

static u32 vle32(const volatile u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static u32 rx_buf_dma(unsigned i) { return D.dma + (u32)(D.mem + Q_COUNT * PAGE + PAGE - D.mem) + i * RX_BUF_SLOT; }

void scan_note(const volatile u8 *f, unsigned len);
void act_note(const volatile u8 *f, unsigned len);
void conn_note(const volatile u8 *f, unsigned len);
void eapol_note(const volatile u8 *f, unsigned len);
int wifi_rx_data(const volatile u8 *f, unsigned len, u32 w0);
void link_try(void);
int link_is_up(void);
void alive_note(const volatile u8 *f, unsigned len);

/* one received buffer: classify it. `b` is the buffer, as the chip filled it. */
static void rx_one(const volatile u8 *b)
{
    u32 w0 = vle32(b), w2 = vle32(b + 8);
    unsigned len = w0 & 0x3FFF, drv = ((w0 >> 16) & 0xF) * 8, shift = (w0 >> 24) & 3;
    int crc_err = (w0 >> 14) & 1;
    if (w2 & BIT(28)) /* C2H */
    {
        RX.c2h++;
        return;
    }
    if (len <= 4 || drv > 32 || RX_DESC_SIZE + drv + shift + len > RX_BUF_SIZE || crc_err)
    {
        RX.bad++;
        return;
    }
    const volatile u8 *f = b + RX_DESC_SIZE + drv + shift;
    unsigned fc0 = f[0], type = (fc0 >> 2) & 3, subtype = fc0 >> 4;
    RX.frames++;
    if (type != 1)
        alive_note(f, len - 4);
    if (type == 0)
    {
        if (subtype == 8)
            RX.beacons++;
        else if (subtype == 5)
            RX.probe_resp++;
        else
            RX.mgmt_other++;
        if (subtype == 11 || subtype == 1 || subtype == 12 || subtype == 10) /* authentication, association response, deauthentication, disassociation */
            conn_note(f, len - 4);
        if (subtype == 13) /* action */
            act_note(f, len - 4);
        if (subtype == 8 || subtype == 5)
            scan_note(f, len - 4); /* the frame without its FCS */
    }
    else if (type == 1)
        RX.ctrl++;
    else
    {
        RX.data++;
        if (f[1] & 0x40) /* the protected bit: the chip tried to decrypt (descriptor word 0: enc type bits 22..20, "software decrypt" bit 27, ICV error bit 15) */
        {
            if ((w0 >> 15) & 1)
                RX.icv_err++;
            else if (!((w0 >> 27) & 1) && ((w0 >> 20) & 7))
                RX.decrypted++;
        }
        if (type == 2 && !wifi_rx_data(f, len - 4, w0)) /* (once the link is up, the data path takes it) */
            eapol_note(f, len - 4); /* (only kept while the handshake runs) */
    }
}

/* take every buffer the chip has finished, re-arm it, tell the chip how far we are */
/* start counting afresh: the buffers the chip fills next are the ones after the last it filled (an earlier listening left its index where it was) */
void rx_restart(void)
{
    RX.rp = (rd32(R_RXBD_IDX_MPDUQ) >> 16) & 0xFFF;
    if (RX.rp >= RX_COUNT)
        RX.rp = 0;
    RX.frames = RX.beacons = RX.probe_resp = RX.mgmt_other = RX.ctrl = RX.data = RX.c2h = RX.bad = RX.decrypted = RX.icv_err = 0;
}

void rx_poll(void)
{
    unsigned hw = (rd32(R_RXBD_IDX_MPDUQ) >> 16) & 0xFFF;
    if (hw >= RX_COUNT)
        return; /* nonsense from the register: do nothing */
    unsigned n = hw >= RX.rp ? hw - RX.rp : RX_COUNT - (RX.rp - hw);
    if (!n)
        return;
    u8 *ring = D.mem + Q_COUNT * PAGE;
    u8 *bufs = ring + PAGE;
    while (n--)
    {
        rx_one(bufs + RX.rp * RX_BUF_SLOT);
        u8 *bd = ring + RX.rp * 8; /* give the buffer back: size, packet size 0, address */
        put16(bd, RX_BUF_SIZE);
        put16(bd + 2, 0);
        put32(bd + 4, rx_buf_dma(RX.rp));
        if (++RX.rp >= RX_COUNT)
            RX.rp = 0;
    }
    wr16(R_RXBD_IDX_MPDUQ, (u16)RX.rp);
}
