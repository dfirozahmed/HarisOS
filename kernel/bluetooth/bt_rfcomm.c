/* RFCOMM, the serial-cable emulation that carries OBEX file transfer, hands-free and the serial port profile.
 *
 * It rides on one L2CAP channel (PSM 3). On it, frames: address (1) control (1) length (1 or 2) [data] check (1).
 *   address  = DLCI << 2 | C/R << 1 | 1      DLCI 0 is the multiplexer's own control channel, DLCI = server channel << 1 is a data channel
 *   control  = SABM 0x2F (open), UA 0x63 (ok), DM 0x0F (no), DISC 0x43 (close), UIH 0xEF (data); bit 0x10 is the poll/final bit
 *   length   = bytes << 1 | 1 (up to 127 bytes), else two bytes: low 7 bits << 1, then the rest
 *   check    = 0xFF - CRC-8 (polynomial x^8+x^2+x+1, reflected, start 0xFF) over address+control(+length unless UIH); a receiver runs the CRC over
 *              everything including the check byte and must get 0xCF.
 * The side that opened the L2CAP channel is the "initiator": its commands carry C/R = 1 and its answers C/R = 0.
 *
 * Opening: SABM on DLCI 0 -> UA, then SABM on the data DLCI -> UA, then each side sends an MSC (modem status: "ready to send / receive") on DLCI 0
 * and answers the other's. Control messages (multiplexer commands) are UIH frames on DLCI 0: type (MSC 0x38, PN 0x20, TEST 0x08, FCon 0x28,
 * FCoff 0x18; type << 2 | C/R << 1 | 1), length (value bytes << 1 | 1), value. We do not send PN (parameter negotiation): the defaults need no
 * credits, 127 byte frames. If the device sends a PN anyway, we answer it with "no credit flow control".
 *
 * Everything is polled: bt_rfcomm_step() runs the time-outs and the opening; answers made inside the receive handler go through bt_chan_queue (small
 * PDUs only); data goes out with bt_chan_send from bt_rfcomm_send(), which must not be called from a handler. */
#include "bluetooth.h"

#define RF_PSM 3
#define F_SABM 0x2F
#define F_UA 0x63
#define F_DM 0x0F
#define F_DISC 0x43
#define F_UIH 0xEF
#define F_PF 0x10
#define MCC_PN 0x20
#define MCC_MSC 0x38
#define MCC_TEST 0x08
#define MCC_FCON 0x28
#define MCC_FCOFF 0x18
#define MCC_NSC 0x04
#define STEP_LIMIT_MS 10000
#define CLOSE_LIMIT_MS 3000
#define SERVE_WAIT_MS 30000

enum { S_IDLE, S_L2CAP, S_MUX, S_DLC, S_OPEN, S_CLOSING, S_CLOSED, S_FAILED, S_SRV, S_SRV_MUX }; /* S_SRV: a device opened the L2CAP channel, waiting for its SABM; S_SRV_MUX: the multiplexer is up, waiting for the data channel */

static struct
{
    int state;
    int slot;
    unsigned dlci;
    const char *why;
    int code;
    unsigned long long t0;
    int initiator;      /* 1 = we opened the channel (commands carry C/R = 1), 0 = the device did */
    int tx_paused;      /* the device said FCoff: do not send data */
    int want_close_l2;  /* close the L2CAP channel from the next step (not from inside a handler) */
    int sabm_dlc;       /* send the data channel's SABM from the next step (set by the UA of the multiplexer) */
    int send_msc;       /* send our MSC from the next step */
    void (*rx)(const unsigned char *d, unsigned n);
    unsigned frames_rx, frames_tx, bad_frames;
} R;

unsigned char bt_rfcomm_fcs(const unsigned char *p, unsigned n)
{
    unsigned c = 0xFF;
    for (unsigned i = 0; i < n; i++)
    {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xE0 : c >> 1;
    }
    return (unsigned char)(0xFF - c);
}

static int is_uih(unsigned ctrl) { return (ctrl & ~(unsigned)F_PF) == F_UIH; }

unsigned bt_rfcomm_build(unsigned char *out, unsigned cap, unsigned addr, unsigned ctrl, const unsigned char *data, unsigned n)
{
    unsigned h = 0;
    if (n > 0x7FFF || cap < n + 5)
        return 0;
    out[h++] = (unsigned char)addr;
    out[h++] = (unsigned char)ctrl;
    if (n < 128)
        out[h++] = (unsigned char)(n << 1 | 1);
    else
    {
        out[h++] = (unsigned char)((n & 0x7F) << 1);
        out[h++] = (unsigned char)(n >> 7);
    }
    for (unsigned i = 0; i < n; i++)
        out[h + i] = data[i];
    out[h + n] = bt_rfcomm_fcs(out, is_uih(ctrl) ? 2 : h); /* UIH: address and control only; the others: also the length */
    return h + n + 1;
}

