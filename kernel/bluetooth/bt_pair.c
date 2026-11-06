/* Bluetooth pairing and link security (stage 6). See include/bluetooth.h.
 *
 * Many devices refuse to talk until the two sides have "paired": agreed on a secret link key and switched encryption on. The key is
 * made once (pairing) and kept by both sides; later links start with the stored key and only prove they both have it.
 *
 * Simple Secure Pairing (Bluetooth 2.1 and newer), the case of a headset without keys or screen, "Just Works":
 *   host: Authentication Requested
 *   chip: Link Key Request              -> we have no key for this device: negative reply
 *   chip: IO Capability Request         -> we say: no input, no output, general bonding (store the key)
 *   chip: IO Capability Response        (what the device said)
 *   chip: User Confirmation Request     -> a number to compare; with no display on either side this is "Just Works": we accept
 *   chip: Simple Pairing Complete, Link Key Notification (the new key), Authentication Complete
 *   host: Set Connection Encryption -> chip: Encryption Change
 * Older devices use a PIN instead: the chip sends PIN Code Request and we answer with a fixed PIN ("0000", what headsets are given).
 * "Just Works" proves nothing about who is on the other end (no protection against a man in the middle); that is the price of devices
 * without a display. The link key stays in memory only (it is a secret; nothing is written to a disk).
 * Commands and events: Core specification volume 2 part E / volume 4 part E, checked against Linux net/bluetooth hci_event.c / hci.h. */
#include "bluetooth.h"

#define OP_AUTH_REQUESTED 0x0411
#define OP_SET_CONN_ENCRYPT 0x0413
#define OP_LINK_KEY_REPLY 0x040B
#define OP_LINK_KEY_NEG_REPLY 0x040C
#define OP_PIN_CODE_REPLY 0x040D
#define OP_PIN_CODE_NEG_REPLY 0x040E
#define OP_IO_CAPABILITY_REPLY 0x042B
#define OP_USER_CONFIRM_REPLY 0x042C
#define OP_USER_CONFIRM_NEG_REPLY 0x042D
#define OP_IO_CAPABILITY_NEG_REPLY 0x0434
#define OP_WRITE_SSP_MODE 0x0C56
#define OP_WRITE_LOCAL_NAME 0x0C13
#define OP_SET_EVENT_MASK 0x0C01

#define EV_AUTH_COMPLETE 0x06
#define EV_ENCRYPT_CHANGE 0x08
#define EV_PIN_CODE_REQUEST 0x16
#define EV_LINK_KEY_REQUEST 0x17
#define EV_LINK_KEY_NOTIFY 0x18
#define EV_IO_CAPA_REQUEST 0x31
#define EV_IO_CAPA_REPLY 0x32
#define EV_USER_CONFIRM_REQUEST 0x33
#define EV_SIMPLE_PAIR_COMPLETE 0x36

#define IO_NO_INPUT_NO_OUTPUT 0x03 /* what we tell the device we can do: nothing */
#define AUTH_GENERAL_BONDING 0x04  /* no man-in-the-middle protection asked for, keep the key */
#define REASON_PAIRING_NOT_ALLOWED 0x18
#define PIN_DEFAULT "0000"

#define PAIR_LIMIT_MS 30000
#define QN 8

typedef struct
{
    unsigned op;
    unsigned char n;
    unsigned char p[24];
} Cmd;

enum { PS_IDLE, PS_START, PS_AUTH, PS_ENCRYPT, PS_DONE, PS_FAILED };

static struct
{
    int state;
    const char *why;
    int code;
    unsigned handle;
    unsigned char addr[6];     /* the device being paired, least significant byte first */
    unsigned long long t0;
    int setup_done;
    int key_used;              /* we answered a Link Key Request with the stored key during this attempt */
    int retried;               /* ... and the device did not know it: once more without */
    int used_pin;
    int peer_io, peer_auth;    /* what the device said in its IO Capability Response, -1 = nothing yet */
    int sp_status;             /* Simple Pairing Complete status, -1 = none */
    int encrypted;
    Cmd q[QN];
    unsigned qh, qt;
} P;

/* the key store: one device (we only ever talk to one). Survives the end of a link, not a restart of the system. */
static struct
{
    int valid;
    unsigned char addr[6];
    unsigned char key[16];
    int type;
} K;

