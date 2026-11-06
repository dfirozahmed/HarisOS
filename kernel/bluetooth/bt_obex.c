/* OBEX client, Object Push: sends one file to a phone or computer.
 *
 * OBEX (IrDA's "object exchange", Bluetooth uses it over RFCOMM) is request and answer, all in packets: opcode (1), packet length (2), data. Data
 * after the fixed part is a list of headers: id (1), then by the id's top two bits
 *     00 text (UTF-16BE, zero ended): length (2, includes the 3 header bytes) + text
 *     01 bytes:                       length (2, includes the 3 header bytes) + bytes
 *     10 one byte
 *     11 four bytes (a number, most significant first)
 * The conversation here:
 *   CONNECT  (0x80): version 0x10, flags 0, the biggest packet we accept (2)        -> 0xA0 OK: version, flags, the biggest packet the device accepts
 *   PUT      (0x02 more follows, 0x82 final): Name (0x01), Length (0xC3), Body (0x48) chunk of the file ... the last one with End-of-Body (0x49)
 *                                                                                  -> 0x90 Continue (send the next), 0xA0 OK (all taken)
 *   DISCONNECT (0x81)                                                              -> 0xA0
 * A device may give a connection id in its CONNECT answer (header 0xCB): then every request must carry it. A phone asks its user whether the file is
 * welcome as the first PUT arrives, so that answer can take half a minute; later ones come fast. The answer to a PUT is waited for before the next
 * is sent, so the whole thing is one outstanding request at a time. Response codes: 0xC0 bad request, 0xC1 unauthorized, 0xC3 forbidden, 0xC4 not
 * found, 0xC6 not acceptable, 0xCF unsupported media type, 0xD0 .. internal / service errors. */
#include "bluetooth.h"

#define OBEX_PKT 1000     /* the biggest request we send (and the biggest answer we take) */
#define OBEX_RX_MAX 512
#define OP_CONNECT 0x80
#define OP_DISCONNECT 0x81
#define OP_PUT 0x02
#define OP_FINAL 0x80
#define OP_ABORT 0xFF
#define RSP_CONTINUE 0x90
#define RSP_OK 0xA0
#define H_NAME 0x01
#define H_LENGTH 0xC3
#define H_BODY 0x48
#define H_END_OF_BODY 0x49
#define H_CONN_ID 0xCB
#define CONNECT_LIMIT_MS 15000
#define FIRST_PUT_LIMIT_MS 60000 /* the user of the phone has to say yes */
#define PUT_LIMIT_MS 20000
#define DISC_LIMIT_MS 4000
#define NAME_MAX_CHARS 60

enum { OB_IDLE, OB_RF, OB_CONNECT, OB_PUT, OB_DISC, OB_DONE, OB_FAILED };

static struct
{
    int state;
    const char *why;
    int code;
    unsigned long long t0;
    BtObexRead rd;
    void *ctx;
    unsigned size, off, inflight, acked; /* off: the next byte to send; inflight: bytes in the request waiting for its answer; acked: bytes the device took */
    unsigned short name16[NAME_MAX_CHARS + 1];
    unsigned name_chars;                 /* including the terminating zero */
    unsigned channel;
    unsigned peer_max;
    int first, final_sent, have_cid, cancel;
    unsigned char cid[4];
    unsigned char acc[OBEX_RX_MAX];
    unsigned acc_len;
    int pkt_ready;
    unsigned char tx[OBEX_PKT];
} O;

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static void put_be16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

