/* Bluetooth ACL link and L2CAP signalling (stage 4). See include/bluetooth.h.
 *
 * Classic Bluetooth connects like this: the host asks the chip to "page" a device (Create Connection); the chip sends page packets on
 * the frequencies the device listens to and, when it answers, reports a Connection Complete event with a connection handle. Data to and
 * from that device then travels in ACL packets that carry the handle. On top of ACL sits L2CAP: every PDU has a length and a channel id,
 * and channel 1 is the "signalling channel" where the two sides send each other commands (connect a channel, echo, ask for features ...).
 * Here: the signalling channel is used for a probe (Echo Request: does data flow both ways; Information Requests: what can the device do)
 * and the device's own requests are answered. Other channels (SDP, AVDTP) come in later stages. Commands and events: Core specification
 * volume 2 part E / volume 3 part A, checked against Linux net/bluetooth (hci_conn.c, hci_event.c, l2cap_core.c). */
#include "bluetooth.h"

#define OP_CREATE_CONNECTION 0x0405
#define OP_CREATE_CONNECTION_CANCEL 0x0408
#define OP_DISCONNECT 0x0406

#define EV_CONNECTION_COMPLETE 0x03
#define EV_DISCONNECTION_COMPLETE 0x05
#define EV_AUTH_COMPLETE 0x06
#define EV_PIN_CODE_REQUEST 0x16
#define EV_LINK_KEY_REQUEST 0x17

#define PKT_TYPES 0xCC18 /* DM1 DH1 DM3 DH3 DM5 DH5 allowed; the "EDR not allowed" bits 0 = allowed */
#define CONNECT_LIMIT_MS 25000
#define CLOSE_LIMIT_MS 5000
#define PROBE_LIMIT_MS 3000

/* L2CAP signalling command codes */
#define SIG_COMMAND_REJECT 0x01
#define SIG_CONNECTION_REQ 0x02
#define SIG_CONNECTION_RSP 0x03
#define SIG_CONFIG_REQ 0x04
#define SIG_CONFIG_RSP 0x05
#define SIG_DISCONNECTION_REQ 0x06
#define SIG_DISCONNECTION_RSP 0x07
#define SIG_ECHO_REQ 0x08
#define SIG_ECHO_RSP 0x09
#define SIG_INFO_REQ 0x0A
#define SIG_INFO_RSP 0x0B

#define INFO_EXT_FEATURES 0x0002
#define INFO_FIXED_CHANNELS 0x0003

#define TXQ_N 10 /* the ACL queue of the core holds 8 packets, each can ask for one answer */
#define PDU_MAX 64
#define RX_MAX 700 /* the default L2CAP MTU is 672 bytes, plus the 4 byte header */

typedef struct
{
    unsigned char len;
    unsigned char data[PDU_MAX];
} Pdu;

static struct
{
    BtLinkState state;
    unsigned char addr[6]; /* least significant byte first, as in the HCI commands */
    unsigned handle;
    unsigned long long t0;     /* when the current phase began */
    int fail_status;           /* BT_LINK_FAILED: the HCI status or BT_E_* */
    int last_reason;           /* how the last link ended: HCI reason, or -1 = never was one */
    int want_close;
    int auth_status;           /* Authentication Complete status seen on this link, -1 = none */

    /* the probe */
    int probe;                 /* 0 not started, 1 running, 2 done */
    unsigned long long t_probe;
    unsigned char sig_id;
    unsigned char echo_id, info_ext_id, info_fix_id;
    int echo_state;            /* 0 not asked, 1 waiting, 2 answered, 3 no answer, 4 rejected */
    unsigned long long echo_t0;
    unsigned echo_ms, echo_bytes;
    int ext_state, fix_state;  /* same codes as echo_state, for the two Information Requests (4 = not supported) */
    unsigned ext_features;
    unsigned fixed0;

    /* what the device asked of us and we still owe */
    unsigned key_requests, pin_requests;
    unsigned peer_cmds;        /* signalling commands received */

    Pdu txq[TXQ_N];
    unsigned txh, txt;

    unsigned char rx[RX_MAX];  /* the L2CAP PDU being put together from ACL fragments */
    unsigned rx_have, rx_need;
    unsigned acl_rx, acl_other;
    unsigned tx_sent, tx_fail;     /* ACL packets handed to the chip / attempts the chip did not take */
    unsigned char trace[16][2];    /* the last signalling commands, both ways: direction + code, and a detail */
    unsigned ntrace;
} L;

static int chan_sig(unsigned code, unsigned id, const unsigned char *d, unsigned n);
static void chan_data(unsigned cid, const unsigned char *d, unsigned n);
static void chan_reset(void);
static void chan_link_down(void);
static void sdp_step(unsigned long long now);

static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned le32(const unsigned char *p) { return le16(p) | (le16(p + 2) << 16); }
static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

void bt_link_reset(void)
{
    unsigned char *z = (unsigned char *)&L;
    for (unsigned i = 0; i < sizeof L; i++)
        z[i] = 0;
    L.last_reason = -1;
    L.auth_status = -1;
    chan_reset();
    bt_pair_reset();
}

unsigned bt_link_handle(void) { return L.handle; }
const unsigned char *bt_link_addr(void) { return L.addr; }

BtLinkState bt_link_state(void) { return L.state; }

/* ---- sending signalling commands ---- */

/* Queue one command on the signalling channel (cid 1): PDU = length (2), cid (2), command code, id, command length (2), data. */
/* The log of signalling commands: what we sent (tx) and what the device sent, each with the one detail that tells the most (the PSM of a
 * Connection Request, the result of a response, the type of an Information Request ...). For the person who has to find out why a
 * channel does not open. */
