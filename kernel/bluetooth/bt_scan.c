/* Bluetooth device discovery: Inquiry, the answers, names (stage 3). See include/bluetooth.h.
 *
 * Classic Bluetooth finds devices like this: the host tells the chip to run an Inquiry; the chip hops over the 79 channels for a few
 * seconds sending short "is anybody there" packets (IAC = the General Inquiry Access Code, LAP 0x9E8B33) and listens for answers from
 * devices in discoverable mode. Each answer comes up as an event with the device's address, class and signal strength; newer
 * devices put their name into the answer too ("extended inquiry response", EIR). The others are asked for the name in a second step
 * (Remote Name Request: the chip pages the device, which takes a moment). Commands and events: Core specification volume 4 part E,
 * checked against Linux net/bluetooth (hci_event.c, hci_sync.c) and include/net/bluetooth/hci.h. */
#include "bluetooth.h"

#define OP_INQUIRY 0x0401
#define OP_INQUIRY_CANCEL 0x0402
#define OP_REMOTE_NAME_REQ 0x0419
#define OP_REMOTE_NAME_REQ_CANCEL 0x041A
#define OP_SET_EVENT_MASK 0x0C01
#define OP_WRITE_INQUIRY_MODE 0x0C45
#define OP_READ_LOCAL_FEATURES 0x1003

#define EV_INQUIRY_COMPLETE 0x01
#define EV_INQUIRY_RESULT 0x02
#define EV_REMOTE_NAME 0x07
#define EV_INQUIRY_RESULT_RSSI 0x22
#define EV_EXTENDED_INQUIRY_RESULT 0x2F

#define EIR_NAME_SHORT 0x08
#define EIR_NAME_COMPLETE 0x09

#define INQUIRY_UNITS 8              /* x 1.28 s = 10.24 s */
#define INQUIRY_LIMIT_MS 16000       /* the chip should have said "complete" by then */
#define NAME_LIMIT_MS 6000           /* a name request that takes longer is given up */

static struct
{
    BtScanState state;
    int setup_done;
    BtScanDev dev[BT_SCAN_MAX];
    int count;
    unsigned long long t0;     /* when the current phase began */
    int inquiry_status;        /* status of the Inquiry Complete event, or the failure of the command */
    int pending;               /* index of the device whose name was asked for, or -1 */
    unsigned overflow;         /* devices that did not fit in the list */
} S;

void bt_scan_reset(void)
{
    S.state = BT_SCAN_IDLE;
    S.setup_done = 0;
    S.count = 0;
    S.pending = -1;
    S.inquiry_status = 0;
    S.overflow = 0;
}

BtScanState bt_scan_state(void) { return S.state; }
int bt_scan_count(void) { return S.count; }
const BtScanDev *bt_scan_dev(int i) { return i >= 0 && i < S.count ? &S.dev[i] : 0; }

/* ---- the list ---- */