/* ---- building a PUT request ---- */
unsigned bt_obex_put_packet(unsigned char *out, unsigned cap, const unsigned char *cid, const unsigned short *name16, unsigned name_chars, unsigned total,
                            int first, int final, const unsigned char *body, unsigned body_len)
{
    unsigned n = 3;
    if (cap < 3)
        return 0;
    if (cid)
    {
        if (n + 5 > cap)
            return 0;
        out[n++] = H_CONN_ID;
        for (int i = 0; i < 4; i++)
            out[n++] = cid[i];
    }
    if (first)
    {
        unsigned hl = 3 + 2 * name_chars;
        if (n + hl + 5 > cap)
            return 0;
        out[n++] = H_NAME;
        put_be16(out + n, hl);
        n += 2;
        for (unsigned i = 0; i < name_chars; i++)
        {
            put_be16(out + n, name16[i]);
            n += 2;
        }
        out[n++] = H_LENGTH;
        out[n++] = (unsigned char)(total >> 24);
        out[n++] = (unsigned char)(total >> 16);
        out[n++] = (unsigned char)(total >> 8);
        out[n++] = (unsigned char)total;
    }
    if (n + 3 + body_len > cap)
        return 0;
    out[n++] = final ? H_END_OF_BODY : H_BODY;
    put_be16(out + n, 3 + body_len);
    n += 2;
    for (unsigned i = 0; i < body_len; i++)
        out[n++] = body[i];
    out[0] = (unsigned char)(final ? (OP_PUT | OP_FINAL) : OP_PUT);
    put_be16(out + 1, n);
    return n;
}

/* ---- the session ---- */
static void fail(const char *why, int code)
{
    O.state = OB_FAILED;
    O.why = why;
    O.code = code;
    bt_rfcomm_close();
}

static void on_rx(const unsigned char *d, unsigned n)
{
    if (O.pkt_ready)
        return; /* the last answer is not looked at yet: the device sent more than it should */
    for (unsigned i = 0; i < n && O.acc_len < OBEX_RX_MAX; i++)
        O.acc[O.acc_len++] = d[i];
    if (O.acc_len >= 3 && O.acc_len >= be16(O.acc + 1))
        O.pkt_ready = 1;
}

/* UTF-8 to UTF-16 (what OBEX names are); bad bytes become '?' */
static unsigned to_utf16(const char *s, unsigned short *out, unsigned max)
{
    unsigned n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n + 2 < max)
    {
        unsigned c = *p++, extra = 0;
        if (c >= 0xF0)
            c &= 0x07, extra = 3;
        else if (c >= 0xE0)
            c &= 0x0F, extra = 2;
        else if (c >= 0xC0)
            c &= 0x1F, extra = 1;
        else if (c >= 0x80)
            c = '?';
        for (; extra; extra--)
        {
            if ((*p & 0xC0) != 0x80)
            {
                c = '?';
                break;
            }
            c = c << 6 | (*p++ & 0x3F);
        }
        if (c >= 0x10000)
        {
            c -= 0x10000;
            out[n++] = (unsigned short)(0xD800 | (c >> 10));
            out[n++] = (unsigned short)(0xDC00 | (c & 0x3FF));
        }
        else
            out[n++] = (unsigned short)c;
    }
    out[n++] = 0;
    return n;
}

int bt_obex_send(const char *name, unsigned size, BtObexRead rd, void *ctx, unsigned server_channel)
{
    if (!name || !*name || !rd)
        return BT_E_SHORT;
    if (O.state == OB_RF || O.state == OB_CONNECT || O.state == OB_PUT || O.state == OB_DISC)
        return BT_E_LINK_BUSY;
    if (bt_link_state() != BT_LINK_UP)
        return BT_E_NOT_UP;
    int r = bt_rfcomm_open(server_channel);
    if (r != 0)
        return r;
    bt_rfcomm_set_rx(on_rx);
    O.rd = rd;
    O.ctx = ctx;
    O.size = size;
    O.off = O.inflight = O.acked = 0;
    O.name_chars = to_utf16(name, O.name16, sizeof O.name16 / sizeof O.name16[0]);
    O.channel = server_channel;
    O.peer_max = 255;
    O.first = 1;
    O.final_sent = O.have_cid = O.cancel = 0;
    O.acc_len = 0;
    O.pkt_ready = 0;
    O.why = 0;
    O.code = 0;
    O.state = OB_RF;
    O.t0 = bt_now();
    return 0;
}

static int send_packet(const unsigned char *p, unsigned n) { return bt_rfcomm_send(p, n) == (int)n ? 0 : -1; }