static unsigned trace_aux(int tx, unsigned code, const unsigned char *d, unsigned n)
{
    (void)tx;
    switch (code)
    {
    case SIG_CONNECTION_REQ: /* psm */
    case SIG_INFO_REQ:       /* type */
    case SIG_INFO_RSP:       /* type */
    case SIG_COMMAND_REJECT: /* reason */
        return n >= 2 ? le16(d) : 0;
    case SIG_CONNECTION_RSP: /* dcid, scid, result */
    case SIG_CONFIG_RSP:     /* scid, flags, result */
        return code == SIG_CONNECTION_RSP ? (n >= 6 ? le16(d + 4) : 0) : (n >= 6 ? le16(d + 4) : 0);
    default:
        return 0;
    }
}

static void trace_add(int tx, unsigned code, const unsigned char *d, unsigned n)
{
    unsigned char *t = L.trace[L.ntrace++ % 16];
    t[0] = (unsigned char)((tx ? 0x80 : 0) | (code & 0x7F));
    unsigned a = trace_aux(tx, code, d, n);
    t[1] = (unsigned char)(a > 255 ? 255 : a);
}

static void sig_queue(unsigned code, unsigned id, const unsigned char *data, unsigned n)
{
    if (L.txh - L.txt >= TXQ_N || n > PDU_MAX - 8)
        return;
    trace_add(1, code, data, n);
    Pdu *p = &L.txq[L.txh++ % TXQ_N];
    put16(p->data, 4 + n);
    put16(p->data + 2, 1);
    p->data[4] = (unsigned char)code;
    p->data[5] = (unsigned char)id;
    put16(p->data + 6, n);
    for (unsigned i = 0; i < n; i++)
        p->data[8 + i] = data[i];
    p->len = (unsigned char)(8 + n);
}

/* The same for data on a channel (not the signalling channel): PDU = length (2), channel id of the device's end (2), data. */
static void data_queue(unsigned cid, const unsigned char *data, unsigned n)
{
    if (L.txh - L.txt >= TXQ_N || n > PDU_MAX - 4)
        return;
    Pdu *p = &L.txq[L.txh++ % TXQ_N];
    put16(p->data, n);
    put16(p->data + 2, cid);
    for (unsigned i = 0; i < n; i++)
        p->data[4 + i] = data[i];
    p->len = (unsigned char)(4 + n);
}

static unsigned next_id(void)
{
    if (++L.sig_id == 0) /* 0 is not a valid identifier */
        L.sig_id = 1;
    return L.sig_id;
}

/* ---- the device's commands and our answers ---- */

static void sig_command(unsigned code, unsigned id, const unsigned char *d, unsigned n)
{
    unsigned char r[16];
    L.peer_cmds++;
    trace_add(0, code, d, n);
    if (chan_sig(code, id, d, n)) /* about a channel we opened */
        return;
    switch (code)
    {
    case SIG_COMMAND_REJECT:
        if (L.echo_state == 1 && id == L.echo_id)
            L.echo_state = 4;
        if (L.ext_state == 1 && id == L.info_ext_id)
            L.ext_state = 4;
        if (L.fix_state == 1 && id == L.info_fix_id)
            L.fix_state = 4;
        break;
    case SIG_ECHO_REQ:
        sig_queue(SIG_ECHO_RSP, id, d, n > 24 ? 24 : n); /* the same data back */
        break;
    case SIG_ECHO_RSP:
        if (L.echo_state == 1 && id == L.echo_id)
        {
            L.echo_state = 2;
            L.echo_bytes = n;
            L.echo_ms = (unsigned)(bt_now() - L.echo_t0);
        }
        break;
    case SIG_INFO_REQ:
        if (n >= 2)
        {
            unsigned type = le16(d);
            put16(r, type);
            if (type == INFO_EXT_FEATURES)
            {
                put16(r + 2, 0); /* success */
                r[4] = r[5] = r[6] = r[7] = 0; /* no optional features */
                sig_queue(SIG_INFO_RSP, id, r, 8);
            }
            else if (type == INFO_FIXED_CHANNELS)
            {
                put16(r + 2, 0);
                for (int i = 4; i < 12; i++)
                    r[i] = 0;
                r[4] = 0x02; /* bit 1: the signalling channel */
                sig_queue(SIG_INFO_RSP, id, r, 12);
            }
            else
            {
                put16(r + 2, 1); /* not supported */
                sig_queue(SIG_INFO_RSP, id, r, 4);
            }
        }
        break;
    case SIG_INFO_RSP:
        if (n >= 4)
        {
            unsigned type = le16(d), result = le16(d + 2);
            if (type == INFO_EXT_FEATURES && L.ext_state == 1 && id == L.info_ext_id)
            {
                if (result == 0 && n >= 8)
                {
                    L.ext_features = le32(d + 4);
                    L.ext_state = 2;
                }
                else
                    L.ext_state = 4;
            }
            else if (type == INFO_FIXED_CHANNELS && L.fix_state == 1 && id == L.info_fix_id)
            {
                if (result == 0 && n >= 5)
                {
                    L.fixed0 = d[4];
                    L.fix_state = 2;
                }
                else
                    L.fix_state = 4;
            }
        }
        break;
    case SIG_CONNECTION_REQ:
        /* the device wants a channel to a service (psm); we have none yet: "PSM not supported" (result 2). Answer: dcid (2), scid (2), result (2), status (2) */
        if (n >= 4)
        {
            put16(r, 0);
            put16(r + 2, le16(d + 2));
            put16(r + 4, 2);
            put16(r + 6, 0);
            sig_queue(SIG_CONNECTION_RSP, id, r, 8);
        }
        break;
    case SIG_CONFIG_REQ:
    case SIG_DISCONNECTION_REQ:
        /* a channel that does not exist (we never made one): Command Reject, reason 2 = invalid CID; then the CIDs of the request */
        put16(r, 2);
        put16(r + 2, n >= 2 ? le16(d) : 0);
        put16(r + 4, n >= 4 ? le16(d + 2) : 0);
        sig_queue(SIG_COMMAND_REJECT, id, r, 6);
        break;
    default:
        if (!(code & 1)) /* a request we do not know: Command Reject, reason 0 = command not understood. Answers (odd codes) get no answer. */
        {
            put16(r, 0);
            sig_queue(SIG_COMMAND_REJECT, id, r, 2);
        }
        break;
    }
}