int bt_rfcomm_parse(const unsigned char *p, unsigned n, BtRfFrame *f)
{
    if (n < 4)
        return -1;
    unsigned h = (p[2] & 1) ? 3 : 4;
    if (h == 4 && n < 5)
        return -1;
    unsigned len = (p[2] & 1) ? (unsigned)(p[2] >> 1) : ((unsigned)(p[2] >> 1) | ((unsigned)p[3] << 7));
    if (h + len + 1 > n)
        return -1;
    unsigned char fcs = bt_rfcomm_fcs(p, is_uih(p[1]) ? 2 : h);
    if (fcs != p[h + len])
        return -1;
    f->addr = p[0];
    f->ctrl = p[1];
    f->dlci = p[0] >> 2;
    f->cr = (p[0] >> 1) & 1;
    f->data = p + h;
    f->len = len;
    return 0;
}

/* ---- sending ---- */
static unsigned addr_cmd(unsigned dlci) { return dlci << 2 | (R.initiator ? 3 : 1); } /* the initiator's commands and UIH frames: C/R = 1; the other side's: 0 */
static unsigned addr_rsp(unsigned dlci) { return dlci << 2 | (R.initiator ? 1 : 3); } /* answers: the opposite */

/* queue a control frame (small); safe inside handlers */
static void ctl(unsigned addr, unsigned ctrl, const unsigned char *data, unsigned n)
{
    unsigned char f[60];
    unsigned len = bt_rfcomm_build(f, sizeof f, addr, ctrl, data, n);
    if (len && bt_chan_queue(R.slot, f, len) == 0)
        R.frames_tx++;
}

/* a multiplexer control message (UIH on DLCI 0): type byte, length byte, value */
static void mcc(unsigned type, int command, const unsigned char *val, unsigned n)
{
    unsigned char m[40];
    if (n > sizeof m - 2)
        return;
    m[0] = (unsigned char)(type << 2 | (command ? 2 : 0) | 1);
    m[1] = (unsigned char)(n << 1 | 1);
    for (unsigned i = 0; i < n; i++)
        m[2 + i] = val[i];
    ctl(addr_cmd(0), F_UIH, m, 2 + n);
}

static void send_msc(void)
{
    unsigned char v[2];
    v[0] = (unsigned char)(R.dlci << 2 | 3); /* the DLCI the signals are for, in address form (EA and C/R always set) */
    v[1] = 0x8D;                             /* EA, RTC (ready to communicate), RTR (ready to receive), DV (data valid) */
    mcc(MCC_MSC, 1, v, 2);
}

static void fail(const char *why, int code)
{
    R.state = S_FAILED;
    R.why = why;
    R.code = code;
    if (R.slot >= 0)
        bt_chan_close(R.slot);
}

/* ---- receiving ---- */
static void on_mcc(const unsigned char *d, unsigned n)
{
    while (n >= 2)
    {
        unsigned t = d[0], type = t >> 2, len = d[1] >> 1;
        int command = (t & 2) != 0;
        if (!(d[1] & 1) || 2 + len > n) /* lengths over 127 do not occur in control messages */
            return;
        unsigned char v[40];
        if (len > sizeof v)
            return;
        for (unsigned i = 0; i < len; i++)
            v[i] = d[2 + i];
        if (command)
        {
            switch (type)
            {
            case MCC_PN:
                if (len == 8)
                {
                    v[1] = 0; /* no credit based flow control */
                    v[7] = 0;
                    if ((unsigned)v[4] + ((unsigned)v[5] << 8) > BT_RF_MAX_N1) /* a frame must fit one L2CAP PDU */
                    {
                        v[4] = (unsigned char)(BT_RF_MAX_N1 & 0xFF);
                        v[5] = (unsigned char)(BT_RF_MAX_N1 >> 8);
                    }
                }
                mcc(type, 0, v, len);
                break;
            case MCC_MSC:
            case MCC_TEST:
                mcc(type, 0, v, len);
                break;
            case MCC_FCON:
                R.tx_paused = 0;
                mcc(type, 0, v, 0);
                break;
            case MCC_FCOFF:
                R.tx_paused = 1;
                mcc(type, 0, v, 0);
                break;
            default: /* not understood: Non Supported Command Response carries the type we got */
            {
                unsigned char x = (unsigned char)t;
                mcc(MCC_NSC, 0, &x, 1);
            }
            }
        }
        d += 2 + len;
        n -= 2 + len;
    }
}