static void send_connect(void)
{
    unsigned char p[7] = {OP_CONNECT, 0, 7, 0x10, 0, (unsigned char)(OBEX_RX_MAX >> 8), (unsigned char)OBEX_RX_MAX};
    if (send_packet(p, sizeof p) != 0)
        fail("could not send OBEX connect", BT_E_TRANSPORT);
    else
    {
        O.state = OB_CONNECT;
        O.acc_len = 0;
        O.pkt_ready = 0;
        O.t0 = bt_now();
    }
}

static void send_disconnect(void)
{
    unsigned char p[8] = {OP_DISCONNECT, 0, 3};
    unsigned n = 3;
    if (O.have_cid)
    {
        p[n++] = H_CONN_ID;
        for (int i = 0; i < 4; i++)
            p[n++] = O.cid[i];
        p[2] = (unsigned char)n;
    }
    send_packet(p, n);
    O.state = OB_DISC;
    O.acc_len = 0;
    O.pkt_ready = 0;
    O.t0 = bt_now();
}

/* the next PUT: as much of the file as fits one packet of the size the device allows */
static void send_put(void)
{
    unsigned max = O.peer_max < OBEX_PKT ? O.peer_max : OBEX_PKT;
    unsigned over = 3 + 3 + (O.have_cid ? 5 : 0) + (O.first ? 3 + 2 * O.name_chars + 5 : 0);
    if (max <= over)
    {
        fail("the device accepts only tiny packets", (int)max);
        return;
    }
    unsigned room = max - over, left = O.size - O.off;
    unsigned chunk = left < room ? left : room;
    int final = chunk == left;
    unsigned char body[OBEX_PKT];
    if (chunk)
    {
        int got = O.rd(O.ctx, O.off, body, chunk);
        if (got != (int)chunk)
        {
            fail("could not read the file", got);
            return;
        }
    }
    unsigned n = bt_obex_put_packet(O.tx, sizeof O.tx, O.have_cid ? O.cid : 0, O.name16, O.name_chars, O.size, O.first, final, body, chunk);
    if (!n || send_packet(O.tx, n) != 0)
    {
        fail("could not send the file data", BT_E_TRANSPORT);
        return;
    }
    O.inflight = chunk;
    O.off += chunk;
    O.first = 0;
    O.final_sent = final;
    O.acc_len = 0;
    O.pkt_ready = 0;
    O.t0 = bt_now();
}

/* headers of a CONNECT answer: take the connection id */
static void read_headers(const unsigned char *p, unsigned n)
{
    while (n >= 2)
    {
        unsigned id = p[0], len;
        switch (id >> 6)
        {
        case 0:
        case 1:
            if (n < 3)
                return;
            len = be16(p + 1);
            if (len < 3 || len > n)
                return;
            break;
        case 2:
            len = 2;
            break;
        default:
            len = 5;
        }
        if (len > n)
            return;
        if (id == H_CONN_ID && len == 5)
        {
            for (int i = 0; i < 4; i++)
                O.cid[i] = p[1 + i];
            O.have_cid = 1;
        }
        p += len;
        n -= len;
    }
}

static void on_packet(void)
{
    unsigned op = O.acc[0], len = be16(O.acc + 1);
    switch (O.state)
    {
    case OB_CONNECT:
        if (op != RSP_OK || len < 7)
        {
            fail("the device refused the file transfer", (int)op);
            return;
        }
        O.peer_max = be16(O.acc + 5);
        if (O.peer_max < 255)
            O.peer_max = 255; /* the least the standard allows: a smaller figure is a broken device */
        read_headers(O.acc + 7, len - 7);
        O.state = OB_PUT;
        send_put();
        break;
    case OB_PUT:
        if (op == RSP_CONTINUE && !O.final_sent)
        {
            O.acked += O.inflight;
            O.inflight = 0;
            send_put();
        }
        else if (op == RSP_OK || (op == RSP_CONTINUE && O.final_sent))
        {
            O.acked = O.size;
            send_disconnect();
        }
        else
            fail(op == 0xC3 ? "the device does not accept files" : op == 0xCF ? "the device does not accept this kind of file" : op == 0xC6 ? "the device does not want this file" : "the device refused the file", (int)op);
        break;
    case OB_DISC:
        O.state = OB_DONE;
        bt_rfcomm_close();
        break;
    default:
        break;
    }
}