static void sig_pdu(const unsigned char *p, unsigned n)
{
    while (n >= 4)
    {
        unsigned clen = le16(p + 2);
        if (4 + clen > n)
            break;
        sig_command(p[0], p[1], p + 4, clen);
        p += 4 + clen;
        n -= 4 + clen;
    }
}

/* ---- L2CAP channels and the SDP client ----
 * A channel is opened with a Connection Request (which service, by PSM, and our end's channel id); the device answers with its own end's id.
 * Then each side sends a Configuration Request (options, here none: the defaults, 672 byte MTU) and answers the other's. When both are done the
 * channel is open and data goes in PDUs addressed to the other end's id. Closed with a Disconnection Request. Core specification volume 3 part A. */
#define CHAN_N 3
#define CHAN_BASE_CID 0x0040
#define SDP_PSM 1
#define SDP_ACC_MAX 2560
#define SDP_MAX_BYTES 400 /* the most attribute data the device may put in one answer */
#define CHAN_LIMIT_MS 10000
#define SDP_LIMIT_MS 5000

enum { CH_FREE, CH_CONNECTING, CH_CONFIG, CH_OPEN, CH_CLOSING };

typedef struct
{
    int state;
    unsigned psm, scid, dcid, mtu; /* scid: our end, dcid: the device's end; mtu: the most we may send in one PDU */
    unsigned char conn_id, cfg_id, disc_id;
    int cfg_acked, cfg_peer;       /* our Configuration Request was accepted / we accepted the device's */
    unsigned long long t0;
    int result;                    /* why a Connection Response refused (kept after the channel is gone) */
} Chan;

static Chan CH[CHAN_N];

enum { SDP_IDLE, SDP_OPENING, SDP_ASKING, SDP_DONE, SDP_FAILED };

static const char WHY_CHANNEL[] = "channel refused"; /* the only failure whose code is an L2CAP result */

static struct
{
    int state;
    const char *why;
    int code;
    int chan;
    unsigned tid;
    int send, want_close;
    unsigned long long t0;
    unsigned char acc[SDP_ACC_MAX]; /* the attribute lists, piece by piece (the device may answer in several) */
    unsigned acc_len;
    unsigned char cont[16];         /* continuation state: the device's bookmark for "send the rest" */
    unsigned contlen;
    BtSdpService svc[BT_SDP_MAX];
    int nsvc;
} SD;

static void chan_reset(void)
{
    for (int i = 0; i < CHAN_N; i++)
    {
        CH[i].state = CH_FREE;
        CH[i].result = 0;
    }
    SD.state = SDP_IDLE;
    SD.why = 0;
    SD.code = 0;
    SD.nsvc = 0;
    SD.acc_len = 0;
    SD.send = SD.want_close = 0;
}

static void sdp_fail(const char *why, int code)
{
    SD.state = SDP_FAILED;
    SD.why = why;
    SD.code = code;
    SD.want_close = 1;
}

/* the link is gone: every channel with it */
static void chan_link_down(void)
{
    bt_pair_reset();
    if (SD.state == SDP_OPENING || SD.state == SDP_ASKING)
    {
        SD.state = SDP_FAILED;
        SD.why = "link ended";
        SD.code = 0;
    }
    for (int i = 0; i < CHAN_N; i++)
        CH[i].state = CH_FREE;
    SD.want_close = 0;
}

static int chan_open(unsigned psm)
{
    for (int i = 0; i < CHAN_N; i++)
    {
        Chan *c = &CH[i];
        if (c->state != CH_FREE)
            continue;
        c->state = CH_CONNECTING;
        c->psm = psm;
        c->scid = CHAN_BASE_CID + (unsigned)i;
        c->dcid = 0;
        c->mtu = 672;
        c->cfg_acked = c->cfg_peer = 0;
        c->result = 0;
        c->t0 = bt_now();
        c->conn_id = (unsigned char)next_id();
        unsigned char d[4];
        put16(d, psm);
        put16(d + 2, c->scid);
        sig_queue(SIG_CONNECTION_REQ, c->conn_id, d, 4);
        return i;
    }
    return -1;
}

static void chan_close(int i)
{
    Chan *c = &CH[i];
    if (c->state == CH_OPEN || c->state == CH_CONFIG)
    {
        unsigned char d[4];
        put16(d, c->dcid);
        put16(d + 2, c->scid);
        c->disc_id = (unsigned char)next_id();
        sig_queue(SIG_DISCONNECTION_REQ, c->disc_id, d, 4);
        c->state = CH_CLOSING;
        c->t0 = bt_now();
    }
    else
        c->state = CH_FREE;
}

static Chan *chan_by_scid(unsigned scid)
{
    for (int i = 0; i < CHAN_N; i++)
        if (CH[i].state != CH_FREE && CH[i].scid == scid)
            return &CH[i];
    return 0;
}

static void chan_check_open(Chan *c)
{
    if (c->state == CH_CONFIG && c->cfg_acked && c->cfg_peer)
        c->state = CH_OPEN;
}