static int same6(const unsigned char *a, const unsigned char *b)
{
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

void bt_pair_forget(void) { K.valid = 0; }

static int pairing(void) { return P.state == PS_START || P.state == PS_AUTH || P.state == PS_ENCRYPT; }

static void push(unsigned op, const unsigned char *par, unsigned n)
{
    if (P.qh - P.qt >= QN || n > sizeof P.q[0].p)
        return;
    Cmd *c = &P.q[P.qh++ % QN];
    c->op = op;
    c->n = (unsigned char)n;
    for (unsigned i = 0; i < n; i++)
        c->p[i] = par[i];
}

static void fail(const char *why, int code)
{
    P.state = PS_FAILED;
    P.why = why;
    P.code = code;
}

void bt_pair_reset(void)
{
    if (pairing())
        fail("link ended", 0);
    else if (P.state != PS_DONE && P.state != PS_FAILED)
        P.state = PS_IDLE;
    P.qh = P.qt = 0;
    P.setup_done = 0; /* a chip that was reset has its settings back at the defaults */
}

/* ---- events ---- */

void bt_pair_event(const unsigned char *e, unsigned len)
{
    (void)len;
    unsigned plen = e[1];
    const unsigned char *p = e + 2;
    unsigned char r[24];
    switch (e[0])
    {
    case EV_LINK_KEY_REQUEST: /* address: the chip asks for the stored key of this device */
        if (plen < 6)
            return;
        if (K.valid && same6(K.addr, p))
        {
            for (int i = 0; i < 6; i++)
                r[i] = p[i];
            for (int i = 0; i < 16; i++)
                r[6 + i] = K.key[i];
            push(OP_LINK_KEY_REPLY, r, 22);
            if (pairing())
                P.key_used = 1;
        }
        else
            push(OP_LINK_KEY_NEG_REPLY, p, 6);
        break;
    case EV_PIN_CODE_REQUEST: /* the device wants a PIN (old style pairing): ours only when we started the pairing */
        if (plen < 6)
            return;
        if (pairing())
        {
            for (int i = 0; i < 6; i++)
                r[i] = p[i];
            r[6] = 4; /* PIN length */
            for (int i = 0; i < 16; i++)
                r[7 + i] = i < 4 ? (unsigned char)PIN_DEFAULT[i] : 0;
            push(OP_PIN_CODE_REPLY, r, 23);
            P.used_pin = 1;
        }
        else
            push(OP_PIN_CODE_NEG_REPLY, p, 6);
        break;
    case EV_IO_CAPA_REQUEST: /* the chip needs our input / output abilities */
        if (plen < 6)
            return;
        if (pairing())
        {
            for (int i = 0; i < 6; i++)
                r[i] = p[i];
            r[6] = IO_NO_INPUT_NO_OUTPUT;
            r[7] = 0; /* no out-of-band data */
            r[8] = AUTH_GENERAL_BONDING;
            push(OP_IO_CAPABILITY_REPLY, r, 9);
        }
        else /* a pairing nobody here asked for: refuse */
        {
            for (int i = 0; i < 6; i++)
                r[i] = p[i];
            r[6] = REASON_PAIRING_NOT_ALLOWED;
            push(OP_IO_CAPABILITY_NEG_REPLY, r, 7);
        }
        break;
    case EV_IO_CAPA_REPLY: /* address, the device's IO capability, OOB flag, authentication requirements */
        if (plen >= 9 && same6(P.addr, p))
        {
            P.peer_io = p[6];
            P.peer_auth = p[8];
        }
        break;
    case EV_USER_CONFIRM_REQUEST: /* address, a number both sides would show: with no display on our side it is "Just Works": accept */
        if (plen < 6)
            return;
        push(pairing() ? OP_USER_CONFIRM_REPLY : OP_USER_CONFIRM_NEG_REPLY, p, 6);
        break;
    case EV_SIMPLE_PAIR_COMPLETE: /* status, address */
        if (plen >= 7 && same6(P.addr, p + 1))
            P.sp_status = p[0];
        break;
    case EV_LINK_KEY_NOTIFY: /* address, the new key (16), its type: keep it */
        if (plen >= 23)
        {
            for (int i = 0; i < 6; i++)
                K.addr[i] = p[i];
            for (int i = 0; i < 16; i++)
                K.key[i] = p[6 + i];
            K.type = p[22];
            K.valid = 1;
        }
        break;
    case EV_AUTH_COMPLETE: /* status, handle */
        if (plen < 3 || !pairing() || (le16(p + 1) & 0x0FFF) != P.handle)
            return;
        if (p[0] == 0)
        {
            P.state = PS_ENCRYPT;
            P.t0 = bt_now();
            unsigned char c[3] = {(unsigned char)P.handle, (unsigned char)(P.handle >> 8), 1};
            push(OP_SET_CONN_ENCRYPT, c, 3);
        }
        else if (p[0] == 0x06 && P.key_used && !P.retried) /* "PIN or key missing": the device lost the key we hold: ask again without it */
        {
            K.valid = 0;
            P.retried = 1;
            P.key_used = 0;
            P.state = PS_START;
        }
        else
            fail("authentication failed", p[0]);
        break;
    case EV_ENCRYPT_CHANGE: /* status, handle, encryption on */
        if (plen < 4 || P.state != PS_ENCRYPT || (le16(p + 1) & 0x0FFF) != P.handle)
            return;
        if (p[0] == 0 && p[3])
        {
            P.encrypted = 1;
            P.state = PS_DONE;
        }
        else
            fail("encryption failed", p[0]);
        break;
    default:
        break;
    }
}

/* ---- starting and stepping ---- */

int bt_pair_start(void)
{
    if (bt_link_state() != BT_LINK_UP)
        return BT_E_NOT_UP;
    if (pairing())
        return BT_E_LINK_BUSY;
    P.state = PS_START;
    P.why = 0;
    P.code = 0;
    P.handle = bt_link_handle();
    const unsigned char *a = bt_link_addr();
    for (int i = 0; i < 6; i++)
        P.addr[i] = a[i];
    P.t0 = bt_now();
    P.key_used = P.retried = P.used_pin = 0;
    P.peer_io = P.peer_auth = -1;
    P.sp_status = -1;
    P.encrypted = 0;
    return 0;
}

void bt_pair_step(unsigned long long now)
{
    unsigned char r[8];
    unsigned n;
    /* answers the chip waits for (bt_cmd, so not from inside the event handler) */
    while (P.qt != P.qh)
    {
        Cmd c = P.q[P.qt++ % QN];
        bt_cmd(c.op, c.p, c.n, r, sizeof r, &n, 1000);
    }
    if (P.state == PS_START && bt_link_state() == BT_LINK_UP)
    {
        if (!P.setup_done)
        {
            /* Secure Simple Pairing on, the events about it enabled, a name for the device's list */
            unsigned char mode = 1;
            static const unsigned char mask[8] = BT_EVENT_MASK;
            unsigned char name[248];
            for (unsigned i = 0; i < sizeof name; i++)
                name[i] = 0;
            static const char NAME[] = "HarisOS";
            for (unsigned i = 0; i < sizeof NAME - 1; i++)
                name[i] = (unsigned char)NAME[i];
            bt_cmd(OP_SET_EVENT_MASK, mask, 8, r, sizeof r, &n, 1000);
            bt_cmd(OP_WRITE_SSP_MODE, &mode, 1, r, sizeof r, &n, 1000);
            bt_cmd(OP_WRITE_LOCAL_NAME, name, sizeof name, r, sizeof r, &n, 1000);
            P.setup_done = 1;
        }
        unsigned char h[2] = {(unsigned char)P.handle, (unsigned char)(P.handle >> 8)};
        P.state = PS_AUTH; /* before the command: the events follow at once */
        int st = bt_cmd(OP_AUTH_REQUESTED, h, 2, r, sizeof r, &n, 1000); /* Command Status */
        if (st != 0)
            fail("the chip refused", st);
        return;
    }
    if (pairing() && now && now - P.t0 > PAIR_LIMIT_MS)
        fail("time-out", 0);
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

static const char *key_type_word(int t)
{
    switch (t)
    {
    case 0: return "combination key";
    case 4: return "unauthenticated key (P-192)";
    case 5: return "authenticated key (P-192)";
    case 7: return "unauthenticated key (P-256)";
    case 8: return "authenticated key (P-256)";
    default: return "key";
    }
}

int bt_pair_line(unsigned index, char *buf, unsigned cap)
{
    if (cap < 2 || index > 1)
        return -1;
    Out o = {buf, cap, 0, 0};
    if (index == 0)
    {
        switch (P.state)
        {
        case PS_IDLE:
            puts_(&o, K.valid ? "pair: -, a link key is stored" : "pair: -");
            break;
        case PS_START:
        case PS_AUTH:
            puts_(&o, "pair: authenticating");
            if (P.used_pin)
                puts_(&o, " (old style, PIN " PIN_DEFAULT ")");
            break;
        case PS_ENCRYPT:
            puts_(&o, "pair: encrypting the link");
            break;
        case PS_DONE:
            puts_(&o, "pair: done, ");
            puts_(&o, K.valid ? key_type_word(K.type) : "no key stored");
            puts_(&o, ", link encrypted");
            if (P.used_pin)
                puts_(&o, " (PIN " PIN_DEFAULT ")");
            break;
        default:
            puts_(&o, "pair: failed: ");
            puts_(&o, P.why ? P.why : "?");
            if (P.code)
            {
                puts_(&o, " (status 0x");
                putx(&o, (unsigned)P.code, 2);
                puts_(&o, ")");
            }
        }
    }
    else
    {
        puts_(&o, "pair: device io capability ");
        if (P.peer_io < 0)
            puts_(&o, "-");
        else
            putd(&o, P.peer_io);
        puts_(&o, ", simple pairing ");
        if (P.sp_status < 0)
            puts_(&o, "-");
        else
        {
            puts_(&o, "status 0x");
            putx(&o, (unsigned)P.sp_status, 2);
        }
        puts_(&o, P.retried ? ", the stored key was refused once" : "");
    }
    buf[o.n] = 0;
    if (o.cut)
        return -1;
    return (int)o.n;
}