void bt_obex_step(unsigned long long now)
{
    if (O.state == OB_IDLE || O.state == OB_DONE || O.state == OB_FAILED)
        return;
    if (!bt_chan_tx_idle())
        return; /* queued control frames first: their order matters */
    if (O.cancel)
    {
        O.cancel = 0;
        if (O.state == OB_PUT)
        {
            unsigned char p[3] = {OP_ABORT, 0, 3};
            send_packet(p, 3);
        }
        fail("cancelled", 0);
        return;
    }
    BtRfState rs = bt_rfcomm_state();
    if (O.state == OB_RF)
    {
        if (rs == BT_RF_OPEN)
            send_connect();
        else if (rs == BT_RF_FAILED || rs == BT_RF_CLOSED || rs == BT_RF_IDLE)
        {
            int code;
            const char *w = bt_rfcomm_why(&code);
            fail(*w ? w : "the channel closed", code);
        }
        return;
    }
    if (rs != BT_RF_OPEN)
    {
        if (O.state == OB_DISC)
            O.state = OB_DONE;
        else
            fail("the channel closed", 0);
        return;
    }
    if (O.pkt_ready)
    {
        on_packet();
        return;
    }
    unsigned long long limit = O.state == OB_CONNECT ? CONNECT_LIMIT_MS : O.state == OB_DISC ? DISC_LIMIT_MS : O.acked == 0 && O.off <= O.inflight ? FIRST_PUT_LIMIT_MS : PUT_LIMIT_MS;
    if (now && now - O.t0 > limit)
    {
        if (O.state == OB_DISC)
        {
            O.state = OB_DONE; /* the file went through; the goodbye was not answered */
            bt_rfcomm_close();
        }
        else
            fail(O.state == OB_CONNECT ? "the device did not answer the OBEX connect" : "the device did not answer (did its user accept the file?)", BT_E_TIMEOUT);
    }
}

BtObexState bt_obex_state(void)
{
    switch (O.state)
    {
    case OB_RF:
    case OB_CONNECT:
    case OB_PUT:
    case OB_DISC:
        return BT_OBEX_RUNNING;
    case OB_DONE:
        return BT_OBEX_DONE;
    case OB_FAILED:
        return BT_OBEX_FAILED;
    default:
        return BT_OBEX_IDLE;
    }
}

void bt_obex_progress(unsigned *done, unsigned *total)
{
    if (done)
        *done = O.acked;
    if (total)
        *total = O.size;
}

const char *bt_obex_why(int *code)
{
    if (code)
        *code = O.code;
    return O.why ? O.why : "";
}

void bt_obex_cancel(void)
{
    if (bt_obex_state() == BT_OBEX_RUNNING)
        O.cancel = 1;
}

void bt_obex_reset(void)
{
    if (bt_obex_state() == BT_OBEX_RUNNING)
    {
        O.state = OB_FAILED;
        O.why = "link ended";
        O.code = 0;
    }
}

int bt_obex_line(char *buf, unsigned cap)
{
    static const char *const W[] = {"idle", "opening the channel", "connecting", "sending", "finishing", "done", "failed"};
    unsigned n = 0;
    const char *s = "obex: ";
    while (*s && n + 1 < cap)
        buf[n++] = *s++;
    s = W[O.state];
    while (*s && n + 1 < cap)
        buf[n++] = *s++;
    if (O.state == OB_FAILED)
    {
        s = ": ";
        while (*s && n + 1 < cap)
            buf[n++] = *s++;
        s = O.why ? O.why : "";
        while (*s && n + 1 < cap)
            buf[n++] = *s++;
    }
    buf[n] = 0;
    return (int)n;
}