/* Signalling commands that concern a channel. Returns 1 if the command was taken care of here. */
static int chan_sig(unsigned code, unsigned id, const unsigned char *d, unsigned n)
{
    unsigned char r[8];
    switch (code)
    {
    case SIG_CONNECTION_RSP: /* dcid (2), scid (2), result (2), status (2) */
    {
        if (n < 8)
            return 1;
        Chan *c = chan_by_scid(le16(d + 2));
        if (!c || c->state != CH_CONNECTING || c->conn_id != id)
            return 1;
        unsigned result = le16(d + 4);
        if (result == 0)
        {
            c->dcid = le16(d);
            c->state = CH_CONFIG;
            c->cfg_id = (unsigned char)next_id();
            put16(r, c->dcid);
            put16(r + 2, 0); /* no continuation, no options */
            sig_queue(SIG_CONFIG_REQ, c->cfg_id, r, 4);
            c->t0 = bt_now();
        }
        else if (result == 1) /* pending (the device is still deciding) */
            c->t0 = bt_now();
        else
        {
            c->result = (int)result;
            c->state = CH_FREE;
        }
        return 1;
    }
    case SIG_CONFIG_RSP: /* scid (2: the device's end), flags (2), result (2), options */
    {
        if (n < 6)
            return 1;
        for (int i = 0; i < CHAN_N; i++)
        {
            Chan *c = &CH[i];
            if (c->state == CH_CONFIG && c->dcid == le16(d) && c->cfg_id == id)
            {
                if (le16(d + 4) == 0)
                {
                    c->cfg_acked = 1;
                    chan_check_open(c);
                }
                else
                {
                    c->result = 0x100 + (int)le16(d + 4); /* configuration refused */
                    c->state = CH_FREE;
                }
            }
        }
        return 1;
    }
    case SIG_CONFIG_REQ: /* dcid (2: our end), flags (2), options */
    {
        if (n < 4)
            return 0;
        Chan *c = chan_by_scid(le16(d));
        if (!c || (c->state != CH_CONFIG && c->state != CH_OPEN))
            return 0; /* not ours: the generic answer (Command Reject) follows */
        for (unsigned o = 4; o + 2 <= n;) /* options: type (1), length (1), value; type 1 is the MTU the device accepts */
        {
            unsigned len = d[o + 1];
            if (o + 2 + len > n)
                break;
            if ((d[o] & 0x7F) == 1 && len == 2)
                c->mtu = le16(d + o + 2);
            o += 2 + len;
        }
        put16(r, c->dcid);
        put16(r + 2, 0); /* flags */
        put16(r + 4, 0); /* result: success */
        sig_queue(SIG_CONFIG_RSP, id, r, 6);
        c->cfg_peer = 1;
        chan_check_open(c);
        return 1;
    }
    case SIG_DISCONNECTION_REQ: /* dcid (2: our end), scid (2: the device's end) */
    {
        if (n < 4)
            return 0;
        Chan *c = chan_by_scid(le16(d));
        if (!c)
            return 0;
        sig_queue(SIG_DISCONNECTION_RSP, id, d, 4);
        c->state = CH_FREE;
        return 1;
    }
    case SIG_DISCONNECTION_RSP:
        for (int i = 0; i < CHAN_N; i++)
            if (CH[i].state == CH_CLOSING && CH[i].disc_id == id)
                CH[i].state = CH_FREE;
        return 1;
    case SIG_COMMAND_REJECT: /* a request of ours was refused: let the channel go (the echo / information logic sees it too) */
        for (int i = 0; i < CHAN_N; i++)
        {
            Chan *c = &CH[i];
            if ((c->state == CH_CONNECTING && c->conn_id == id) || (c->state == CH_CONFIG && c->cfg_id == id))
            {
                c->result = 0x200;
                c->state = CH_FREE;
            }
        }
        return 0;
    default:
        return 0;
    }
}

/* ---- the SDP client ---- */

/* An SDP PDU from the device: PDU id (1), transaction id (2), parameter length (2), parameters. */
static void sdp_rx(const unsigned char *d, unsigned n)
{
    if (SD.state != SDP_ASKING || n < 5)
        return;
    unsigned pdu = d[0], tid = ((unsigned)d[1] << 8) | d[2], plen = ((unsigned)d[3] << 8) | d[4];
    if (5 + plen > n || (tid & 0xFFFF) != (SD.tid & 0xFFFF))
        return;
    const unsigned char *p = d + 5;
    if (pdu == 0x01) /* error response: error code (2) */
    {
        sdp_fail("the device refused", plen >= 2 ? (int)(((unsigned)p[0] << 8) | p[1]) : 0);
        return;
    }
    if (pdu != 0x07 || plen < 3)
    {
        sdp_fail("unexpected answer", (int)pdu);
        return;
    }
    /* ServiceSearchAttributeResponse: byte count of this piece (2), the piece, continuation state (1 + bytes) */
    unsigned cnt = ((unsigned)p[0] << 8) | p[1];
    if (2 + cnt + 1 > plen)
    {
        sdp_fail("answer too short", 0);
        return;
    }
    unsigned cl = p[2 + cnt];
    if (cl > sizeof SD.cont || 2 + cnt + 1 + cl > plen)
    {
        sdp_fail("bad continuation", 0);
        return;
    }
    if (SD.acc_len + cnt > SDP_ACC_MAX)
    {
        sdp_fail("answer too long", 0);
        return;
    }
    for (unsigned i = 0; i < cnt; i++)
        SD.acc[SD.acc_len++] = p[2 + i];
    if (cl)
    {
        for (unsigned i = 0; i < cl; i++)
            SD.cont[i] = p[2 + cnt + 1 + i];
        SD.contlen = cl;
        SD.send = 1; /* the rest: another request with the bookmark (sent from sdp_step) */
        return;
    }
    int k = bt_sdp_parse(SD.acc, SD.acc_len, SD.svc, BT_SDP_MAX);
    if (k < 0)
    {
        sdp_fail("answer not understood", 0);
        return;
    }
    SD.nsvc = k;
    SD.state = SDP_DONE;
    SD.want_close = 1;
}

