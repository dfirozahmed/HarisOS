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
#define OP_LINK_KEY_NEG_REPLY 0x040C
#define OP_PIN_NEG_REPLY 0x040E

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
#define SIG_DISCONNECTION_REQ 0x06
#define SIG_ECHO_REQ 0x08
#define SIG_ECHO_RSP 0x09
#define SIG_INFO_REQ 0x0A
#define SIG_INFO_RSP 0x0B

#define INFO_EXT_FEATURES 0x0002
#define INFO_FIXED_CHANNELS 0x0003

#define TXQ_N 10 /* the ACL queue of the core holds 8 packets, each can ask for one answer */
#define PDU_MAX 48
#define RX_MAX 320

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
    int neg_key, neg_pin;
    unsigned char peer[6];
    unsigned key_requests, pin_requests;
    unsigned peer_cmds;        /* signalling commands received */

    Pdu txq[TXQ_N];
    unsigned txh, txt;

    unsigned char rx[RX_MAX];  /* the L2CAP PDU being put together from ACL fragments */
    unsigned rx_have, rx_need;
    unsigned acl_rx, acl_other;
} L;

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
}

BtLinkState bt_link_state(void) { return L.state; }

/* ---- sending signalling commands ---- */

/* Queue one command on the signalling channel (cid 1): PDU = length (2), cid (2), command code, id, command length (2), data. */
static void sig_queue(unsigned code, unsigned id, const unsigned char *data, unsigned n)
{
    if (L.txh - L.txt >= TXQ_N || n > PDU_MAX - 8)
        return;
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
        /* other channels: none open yet */
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
    (void)len;
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
        }
        break;
    case EV_AUTH_COMPLETE:
        if (plen >= 3 && (L.state == BT_LINK_UP) && (le16(p + 1) & 0x0FFF) == L.handle)
            L.auth_status = p[0];
        break;
    case EV_LINK_KEY_REQUEST: /* the chip asks for the stored key of a device: we have none (no pairing yet) */
        if (plen >= 6)
        {
            for (int i = 0; i < 6; i++)
                L.peer[i] = p[i];
            L.neg_key = 1;
            L.key_requests++;
        }
        break;
    case EV_PIN_CODE_REQUEST: /* the legacy pairing asks for a PIN: we refuse */
        if (plen >= 6)
        {
            for (int i = 0; i < 6; i++)
                L.peer[i] = p[i];
            L.neg_pin = 1;
            L.pin_requests++;
        }
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

    /* refusals the chip waits for (these use bt_cmd, so they are not sent from inside an event handler) */
    if (L.neg_key)
    {
        L.neg_key = 0;
        bt_cmd(OP_LINK_KEY_NEG_REPLY, L.peer, 6, r, sizeof r, &n, 1000);
    }
    if (L.neg_pin)
    {
        L.neg_pin = 0;
        bt_cmd(OP_PIN_NEG_REPLY, L.peer, 6, r, sizeof r, &n, 1000);
    }

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

    /* send what is queued; copy the entry first, sending may handle events that queue more */
    while (L.txt != L.txh && L.state == BT_LINK_UP)
    {
        Pdu p = L.txq[L.txt++ % TXQ_N];
        if (bt_acl_send(L.handle, p.data, p.len) < 0)
            break;
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
        return -1;
    }
    buf[o.n] = 0;
    if (o.cut)
        return -1;
    return (int)o.n;
}