static int same_addr(const unsigned char *a, const unsigned char *b)
{
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static BtScanDev *find_or_add(const unsigned char *addr)
{
    for (int i = 0; i < S.count; i++)
        if (same_addr(S.dev[i].addr, addr))
            return &S.dev[i];
    if (S.count >= BT_SCAN_MAX)
    {
        S.overflow++;
        return 0;
    }
    BtScanDev *d = &S.dev[S.count++];
    unsigned char *z = (unsigned char *)d;
    for (unsigned i = 0; i < sizeof *d; i++)
        z[i] = 0;
    for (int i = 0; i < 6; i++)
        d->addr[i] = addr[i];
    return d;
}

/* Copies a name into the device record. The name is UTF-8 and may be longer than we keep: cut it between characters. */
static void set_name(BtScanDev *d, const unsigned char *n, unsigned len)
{
    if (len >= BT_NAME_MAX)
    {
        len = BT_NAME_MAX - 1;
        while (len > 0 && (n[len] & 0xC0) == 0x80) /* n[len] would be a continuation byte: back up to the start of that character */
            len--;
    }
    unsigned k = 0;
    for (unsigned i = 0; i < len && n[i]; i++)
        d->name[k++] = (char)n[i];
    d->name[k] = 0;
    d->name_state = k ? 1 : 0;
}

/* The name inside an extended inquiry response: a list of (length, type, data...) with the length counting the type byte. */
static void eir_name(BtScanDev *d, const unsigned char *eir, unsigned len)
{
    for (unsigned i = 0; i + 1 < len && eir[i];)
    {
        unsigned l = eir[i]; /* type + data */
        if (i + 1 + l > len)
            break;
        if ((eir[i + 1] == EIR_NAME_COMPLETE || (eir[i + 1] == EIR_NAME_SHORT && d->name_state == 0)) && l > 1)
            set_name(d, eir + i + 2, l - 1);
        i += 1 + l;
    }
}

/* ---- events ---- */

void bt_scan_event(const unsigned char *e, unsigned len)
{
    (void)len;
    unsigned plen = e[1];
    const unsigned char *p = e + 2;
    if (e[0] == EV_INQUIRY_COMPLETE && plen >= 1)
    {
        if (S.state == BT_SCAN_INQUIRY)
        {
            S.inquiry_status = p[0];
            S.state = BT_SCAN_NAMES;
            S.t0 = bt_now();
        }
    }
    else if ((e[0] == EV_INQUIRY_RESULT || e[0] == EV_INQUIRY_RESULT_RSSI || e[0] == EV_EXTENDED_INQUIRY_RESULT) && S.state != BT_SCAN_INQUIRY)
        return; /* answers of an inquiry that is over (or never asked for) */
    else if (e[0] == EV_INQUIRY_RESULT && plen >= 1)
    {
        /* num, then per device (14 bytes): address (6), page scan repetition mode, period mode, page scan mode, class (3), clock offset (2) */
        unsigned n = p[0];
        for (unsigned i = 0; i < n && 1 + (i + 1) * 14 <= plen; i++)
        {
            const unsigned char *r = p + 1 + i * 14;
            BtScanDev *d = find_or_add(r);
            if (!d)
                continue;
            d->psrm = r[6];
            for (int k = 0; k < 3; k++)
                d->cls[k] = r[9 + k];
            d->clock = (unsigned short)(r[12] | (r[13] << 8));
        }
    }
    else if (e[0] == EV_INQUIRY_RESULT_RSSI && plen >= 1)
    {
        /* per device (14 bytes): address (6), page scan repetition mode, period mode, class (3), clock offset (2), RSSI (1) */
        unsigned n = p[0];
        for (unsigned i = 0; i < n && 1 + (i + 1) * 14 <= plen; i++)
        {
            const unsigned char *r = p + 1 + i * 14;
            BtScanDev *d = find_or_add(r);
            if (!d)
                continue;
            d->psrm = r[6];
            for (int k = 0; k < 3; k++)
                d->cls[k] = r[8 + k];
            d->clock = (unsigned short)(r[11] | (r[12] << 8));
            d->rssi = (signed char)r[13];
            d->has_rssi = 1;
        }
    }
    else if (e[0] == EV_EXTENDED_INQUIRY_RESULT && plen >= 1 + 15)
    {
        /* one device: address (6), page scan repetition mode, period mode, class (3), clock offset (2), RSSI (1), then the 240 byte response */
        const unsigned char *r = p + 1;
        BtScanDev *d = find_or_add(r);
        if (!d)
            return;
        d->psrm = r[6];
        for (int k = 0; k < 3; k++)
            d->cls[k] = r[8 + k];
        d->clock = (unsigned short)(r[11] | (r[12] << 8));
        d->rssi = (signed char)r[13];
        d->has_rssi = 1;
        unsigned elen = plen - 1 - 14;
        eir_name(d, r + 14, elen > 240 ? 240 : elen);
    }
    else if (e[0] == EV_REMOTE_NAME && plen >= 7)
    {
        /* status (1), address (6), name (up to 248 bytes, NUL padded) */
        if (S.pending >= 0 && same_addr(S.dev[S.pending].addr, p + 1))
        {
            BtScanDev *d = &S.dev[S.pending];
            S.pending = -1;
            if (p[0] == 0)
                set_name(d, p + 7, plen - 7);
            if (d->name_state != 1)
                d->name_state = 3;
            S.t0 = bt_now();
        }
    }
}

/* ---- starting and stepping ---- */

/* What the chip should do for discovery. Failures here only mean fewer features (for example no names inside the answers). */
static void setup(void)
{
    unsigned char r[16];
    unsigned n = 0;
    unsigned char feat[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (bt_cmd(OP_READ_LOCAL_FEATURES, 0, 0, r, sizeof r, &n, 1000) == 0 && n >= 9)
        for (int i = 0; i < 8; i++)
            feat[i] = r[1 + i];
    /* The chip's default event mask (bits 0 .. 44) already has Inquiry Result, with RSSI, Remote Name; "Extended Inquiry Result"
     * (event 0x2F = bit 46, byte 5 bit 0x40) is not in it. */
    static const unsigned char mask[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x5F, 0x00, 0x00};
    bt_cmd(OP_SET_EVENT_MASK, mask, 8, r, sizeof r, &n, 1000);
    /* Inquiry mode as in Linux hci_write_inquiry_mode_sync: 2 = extended results if the chip can (features byte 6 bit 0), else 1 = with
     * RSSI if it can (byte 3 bit 6); the default 0 has neither. */
    unsigned char mode = (feat[6] & 0x01) ? 2 : ((feat[3] & 0x40) ? 1 : 0);
    if (mode)
        bt_cmd(OP_WRITE_INQUIRY_MODE, &mode, 1, r, sizeof r, &n, 1000);
    S.setup_done = 1;
}

int bt_scan_start(void)
{
    if (!bt_hci_up())
        return BT_E_NOT_UP;
    if (S.state == BT_SCAN_INQUIRY || S.state == BT_SCAN_NAMES)
        return BT_E_SCANNING;
    if (!S.setup_done)
        setup();
    S.count = 0;
    S.overflow = 0;
    S.pending = -1;
    S.inquiry_status = 0;
    static const unsigned char par[5] = {0x33, 0x8B, 0x9E, INQUIRY_UNITS, 0}; /* LAP of the GIAC, length, any number of answers */
    unsigned char r[4];
    unsigned n;
    S.state = BT_SCAN_INQUIRY; /* before the command: the chip may answer at once */
    S.t0 = bt_now();
    int st = bt_cmd(OP_INQUIRY, par, sizeof par, r, sizeof r, &n, 1000); /* Command Status: started (0) or refused */
    if (st != 0)
    {
        S.inquiry_status = st;
        S.state = BT_SCAN_DONE;
        return st;
    }
    return 0;
}

void bt_scan_step(void)
{
    unsigned long long now = bt_now();
    unsigned char r[4];
    unsigned n;
    if (S.state == BT_SCAN_INQUIRY)
    {
        if (!(now && now - S.t0 > INQUIRY_LIMIT_MS))
            return;
        /* no "Inquiry Complete": stop it ourselves, and go on with the names at once */
        bt_cmd(OP_INQUIRY_CANCEL, 0, 0, r, sizeof r, &n, 1000);
        S.inquiry_status = BT_E_TIMEOUT;
        S.state = BT_SCAN_NAMES;
        S.t0 = now;
    }
    if (S.state != BT_SCAN_NAMES)
        return;

    if (S.pending >= 0)
    {
        if (now - S.t0 > NAME_LIMIT_MS)
        {
            BtScanDev *d = &S.dev[S.pending];
            unsigned char a[6];
            for (int i = 0; i < 6; i++)
                a[i] = d->addr[i];
            d->name_state = 3;
            S.pending = -1;
            bt_cmd(OP_REMOTE_NAME_REQ_CANCEL, a, 6, r, sizeof r, &n, 1000);
        }
        return;
    }
    for (int i = 0; i < S.count; i++)
    {
        BtScanDev *d = &S.dev[i];
        if (d->name_state != 0)
            continue;
        /* address (6), page scan repetition mode, reserved, clock offset (bit 15 = valid) */
        unsigned char par[10];
        for (int k = 0; k < 6; k++)
            par[k] = d->addr[k];
        par[6] = d->psrm;
        par[7] = 0;
        par[8] = (unsigned char)d->clock;
        par[9] = (unsigned char)((d->clock >> 8) | 0x80);
        d->name_state = 2;
        S.pending = i; /* before the command, so that an early answer finds it */
        S.t0 = now;
        int st = bt_cmd(OP_REMOTE_NAME_REQ, par, sizeof par, r, sizeof r, &n, 1000);
        if (st != 0)
        {
            d->name_state = 3;
            S.pending = -1;
            continue;
        }
        return;
    }
    S.state = BT_SCAN_DONE; /* every device has a name or gave up */
}

/* ---- text for the Device Manager and the system call ---- */

typedef struct
{
    char *p;
    unsigned cap, n;
    int cut; /* something did not fit */
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

/* Class of device -> a short word (Bluetooth Assigned Numbers: major device class in bits 8..12, minor in bits 2..7). */
static const char *class_word(const unsigned char *c)
{
    unsigned major = c[1] & 0x1F, minor = c[0] >> 2;
    switch (major)
    {
    case 1: return "computer";
    case 2: return "phone";
    case 3: return "network";
    case 4: /* audio / video */
        switch (minor)
        {
        case 1: return "headset";
        case 2: return "hands-free";
        case 4: return "microphone";
        case 5: return "speaker";
        case 6: return "headphones";
        case 7: return "portable audio";
        default: return "audio/video";
        }
    case 5: return (c[0] & 0xC0) == 0x40 ? "keyboard" : ((c[0] & 0xC0) == 0x80 ? "pointing device" : ((c[0] & 0xC0) == 0xC0 ? "keyboard+mouse" : "input device"));
    case 6: return "imaging";
    case 7: return "wearable";
    case 8: return "toy";
    case 9: return "health";
    default: return "device";
    }
}

int bt_scan_line(unsigned index, char *buf, unsigned cap)
{
    if (cap < 2)
        return -1;
    Out o = {buf, cap, 0, 0};
    if (index == 0)
    {
        switch (S.state)
        {
        case BT_SCAN_IDLE:
            puts_(&o, "idle: no scan yet");
            break;
        case BT_SCAN_INQUIRY:
            puts_(&o, "scanning: ");
            putd(&o, S.count);
            puts_(&o, " found so far");
            break;
        case BT_SCAN_NAMES:
            puts_(&o, "names: ");
            putd(&o, S.count);
            puts_(&o, " found, asking for names");
            break;
        default:
            puts_(&o, "done: ");
            putd(&o, S.count);
            puts_(&o, S.count == 1 ? " device" : " devices");
            if (S.inquiry_status)
            {
                puts_(&o, ", inquiry ended with ");
                if (S.inquiry_status < 0)
                    putd(&o, S.inquiry_status);
                else
                {
                    puts_(&o, "status ");
                    putx(&o, (unsigned)S.inquiry_status, 2);
                }
            }
            if (S.overflow)
                puts_(&o, ", list full");
        }
    }
    else
    {
        if (index > (unsigned)S.count)
            return -1;
        const BtScanDev *d = &S.dev[index - 1];
        for (int k = 5; k >= 0; k--)
        {
            putx(&o, d->addr[k], 2);
            if (k)
                putc_(&o, ':');
        }
        puts_(&o, "  ");
        if (d->has_rssi)
        {
            puts_(&o, "rssi ");
            putd(&o, d->rssi);
        }
        else
            puts_(&o, "rssi ?");
        puts_(&o, "  ");
        puts_(&o, class_word(d->cls));
        puts_(&o, "  ");
        if (d->name_state == 1)
            puts_(&o, d->name);
        else
            puts_(&o, d->name_state == 3 ? "(no name)" : "(name?)");
    }
    buf[o.n] = 0;
    if (o.cut)
        return -1; /* it was cut: the caller wants whole lines */
    return (int)o.n;
}