static void chan_data(unsigned cid, const unsigned char *d, unsigned n)
{
    Chan *c = chan_by_scid(cid);
    if (!c || c->state != CH_OPEN)
        return;
    if (SD.state == SDP_ASKING && c == &CH[SD.chan])
        sdp_rx(d, n);
}

int bt_sdp_browse(void)
{
    if (L.state != BT_LINK_UP)
        return BT_E_NOT_UP;
    if (SD.state == SDP_OPENING || SD.state == SDP_ASKING)
        return BT_E_LINK_BUSY;
    SD.nsvc = 0;
    SD.acc_len = 0;
    SD.contlen = 0;
    SD.send = SD.want_close = 0;
    SD.why = 0;
    SD.code = 0;
    SD.tid = 0; /* a new channel, a new numbering of the transactions */
    SD.chan = chan_open(SDP_PSM);
    if (SD.chan < 0)
        return BT_E_LINK_BUSY;
    SD.state = SDP_OPENING;
    return 0;
}

static void sdp_step(unsigned long long now)
{
    /* channels that take too long to close are just forgotten */
    for (int i = 0; i < CHAN_N; i++)
        if (CH[i].state == CH_CLOSING && now && now - CH[i].t0 > CHAN_LIMIT_MS)
            CH[i].state = CH_FREE;

    if (SD.state == SDP_OPENING)
    {
        Chan *c = &CH[SD.chan];
        if (c->state == CH_OPEN)
        {
            SD.state = SDP_ASKING;
            SD.acc_len = 0;
            SD.contlen = 0;
            SD.send = 1;
        }
        else if (c->state == CH_FREE)
        {
            SD.state = SDP_FAILED;
            SD.why = WHY_CHANNEL;
            SD.code = c->result;
        }
        else if (now && now - c->t0 > CHAN_LIMIT_MS)
        {
            /* where did it stop? the Connection Response, our Configuration being accepted, or the device's own Configuration Request */
            SD.why = c->state == CH_CONNECTING ? "no answer to the connection request"
                     : (!c->cfg_acked && !c->cfg_peer) ? "the configuration did not start"
                     : c->cfg_acked ? "the device never sent its configuration"
                                    : "the device never accepted our configuration";
            c->state = CH_FREE;
            SD.state = SDP_FAILED;
            SD.code = 0;
        }
    }
    if (SD.state == SDP_ASKING)
    {
        if (SD.send)
        {
            unsigned char req[PDU_MAX];
            SD.send = 0;
            SD.tid++;
            unsigned n = bt_sdp_request(req, sizeof req, SD.tid, SDP_MAX_BYTES, SD.cont, SD.contlen);
            data_queue(CH[SD.chan].dcid, req, n);
            SD.t0 = now;
        }
        else if (now && now - SD.t0 > SDP_LIMIT_MS)
            sdp_fail("no answer", 0);
    }
    if (SD.want_close)
    {
        SD.want_close = 0;
        if (SD.chan >= 0 && SD.chan < CHAN_N)
            chan_close(SD.chan);
    }
}

/* ---- ACL data from the device ---- */

void bt_link_acl(const unsigned char *pkt, unsigned len)
{
    unsigned handle = le16(pkt) & 0x0FFF, pb = (le16(pkt) >> 12) & 3, dlen = le16(pkt + 2);
    if (L.state != BT_LINK_UP && L.state != BT_LINK_CLOSING)
    {
        L.acl_other++;
        return;
    }
    if (handle != L.handle || len < 4 + dlen)
    {
        L.acl_other++;
        return;
    }
    const unsigned char *d = pkt + 4;
    L.acl_rx++;
    if (pb == 0 || pb == 2) /* first piece of an L2CAP PDU (0 = host to controller style, 2 = controller to host) */
    {
        L.rx_have = 0;
        if (dlen < 4)
        {
            L.rx_need = 0;
            return;
        }
        L.rx_need = 4 + le16(d);
        if (L.rx_need > RX_MAX) /* longer than we keep: skip this PDU */
        {
            L.rx_need = 0;
            return;
        }
    }
    else if (pb == 1) /* continuation */
    {
        if (L.rx_need == 0)
            return;
    }
    else
        return;
    for (unsigned i = 0; i < dlen && L.rx_have < L.rx_need; i++)
        L.rx[L.rx_have++] = d[i];
    if (L.rx_have == L.rx_need)
    {
        unsigned cid = le16(L.rx + 2);
        L.rx_need = 0;
        if (cid == 1)
            sig_pdu(L.rx + 4, L.rx_have - 4);
        else
            chan_data(cid, L.rx + 4, L.rx_have - 4);
        L.rx_have = 0;
    }
}

/* ---- HCI events about links ---- */