void bt_rfcomm_rx(const unsigned char *d, unsigned n)
{
    BtRfFrame f;
    if (R.state == S_IDLE || R.state == S_FAILED || R.state == S_CLOSED)
        return;
    if (bt_rfcomm_parse(d, n, &f) != 0)
    {
        R.bad_frames++;
        return;
    }
    R.frames_rx++;
    unsigned type = f.ctrl & ~(unsigned)F_PF;
    switch (type)
    {
    case F_UA:
        if (R.state == S_MUX && f.dlci == 0)
        {
            R.sabm_dlc = 1; /* the multiplexer is up: open the data channel (from the step) */
            R.state = S_DLC;
            R.t0 = bt_now();
        }
        else if (R.state == S_DLC && f.dlci == R.dlci)
        {
            R.state = S_OPEN;
            R.send_msc = 1;
        }
        else if (R.state == S_CLOSING && f.dlci == R.dlci) /* the data channel is closed: now the multiplexer */
        {
            ctl(addr_cmd(0), F_DISC | F_PF, 0, 0);
            R.t0 = bt_now();
        }
        else if (R.state == S_CLOSING && f.dlci == 0)
        {
            R.state = S_CLOSED;
            R.want_close_l2 = 1;
        }
        break;
    case F_DM: /* refused (or "not open") */
        if (R.state == S_MUX || R.state == S_DLC)
            fail(R.state == S_MUX ? "the device refused the multiplexer" : "the device refused the channel", (int)f.dlci);
        else if (R.state == S_OPEN && f.dlci == R.dlci)
        {
            R.state = S_CLOSED;
            R.want_close_l2 = 1;
        }
        break;
    case F_DISC: /* the device closes */
        ctl(addr_rsp(f.dlci), F_UA | F_PF, 0, 0);
        if (!R.initiator && f.dlci != 0 && f.dlci == R.dlci && R.state == S_OPEN)
            R.state = S_SRV_MUX; /* only the data channel: the device may open it again on the same multiplexer */
        else if (f.dlci == 0 || f.dlci == R.dlci)
        {
            if (R.state != S_FAILED)
                R.state = S_CLOSED;
            R.want_close_l2 = 1;
        }
        break;
    case F_SABM: /* a device opens the multiplexer and then our one channel (when we serve); anything else gets DM */
        if (!R.initiator && f.dlci == 0 && R.state == S_SRV)
        {
            ctl(addr_rsp(0), F_UA | F_PF, 0, 0);
            R.state = S_SRV_MUX;
            R.t0 = bt_now();
        }
        else if (!R.initiator && f.dlci == (BT_SERVE_CHANNEL << 1) && R.state == S_SRV_MUX)
        {
            ctl(addr_rsp(f.dlci), F_UA | F_PF, 0, 0);
            R.dlci = f.dlci;
            R.state = S_OPEN;
            R.send_msc = 1;
        }
        else
            ctl(addr_rsp(f.dlci), F_DM | F_PF, 0, 0);
        break;
    case F_UIH:
        if (f.dlci == 0)
            on_mcc(f.data, f.len);
        else if (f.dlci == R.dlci && R.state == S_OPEN && R.rx && f.len)
            R.rx(f.data, f.len);
        break;
    default:
        break;
    }
}

/* ---- the session ---- */
int bt_rfcomm_open(unsigned channel)
{
    if (channel < 1 || channel > 30)
        return BT_E_PARTIAL;
    if (bt_link_state() != BT_LINK_UP)
        return BT_E_NOT_UP;
    if (R.state == S_L2CAP || R.state == S_MUX || R.state == S_DLC || R.state == S_OPEN || R.state == S_CLOSING)
        return BT_E_LINK_BUSY;
    int slot = bt_chan_open(RF_PSM, BT_USER_RFCOMM);
    if (slot < 0)
        return BT_E_LINK_BUSY;
    R.slot = slot;
    R.dlci = channel << 1; /* we started the connection: the direction bit is 0 */
    R.initiator = 1;
    R.rx = 0;
    R.state = S_L2CAP;
    R.why = 0;
    R.code = 0;
    R.t0 = bt_now();
    R.tx_paused = R.want_close_l2 = R.sabm_dlc = R.send_msc = 0;
    return 0;
}

void bt_rfcomm_close(void)
{
    switch (R.state)
    {
    case S_OPEN:
        ctl(addr_cmd(R.dlci), F_DISC | F_PF, 0, 0);
        R.state = S_CLOSING;
        R.t0 = bt_now();
        break;
    case S_L2CAP:
    case S_MUX:
    case S_DLC:
        bt_chan_close(R.slot);
        R.state = S_CLOSED;
        break;
    default:
        break;
    }
}