static int same6(const unsigned char *a, const unsigned char *b)
{
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

void bt_link_event(const unsigned char *e, unsigned len)
{
    bt_pair_event(e, len); /* pairing and security events: replies are queued there */
    unsigned plen = e[1];
    const unsigned char *p = e + 2;
    switch (e[0])
    {
    case EV_CONNECTION_COMPLETE:
        /* status (1), handle (2), address (6), link type (1), encryption (1) */
        if (plen >= 11 && L.state == BT_LINK_CONNECTING && same6(L.addr, p + 3))
        {
            if (p[0] == 0)
            {
                L.handle = le16(p + 1) & 0x0FFF;
                L.state = BT_LINK_UP;
                L.t0 = bt_now();
                L.rx_have = L.rx_need = 0;
            }
            else
            {
                L.fail_status = p[0];
                L.state = BT_LINK_FAILED;
            }
        }
        break;
    case EV_DISCONNECTION_COMPLETE:
        /* status (1), handle (2), reason (1) */
        if (plen >= 4 && (L.state == BT_LINK_UP || L.state == BT_LINK_CLOSING) && (le16(p + 1) & 0x0FFF) == L.handle && p[0] == 0)
        {
            L.last_reason = p[3];
            L.state = BT_LINK_NONE;
            bt_acl_reset_credits();
            chan_link_down();
        }
        break;
    case EV_AUTH_COMPLETE:
        if (plen >= 3 && (L.state == BT_LINK_UP) && (le16(p + 1) & 0x0FFF) == L.handle)
            L.auth_status = p[0];
        break;
    case EV_LINK_KEY_REQUEST: /* the chip asks for the stored key of a device (bt_pair.c answers) */
        if (plen >= 6)
            L.key_requests++;
        break;
    case EV_PIN_CODE_REQUEST: /* the old style pairing asks for a PIN (bt_pair.c answers) */
        if (plen >= 6)
            L.pin_requests++;
        break;
    default:
        break;
    }
}

/* ---- the user's calls ---- */

int bt_link_connect(const unsigned char addr[6])
{
    if (!bt_hci_up())
        return BT_E_NOT_UP;
    if (bt_scan_state() == BT_SCAN_INQUIRY || bt_scan_state() == BT_SCAN_NAMES)
        return BT_E_SCANNING; /* the chip cannot page while it inquires */
    if (L.state == BT_LINK_CONNECTING || L.state == BT_LINK_UP || L.state == BT_LINK_CLOSING)
        return BT_E_LINK_BUSY;
    /* forget the last link */
    int last_reason = L.last_reason;
    bt_link_reset();
    L.last_reason = last_reason;
    for (int i = 0; i < 6; i++)
        L.addr[i] = addr[5 - i];
    /* what the inquiry told about this device (page scan repetition mode, clock offset) makes paging quicker */
    unsigned psrm = 1, clock = 0;
    for (int i = 0; i < bt_scan_count(); i++)
    {
        const BtScanDev *d = bt_scan_dev(i);
        if (same6(d->addr, L.addr))
        {
            psrm = d->psrm;
            clock = 0x8000u | d->clock; /* bit 15: the offset is valid */
        }
    }
    /* address (6), packet types (2), page scan repetition mode (1), reserved (1), clock offset (2), allow role switch (1) */
    unsigned char par[13];
    for (int i = 0; i < 6; i++)
        par[i] = L.addr[i];
    put16(par + 6, PKT_TYPES);
    par[8] = (unsigned char)psrm;
    par[9] = 0;
    put16(par + 10, clock);
    par[12] = 1;
    L.state = BT_LINK_CONNECTING; /* before the command: the event may follow at once */
    L.t0 = bt_now();
    unsigned char r[4];
    unsigned n;
    int st = bt_cmd(OP_CREATE_CONNECTION, par, sizeof par, r, sizeof r, &n, 1000); /* Command Status */
    if (st != 0)
    {
        L.fail_status = st;
        L.state = BT_LINK_FAILED;
        return st;
    }
    return 0;
}

int bt_link_disconnect(void)
{
    if (L.state == BT_LINK_UP)
        L.want_close = 1;
    else if (L.state == BT_LINK_CONNECTING)
        L.want_close = 1; /* step() cancels the paging */
    return 0;
}

/* ---- stepping ---- */

void bt_link_step(void)
{
    unsigned long long now = bt_now();
    unsigned char r[8];
    unsigned n;

    /* answers the chip waits for (key, PIN, pairing), and the pairing itself: these use bt_cmd, so they are not sent from inside an event handler */
    bt_pair_step(now);

    if (L.state == BT_LINK_CONNECTING)
    {
        if (L.want_close || (now && now - L.t0 > CONNECT_LIMIT_MS))
        {
            L.fail_status = L.want_close ? 0x16 : BT_E_TIMEOUT; /* 0x16: we gave up on purpose */
            L.want_close = 0;
            bt_cmd(OP_CREATE_CONNECTION_CANCEL, L.addr, 6, r, sizeof r, &n, 1000);
            L.state = BT_LINK_FAILED;
        }
        return;
    }

    if (L.state == BT_LINK_CLOSING)
    {
        if (now && now - L.t0 > CLOSE_LIMIT_MS) /* no Disconnection Complete: take it as closed */
        {
            L.last_reason = 0x16;
            L.state = BT_LINK_NONE;
            bt_acl_reset_credits();
            chan_link_down();
        }
        return;
    }

    if (L.state != BT_LINK_UP)
        return;

    if (L.want_close)
    {
        L.want_close = 0;
        unsigned char par[3];
        put16(par, L.handle);
        par[2] = 0x13; /* reason: remote user terminated connection */
        L.state = BT_LINK_CLOSING;
        L.t0 = now;
        bt_cmd(OP_DISCONNECT, par, 3, r, sizeof r, &n, 1000);
        return;
    }

    /* start the probe once */
    if (L.probe == 0)
    {
        L.probe = 1;
        L.t_probe = now;
        static const unsigned char data[7] = {'H', 'a', 'r', 'i', 's', 'O', 'S'};
        L.echo_id = (unsigned char)next_id();
        sig_queue(SIG_ECHO_REQ, L.echo_id, data, sizeof data);
        L.echo_t0 = now;
        L.echo_state = 1;
        unsigned char t[2];
        L.info_ext_id = (unsigned char)next_id();
        put16(t, INFO_EXT_FEATURES);
        sig_queue(SIG_INFO_REQ, L.info_ext_id, t, 2);
        L.ext_state = 1;
        L.info_fix_id = (unsigned char)next_id();
        put16(t, INFO_FIXED_CHANNELS);
        sig_queue(SIG_INFO_REQ, L.info_fix_id, t, 2);
        L.fix_state = 1;
    }

    sdp_step(now);

    /* send what is queued; copy the entry first, sending may handle events that queue more */
    while (L.txt != L.txh && L.state == BT_LINK_UP)
    {
        Pdu p = L.txq[L.txt % TXQ_N];
        int rc = bt_acl_send(L.handle, p.data, p.len);
        if (rc < 0)
        {
            L.tx_fail++;
            if (rc == BT_E_TIMEOUT && L.tx_fail < 40) /* the chip has no room right now, nothing went out: the PDU stays, next poll */
                break;
            L.txt++; /* it cannot be sent whole (or the chip keeps refusing): given up */
            break;
        }
        L.txt++;
        L.tx_sent++;
    }

    if (L.probe == 1)
    {
        int open = L.echo_state == 1 || L.ext_state == 1 || L.fix_state == 1;
        if (!open || (now && now - L.t_probe > PROBE_LIMIT_MS))
        {
            if (L.echo_state == 1)
                L.echo_state = 3;
            if (L.ext_state == 1)
                L.ext_state = 3;
            if (L.fix_state == 1)
                L.fix_state = 3;
            L.probe = 2;
        }
    }
}

/* ---- text ---- */

typedef struct
{
    char *p;
    unsigned cap, n;
    int cut;
} Out;

static void putc_(Out *o, char c)
{
    if (o->n + 1 < o->cap)
        o->p[o->n++] = c;
    else
        o->cut = 1;
}

static void puts_(Out *o, const char *s)
{
    while (*s)
        putc_(o, *s++);
}

static void putx(Out *o, unsigned v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        putc_(o, "0123456789abcdef"[(v >> (4 * i)) & 15]);
}

static void putd(Out *o, int v)
{
    char t[11];
    int k = 0;
    unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0)
        putc_(o, '-');
    do
    {
        t[k++] = (char)('0' + u % 10);
        u /= 10;
    } while (u && k < 10);
    while (k)
        putc_(o, t[--k]);
}

static void put_addr(Out *o)
{
    for (int k = 5; k >= 0; k--)
    {
        putx(o, L.addr[k], 2);
        if (k)
            putc_(o, ':');
    }
}

static void put_version(Out *o, unsigned v)
{
    puts_(o, "v");
    putd(o, (int)(v >> 8));
    putc_(o, '.');
    putd(o, (int)(v & 0xFF));
}

/* Line 4 and up: the SDP result. */
static int sdp_line(unsigned index, Out *o)
{
    if (index == 4)
    {
        switch (SD.state)
        {
        case SDP_IDLE:
            puts_(o, "sdp: -");
            break;
        case SDP_OPENING:
            puts_(o, "sdp: opening a channel to the service directory");
            break;
        case SDP_ASKING:
            puts_(o, "sdp: asking, ");
            putd(o, (int)SD.acc_len);
            puts_(o, " bytes so far");
            break;
        case SDP_DONE:
            puts_(o, "sdp: ");
            putd(o, SD.nsvc);
            puts_(o, SD.nsvc == 1 ? " service" : " services");
            break;
        default:
            puts_(o, "sdp: failed: ");
            puts_(o, SD.why ? SD.why : "?");
            if (SD.code)
            {
                puts_(o, " (");
                if (SD.why == WHY_CHANNEL && SD.code == 2)
                    puts_(o, "PSM not supported");
                else if (SD.why == WHY_CHANNEL && SD.code == 3)
                    puts_(o, "security block");
                else if (SD.why == WHY_CHANNEL && SD.code == 4)
                    puts_(o, "no resources");
                else if (SD.why == WHY_CHANNEL && SD.code >= 0x100)
                {
                    puts_(o, SD.code >= 0x200 ? "rejected" : "configuration refused, result ");
                    if (SD.code < 0x200)
                        putd(o, SD.code - 0x100);
                }
                else
                {
                    puts_(o, "code ");
                    putd(o, SD.code);
                }
                puts_(o, ")");
            }
        }
        return 1;
    }
    if (index >= 5 && SD.state == SDP_DONE && index - 5 < (unsigned)SD.nsvc)
    {
        const BtSdpService *s = &SD.svc[index - 5];
        const char *w = bt_sdp_uuid_name(s->class_id[0]);
        puts_(o, "service: ");
        if (!s->class_id[0])
            puts_(o, "(no class)");
        else
        {
            puts_(o, *w ? w : "unknown");
            puts_(o, " 0x");
            putx(o, s->class_id[0], 4);
        }
        if (s->class_id[1])
        {
            puts_(o, ", ");
            w = bt_sdp_uuid_name(s->class_id[1]);
            puts_(o, *w ? w : "unknown");
            puts_(o, " 0x");
            putx(o, s->class_id[1], 4);
        }
        if (s->profile)
        {
            puts_(o, "; profile ");
            w = bt_sdp_uuid_name(s->profile);
            puts_(o, *w ? w : "0x");
            if (!*w)
                putx(o, s->profile, 4);
            puts_(o, " ");
            put_version(o, s->profile_ver);
        }
        if (s->has_psm)
        {
            puts_(o, "; psm 0x");
            putx(o, s->psm, 2);
        }
        if (s->has_rfcomm)
        {
            puts_(o, "; rfcomm channel ");
            putd(o, (int)s->rfcomm);
        }
        if (s->name[0])
        {
            puts_(o, "; \"");
            puts_(o, s->name);
            puts_(o, "\"");
        }
        return 1;
    }
    return 0;
}