int bt_rfcomm_accept(int slot, void (*rx)(const unsigned char *data, unsigned n))
{
    if (R.state == S_L2CAP || R.state == S_MUX || R.state == S_DLC || R.state == S_OPEN || R.state == S_CLOSING || R.state == S_SRV || R.state == S_SRV_MUX)
        return BT_E_LINK_BUSY;
    R.slot = slot;
    R.initiator = 0;
    R.dlci = 0;
    R.state = S_SRV;
    R.why = 0;
    R.code = 0;
    R.t0 = bt_now();
    R.tx_paused = R.want_close_l2 = R.sabm_dlc = R.send_msc = 0;
    R.rx = rx;
    return 0;
}

int bt_rfcomm_is_server(void) { return !R.initiator; }

BtRfState bt_rfcomm_state(void)
{
    switch (R.state)
    {
    case S_L2CAP:
    case S_MUX:
    case S_DLC:
    case S_SRV:
    case S_SRV_MUX:
        return BT_RF_OPENING;
    case S_OPEN:
        return BT_RF_OPEN;
    case S_CLOSING:
        return BT_RF_CLOSING;
    case S_CLOSED:
        return BT_RF_CLOSED;
    case S_FAILED:
        return BT_RF_FAILED;
    default:
        return BT_RF_IDLE;
    }
}

const char *bt_rfcomm_why(int *code)
{
    if (code)
        *code = R.code;
    return R.why ? R.why : "";
}

void bt_rfcomm_set_rx(void (*rx)(const unsigned char *data, unsigned n)) { R.rx = rx; }

int bt_rfcomm_send(const unsigned char *data, unsigned n)
{
    if (R.state != S_OPEN || R.tx_paused)
        return -1;
    unsigned done = 0;
    unsigned char f[BT_RF_MAX_FRAME + 5];
    while (done < n)
    {
        unsigned part = n - done > BT_RF_MAX_FRAME ? BT_RF_MAX_FRAME : n - done;
        unsigned len = bt_rfcomm_build(f, sizeof f, addr_cmd(R.dlci), F_UIH, data + done, part);
        if (!len || bt_chan_send(R.slot, f, len) != 0)
            return -1;
        R.frames_tx++;
        done += part;
    }
    return (int)n;
}

void bt_rfcomm_step(unsigned long long now)
{
    if (R.state == S_IDLE || R.state == S_FAILED)
        return;
    if (R.state == S_CLOSED && !R.want_close_l2)
        return;
    if (R.want_close_l2)
    {
        R.want_close_l2 = 0;
        bt_chan_close(R.slot);
        return;
    }
    if (R.sabm_dlc)
    {
        R.sabm_dlc = 0;
        ctl(addr_cmd(R.dlci), F_SABM | F_PF, 0, 0);
        return;
    }
    if (R.send_msc)
    {
        R.send_msc = 0;
        send_msc();
        return;
    }
    switch (R.state)
    {
    case S_L2CAP:
    {
        int st = bt_chan_state(R.slot);
        if (st == BT_CHAN_OPEN)
        {
            ctl(addr_cmd(0), F_SABM | F_PF, 0, 0);
            R.state = S_MUX;
            R.t0 = now;
        }
        else if (st == BT_CHAN_FREE)
            fail("the device has no RFCOMM channel for us", bt_chan_result(R.slot));
        else if (now && now - R.t0 > STEP_LIMIT_MS)
            fail("no answer to the RFCOMM channel request", 0);
        break;
    }
    case S_MUX:
    case S_DLC:
        if (bt_chan_state(R.slot) != BT_CHAN_OPEN)
            fail("the link or channel went away", 0);
        else if (now && now - R.t0 > STEP_LIMIT_MS)
            fail(R.state == S_MUX ? "no answer to the multiplexer request" : "no answer to the channel request", 0);
        break;
    case S_OPEN:
        if (bt_chan_state(R.slot) != BT_CHAN_OPEN)
            R.state = S_CLOSED;
        break;
    case S_SRV:
    case S_SRV_MUX:
        if (bt_chan_state(R.slot) == BT_CHAN_FREE)
            R.state = S_CLOSED; /* the device closed the L2CAP channel */
        else if (now && now - R.t0 > SERVE_WAIT_MS)
            fail("the device never opened the channel", 0);
        break;
    case S_CLOSING:
        if (bt_chan_state(R.slot) != BT_CHAN_OPEN || (now && now - R.t0 > CLOSE_LIMIT_MS))
        {
            R.state = S_CLOSED;
            R.want_close_l2 = 1;
        }
        break;
    default:
        break;
    }
}

void bt_rfcomm_reset(void)
{
    if (R.state == S_L2CAP || R.state == S_MUX || R.state == S_DLC)
    {
        R.state = S_FAILED;
        R.why = "link ended";
        R.code = 0;
    }
    else if (R.state == S_OPEN || R.state == S_CLOSING || R.state == S_SRV || R.state == S_SRV_MUX)
        R.state = S_CLOSED;
    R.want_close_l2 = R.sabm_dlc = R.send_msc = 0;
}