/* Lines 0x90 and 0x91: what was said on the signalling channel (last 10 commands, "->" ours, "<-" the device's) and the sending statistics. */
static int diag_line(unsigned index, char *buf, unsigned cap)
{
    if (cap < 2 || index > 1)
        return -1;
    Out o = {buf, cap, 0, 0};
    if (index == 0)
    {
        puts_(&o, "sig:");
        unsigned shown = L.ntrace < 10 ? L.ntrace : 10;
        for (unsigned i = L.ntrace - shown; i < L.ntrace; i++)
        {
            const unsigned char *t = L.trace[i % 16];
            unsigned code = t[0] & 0x7F;
            puts_(&o, (t[0] & 0x80) ? " ->" : " <-");
            putx(&o, code, 2);
            if (t[1] || code == SIG_CONNECTION_RSP || code == SIG_CONFIG_RSP)
            {
                putc_(&o, '/');
                putx(&o, t[1], t[1] > 15 ? 2 : 1);
            }
        }
        if (!shown)
            puts_(&o, " -");
    }
    else
    {
        const BtInfo *bi = bt_info();
        puts_(&o, "acl tx: sent ");
        putd(&o, (int)L.tx_sent);
        puts_(&o, ", not taken ");
        putd(&o, (int)L.tx_fail);
        puts_(&o, ", credits ");
        putd(&o, (int)bt_acl_credits());
        puts_(&o, " of ");
        putd(&o, bi->acl_pkts);
        puts_(&o, ", packet size ");
        putd(&o, bi->acl_mtu);
    }
    buf[o.n] = 0;
    if (o.cut)
        return -1;
    return (int)o.n;
}

/* HCI status / reason codes people meet (Core specification volume 1 part F) */
static const char *reason_word(int code)
{
    switch (code)
    {
    case 0x04: return "page timeout: the device did not answer";
    case 0x05: return "authentication failure";
    case 0x06: return "PIN or key missing";
    case 0x07: return "memory full";
    case 0x08: return "connection timeout";
    case 0x09: return "too many connections";
    case 0x0B: return "already connected";
    case 0x0C: return "command disallowed";
    case 0x0D: return "rejected: no resources";
    case 0x0E: return "rejected: security";
    case 0x0F: return "rejected: unacceptable address";
    case 0x10: return "accept timeout";
    case 0x11: return "unsupported feature";
    case 0x13: return "the device ended it";
    case 0x14: return "the device: low resources";
    case 0x15: return "the device: power off";
    case 0x16: return "we ended it";
    case 0x1F: return "unspecified error";
    case 0x22: return "LMP response timeout";
    case BT_E_TIMEOUT: return "gave up waiting";
    default: return "";
    }
}

int bt_link_line(unsigned index, char *buf, unsigned cap)
{
    if (cap < 2)
        return -1;
    if (index >= 0x90) /* diagnostics */
        return diag_line(index - 0x90, buf, cap);
    if (index >= 0x80) /* the pairing lines */
        return bt_pair_line(index - 0x80, buf, cap);
    Out o = {buf, cap, 0, 0};
    switch (index)
    {
    case 0:
        switch (L.state)
        {
        case BT_LINK_NONE:
            if (L.last_reason < 0)
                puts_(&o, "idle: no link yet");
            else
            {
                puts_(&o, "closed: reason 0x");
                putx(&o, (unsigned)L.last_reason, 2);
                puts_(&o, " ");
                puts_(&o, reason_word(L.last_reason));
            }
            break;
        case BT_LINK_CONNECTING:
            puts_(&o, "connecting to ");
            put_addr(&o);
            break;
        case BT_LINK_UP:
            puts_(&o, "connected to ");
            put_addr(&o);
            puts_(&o, ", handle 0x");
            putx(&o, L.handle, 3);
            break;
        case BT_LINK_CLOSING:
            puts_(&o, "closing the link to ");
            put_addr(&o);
            break;
        default:
            puts_(&o, "failed: ");
            if (L.fail_status < 0)
                putd(&o, L.fail_status);
            else
            {
                puts_(&o, "status 0x");
                putx(&o, (unsigned)L.fail_status, 2);
            }
            puts_(&o, " ");
            puts_(&o, reason_word(L.fail_status));
        }
        break;
    case 1:
        switch (L.echo_state)
        {
        case 0:
            puts_(&o, "echo: -");
            break;
        case 1:
            puts_(&o, "echo: waiting");
            break;
        case 2:
            puts_(&o, "echo: answered, ");
            putd(&o, (int)L.echo_bytes);
            puts_(&o, " bytes back in ");
            putd(&o, (int)L.echo_ms);
            puts_(&o, " ms");
            break;
        case 3:
            puts_(&o, "echo: no answer");
            break;
        default:
            puts_(&o, "echo: rejected by the device (command reject)");
        }
        break;
    case 2:
        puts_(&o, "info: ");
        if (L.ext_state == 0 && L.fix_state == 0)
            puts_(&o, "-");
        else if (L.ext_state == 1 || L.fix_state == 1)
            puts_(&o, "waiting");
        else
        {
            if (L.ext_state == 2)
            {
                puts_(&o, "extended features 0x");
                putx(&o, L.ext_features, 8);
            }
            else
                puts_(&o, "extended features not reported");
            puts_(&o, ", ");
            if (L.fix_state == 2)
            {
                puts_(&o, "fixed channels 0x");
                putx(&o, L.fixed0, 2);
            }
            else
                puts_(&o, "fixed channels not reported");
        }
        break;
    case 3:
        puts_(&o, "acl: received ");
        putd(&o, (int)L.acl_rx);
        puts_(&o, " (other ");
        putd(&o, (int)L.acl_other);
        puts_(&o, "), device commands ");
        putd(&o, (int)L.peer_cmds);
        puts_(&o, ", link key requests ");
        putd(&o, (int)L.key_requests);
        puts_(&o, ", PIN requests ");
        putd(&o, (int)L.pin_requests);
        if (L.auth_status >= 0)
        {
            puts_(&o, ", authentication 0x");
            putx(&o, (unsigned)L.auth_status, 2);
        }
        break;
    default:
        if (!sdp_line(index, &o))
            return -1;
    }
    buf[o.n] = 0;
    if (o.cut)
        return -1;
    return (int)o.n;
}
