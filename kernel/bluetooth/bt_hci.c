/* Bluetooth HCI core: command / event exchange with the chip and the start-up sequence (stage 1). See include/bluetooth.h. */
#include "bluetooth.h"

/* HCI opcodes: OGF (6 bits) << 10 | OCF (10 bits). Core specification, volume 4 part E. */
#define OP_RESET 0x0C03            /* OGF 3 (controller & baseband), OCF 3 */
#define OP_READ_LOCAL_VERSION 0x1001 /* OGF 4 (informational parameters), OCF 1 */
#define OP_READ_BD_ADDR 0x1009     /* OGF 4, OCF 9 */
#define OP_READ_BUFFER_SIZE 0x1005 /* OGF 4, OCF 5 */
#define OP_RTL_READ_ROM_VERSION 0xFC6D /* OGF 0x3F (vendor), OCF 0x6D: Realtek, as in Linux btrtl.c */

/* HCI event codes */
#define EV_COMMAND_COMPLETE 0x0E
#define EV_COMMAND_STATUS 0x0F
#define EV_HARDWARE_ERROR 0x10
#define EV_NUM_COMPLETED_PACKETS 0x13

#define USB_VID_REALTEK 0x0BDA

#define EVQ_N 16
#define EVQ_BYTES 260 /* 2 header bytes + at most 255 parameter bytes */

typedef struct
{
    unsigned short len;
    unsigned char data[EVQ_BYTES];
} EvSlot;

#define ACLQ_N 64 /* a phone sends a file as a stream of small packets; the main loop empties the queue once per pass */
#define ACL_BYTES 1032 /* 4 header bytes + up to 1028 of data */

typedef struct
{
    unsigned short len;
    unsigned char data[ACL_BYTES];
} AclSlot;

static struct
{
    BtTransport t;
    int attached;
    BtInfo info;
    EvSlot q[EVQ_N];
    unsigned head, tail; /* free-running counters: head - tail = events waiting */
    AclSlot aq[ACLQ_N];
    unsigned ahead, atail; /* the same for received ACL packets */
    unsigned credits;      /* ACL packets we may send before the chip says it has room again */
    unsigned outstanding;  /* ACL packets sent that the chip has not reported as done */
    /* the streams from the transport, put together packet by packet */
    unsigned char sev[EVQ_BYTES];
    unsigned sev_have;
    unsigned char sacl[ACL_BYTES];
    unsigned sacl_have;
} B;

static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

void bt_rx_event(const unsigned char *evt, unsigned len)
{
    if (!B.attached)
        return;
    if (len < 2 || len > EVQ_BYTES || (unsigned)evt[1] + 2 != len) /* the length byte must agree with what arrived */
        return;
    B.info.events++;
    if (B.head - B.tail >= EVQ_N)
    {
        B.info.dropped++;
        return;
    }
    EvSlot *s = &B.q[B.head % EVQ_N];
    s->len = (unsigned short)len;
    for (unsigned i = 0; i < len; i++)
        s->data[i] = evt[i];
    B.head++;
}

void bt_rx_event_chunk(const unsigned char *data, unsigned len)
{
    if (!B.attached || len == 0)
        return;
    if (B.sev_have + len > EVQ_BYTES) /* cannot be one event: start again with this piece */
    {
        B.sev_have = 0;
        if (len > EVQ_BYTES)
            return;
    }
    for (unsigned i = 0; i < len; i++)
        B.sev[B.sev_have++] = data[i];
    while (B.sev_have >= 2 && B.sev_have >= 2u + B.sev[1]) /* header (code, length) + parameters complete */
    {
        unsigned need = 2u + B.sev[1];
        bt_rx_event(B.sev, need);
        B.sev_have -= need;
        for (unsigned i = 0; i < B.sev_have; i++)
            B.sev[i] = B.sev[need + i];
    }
}

void bt_rx_acl(const unsigned char *pkt, unsigned len)
{
    if (!B.attached)
        return;
    if (len < 4 || len > ACL_BYTES || le16(pkt + 2) + 4u != len) /* the length field must agree with what arrived */
        return;
    B.info.acl_rx++;
    if (B.ahead - B.atail >= ACLQ_N)
    {
        B.info.acl_dropped++;
        return;
    }
    AclSlot *s = &B.aq[B.ahead % ACLQ_N];
    s->len = (unsigned short)len;
    for (unsigned i = 0; i < len; i++)
        s->data[i] = pkt[i];
    B.ahead++;
}

void bt_rx_acl_chunk(const unsigned char *data, unsigned len)
{
    if (!B.attached || len == 0) /* a zero length packet only ends a transfer */
        return;
    if (B.sacl_have + len > ACL_BYTES)
    {
        B.sacl_have = 0;
        if (len > ACL_BYTES)
            return;
    }
    for (unsigned i = 0; i < len; i++)
        B.sacl[B.sacl_have++] = data[i];
    while (B.sacl_have >= 4)
    {
        unsigned need = 4u + le16(B.sacl + 2);
        if (need > ACL_BYTES) /* nonsense header: forget what we have */
        {
            B.sacl_have = 0;
            return;
        }
        if (B.sacl_have < need)
            return;
        bt_rx_acl(B.sacl, need);
        B.sacl_have -= need;
        for (unsigned i = 0; i < B.sacl_have; i++)
            B.sacl[i] = B.sacl[need + i];
    }
}

/* An event nobody is waiting for. */
static void on_event(const unsigned char *e, unsigned len)
{
    if (e[0] == EV_HARDWARE_ERROR)
    {
        B.info.hw_errors++;
        return;
    }
    if (e[0] == EV_NUM_COMPLETED_PACKETS && e[1] >= 1)
    {
        /* number of handles (1), then the handles (2 bytes each), then how many packets of each were sent on (2 bytes each) */
        unsigned n = e[2];
        if (e[1] >= 1 + 4 * n)
            for (unsigned i = 0; i < n; i++)
            {
                unsigned done = le16(e + 3 + 2 * n + 2 * i);
                B.outstanding = done > B.outstanding ? 0 : B.outstanding - done;
                B.credits += done;
            }
        if (B.credits > B.info.acl_pkts)
            B.credits = B.info.acl_pkts;
        return;
    }
    bt_scan_event(e, len);
    bt_link_event(e, len);
}

/* ACL data the chip handed over: for the link layer. */
static void drain_acl(void)
{
    while (B.atail != B.ahead)
    {
        const AclSlot *s = &B.aq[B.atail % ACLQ_N];
        bt_link_acl(s->data, s->len);
        B.atail++;
    }
}

unsigned long long bt_now(void) { return B.attached && B.t.now_ms ? B.t.now_ms(B.t.ctx) : 0; }

int bt_poll(void)
{
    if (!B.attached || B.info.state != BT_HCI_UP)
        return 0;
    int busy = B.tail != B.head || B.atail != B.ahead; /* something came in: the caller should come back at once, not sleep */
    while (B.tail != B.head)
    {
        const EvSlot *s = &B.q[B.tail % EVQ_N];
        on_event(s->data, s->len);
        B.tail++;
    }
    drain_acl();
    bt_scan_step();
    bt_link_step();
    return busy;
}

int bt_cmd(unsigned opcode, const void *params, unsigned plen, unsigned char *ret, unsigned ret_max, unsigned *ret_len, unsigned timeout_ms)
{
    if (ret_len)
        *ret_len = 0;
    if (!B.attached)
        return BT_E_BUSY;
    if (plen > 255)
        return BT_E_SHORT;
    unsigned char pkt[3 + 255];
    pkt[0] = (unsigned char)opcode;
    pkt[1] = (unsigned char)(opcode >> 8);
    pkt[2] = (unsigned char)plen;
    const unsigned char *p = (const unsigned char *)params;
    for (unsigned i = 0; i < plen; i++)
        pkt[3 + i] = p[i];
    if (B.t.send_cmd(B.t.ctx, pkt, 3 + plen) < 0)
        return BT_E_TRANSPORT;

    for (unsigned step = 0; step < timeout_ms * 10u; step++)
    {
        while (B.tail != B.head)
        {
            const EvSlot *s = &B.q[B.tail % EVQ_N];
            const unsigned char *e = s->data;
            unsigned plen_ev = e[1];
            const unsigned char *par = e + 2;
            int done = 0, status = 0;
            const unsigned char *rp = 0;
            unsigned rl = 0;
            if (e[0] == EV_COMMAND_COMPLETE && plen_ev >= 3 && le16(par + 1) == opcode)
            {
                /* num_hci_command_packets (1), opcode (2), return parameters (the first one is the status) */
                rp = par + 3;
                rl = plen_ev - 3;
                status = rl ? rp[0] : BT_E_SHORT;
                done = 1;
            }
            else if (e[0] == EV_COMMAND_STATUS && plen_ev >= 4 && le16(par + 2) == opcode)
            {
                /* status (1), num_hci_command_packets (1), opcode (2): the command was understood, the result comes later */
                rp = par;
                rl = 1;
                status = par[0];
                done = 1;
            }
            if (done)
            {
                if (ret && rp)
                {
                    unsigned n = rl < ret_max ? rl : ret_max;
                    for (unsigned i = 0; i < n; i++)
                        ret[i] = rp[i];
                    if (ret_len)
                        *ret_len = n;
                }
                B.tail++;
                return status;
            }
            on_event(e, s->len);
            B.tail++;
        }
        drain_acl();
        if (!B.t.alive(B.t.ctx))
            return BT_E_TRANSPORT;
        B.t.wait_step(B.t.ctx);
    }
    return BT_E_TIMEOUT;
}

unsigned bt_acl_credits(void) { return B.credits; }

void bt_acl_reset_credits(void)
{
    B.credits = B.info.acl_pkts;
    B.outstanding = 0;
}

int bt_acl_send(unsigned handle, const unsigned char *pdu, unsigned len)
{
    if (!B.attached || !B.t.send_acl || B.info.acl_mtu == 0)
        return BT_E_BUSY;
    unsigned char pkt[4 + 1024];
    unsigned mtu = B.info.acl_mtu > 1024 ? 1024 : B.info.acl_mtu;
    unsigned off = 0;
    int first = 1;
    do
    {
        unsigned n = len - off < mtu ? len - off : mtu;
        /* room in the chip's buffers? The Number of Completed Packets event gives credits back. */
        unsigned waited = 0;
        while (B.credits == 0)
        {
            while (B.tail != B.head) /* only events: they carry the credits */
            {
                const EvSlot *s = &B.q[B.tail % EVQ_N];
                on_event(s->data, s->len);
                B.tail++;
            }
            if (B.credits)
                break;
            if (!B.t.alive(B.t.ctx))
                return BT_E_TRANSPORT;
            if (++waited > 2000) /* 200 ms: the caller (bt_poll) may try again later. After a few pieces of a PDU went out the caller cannot. */
                return off == 0 ? BT_E_TIMEOUT : BT_E_PARTIAL;
            B.t.wait_step(B.t.ctx);
        }
        /* handle (12 bits) and flags: packet boundary 0 = first piece of a PDU, 1 = continuation; length of the data */
        pkt[0] = (unsigned char)handle;
        pkt[1] = (unsigned char)(((handle >> 8) & 0x0F) | ((first ? 0u : 1u) << 4));
        pkt[2] = (unsigned char)n;
        pkt[3] = (unsigned char)(n >> 8);
        for (unsigned i = 0; i < n; i++)
            pkt[4 + i] = pdu[off + i];
        if (B.t.send_acl(B.t.ctx, pkt, 4 + n) < 0)
            return BT_E_TRANSPORT;
        B.credits--;
        B.outstanding++;
        B.info.acl_tx++;
        off += n;
        first = 0;
    } while (off < len);
    return 0;
}

static void fail(const char *why, int code)
{
    B.info.state = BT_FAILED;
    B.info.why = why;
    B.info.why_code = code;
}

static void start(void)
{
    unsigned short vid = B.info.vid;
    unsigned char r[16];
    bt_scan_reset(); /* a reset puts the chip's settings back to the defaults */
    bt_link_reset();   /* ... and ends every link */
    B.sev_have = B.sacl_have = 0;
    B.ahead = B.atail = 0;
    unsigned n;
    int st;

    /* 1. Reset: the chip returns to its default state. Can take a while on a chip that has just powered up. */
    st = bt_cmd(OP_RESET, 0, 0, r, sizeof r, &n, 2000);
    if (st != 0)
    {
        fail("reset", st);
        return;
    }

    /* 2. Read Local Version Information: status, HCI version (1), HCI revision (2), LMP version (1), manufacturer (2), LMP subversion (2) */
    st = bt_cmd(OP_READ_LOCAL_VERSION, 0, 0, r, sizeof r, &n, 1000);
    if (st != 0 || n < 9)
    {
        fail("version", st != 0 ? st : BT_E_SHORT);
        return;
    }
    B.info.hci_ver = r[1];
    B.info.hci_rev = (unsigned short)le16(r + 2);
    B.info.lmp_ver = r[4];
    B.info.manufacturer = (unsigned short)le16(r + 5);
    B.info.lmp_subver = (unsigned short)le16(r + 7);

    /* 3. Realtek: which ROM the chip has (the firmware file to load depends on it). Answer: status, version (1). A failure here is
     * not fatal: the chip just does not know the vendor command. */
    if (vid == USB_VID_REALTEK)
    {
        st = bt_cmd(OP_RTL_READ_ROM_VERSION, 0, 0, r, sizeof r, &n, 1000);
        if (st == 0 && n >= 2)
        {
            B.info.have_rom = 1;
            B.info.rom_version = r[1];
        }
    }

    /* 4. Read BD_ADDR: status, address (6 bytes, least significant first). Not fatal either. */
    st = bt_cmd(OP_READ_BD_ADDR, 0, 0, r, sizeof r, &n, 1000);
    if (st == 0 && n >= 7)
    {
        for (int i = 0; i < 6; i++)
            B.info.bdaddr[i] = r[6 - i];
        B.info.have_addr = 1;
    }

    /* 5. Read Buffer Size: status, largest ACL packet (2), largest SCO packet (1), ACL packets the chip holds (2), SCO packets (2).
     * The packet count is our number of credits for sending. */
    B.info.acl_mtu = B.info.acl_pkts = 0;
    st = bt_cmd(OP_READ_BUFFER_SIZE, 0, 0, r, sizeof r, &n, 1000);
    if (st == 0 && n >= 8)
    {
        B.info.acl_mtu = (unsigned short)le16(r + 1);
        B.info.acl_pkts = (unsigned short)le16(r + 4);
    }
    bt_acl_reset_credits();

    B.info.state = BT_HCI_UP;
}

void bt_attach(const BtTransport *t, unsigned short vid, unsigned short pid)
{
    B.t = *t;
    B.head = B.tail = 0;
    unsigned char *z = (unsigned char *)&B.info; /* no "= {0}": it could turn into a memset call (no libc) */
    for (unsigned i = 0; i < sizeof B.info; i++)
        z[i] = 0;
    B.info.vid = vid;
    B.info.pid = pid;
    B.info.state = BT_INIT;
    B.attached = 1;
    start();
}

void bt_restart(void)
{
    if (!B.attached)
        return;
    B.info.state = BT_INIT;
    B.info.have_rom = B.info.have_addr = 0;
    B.info.why = 0;
    B.info.why_code = 0;
    start();
}

void bt_fw_report(BtFwState state, const char *why, int code, unsigned fw_version, unsigned done, unsigned total)
{
    B.info.fw_state = state;
    B.info.fw_why = why;
    B.info.fw_code = code;
    B.info.fw_version = fw_version;
    B.info.fw_done = done;
    B.info.fw_total = total;
}

void bt_detach(void)
{
    bt_scan_reset();
    bt_link_reset();
    B.attached = 0;
    B.info.state = BT_ABSENT;
}

const BtInfo *bt_info(void) { return &B.info; }

int bt_hci_up(void) { return B.attached && B.info.state == BT_HCI_UP; }

/* ---- the status line ---- */

typedef struct
{
    char *p;
    unsigned cap, n;
} Out;

static void putc_(Out *o, char c)
{
    if (o->n + 1 < o->cap)
        o->p[o->n++] = c;
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

static void putd(Out *o, unsigned v)
{
    char t[10];
    int k = 0;
    do
    {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k < 10);
    while (k)
        putc_(o, t[--k]);
}

void bt_note_cmd_error(int code)
{
    B.info.cmd_errors++;
    B.info.last_cmd_code = code;
}

void bt_status(char *buf, unsigned cap)
{
    if (!cap)
        return;
    Out o = {buf, cap, 0};
    const BtInfo *i = &B.info;
    if (!B.attached || i->state == BT_ABSENT)
    {
        puts_(&o, "no Bluetooth chip");
        buf[o.n] = 0;
        return;
    }
    puts_(&o, "BT ");
    putx(&o, i->vid, 4);
    putc_(&o, ':');
    putx(&o, i->pid, 4);
    if (i->state == BT_FAILED)
    {
        puts_(&o, ": ");
        puts_(&o, i->why ? i->why : "failed");
        puts_(&o, " failed (");
        if (i->why_code < 0)
        {
            putc_(&o, '-');
            putd(&o, (unsigned)-i->why_code);
        }
        else
        {
            putx(&o, (unsigned)i->why_code, 2);
        }
        putc_(&o, ')');
    }
    else
    {
        puts_(&o, " hci ");
        putd(&o, i->hci_ver);
        puts_(&o, " rev ");
        putx(&o, i->hci_rev, 4);
        puts_(&o, " sub ");
        putx(&o, i->lmp_subver, 4);
        puts_(&o, " mfr ");
        putd(&o, i->manufacturer);
        if (i->have_rom)
        {
            puts_(&o, " rom ");
            putd(&o, i->rom_version);
        }
        if (i->have_rom || i->have_addr)
            putc_(&o, ',');
        if (i->have_addr)
        {
            puts_(&o, " addr ");
            for (int k = 0; k < 6; k++)
            {
                if (k)
                    putc_(&o, ':');
                putx(&o, i->bdaddr[k], 2);
            }
        }
        switch (i->fw_state)
        {
        case BT_FW_LOADED:
            puts_(&o, " fw ok ");
            putx(&o, i->fw_version, 8);
            break;
        case BT_FW_MISSING:
            puts_(&o, " fw: files missing");
            break;
        case BT_FW_BAD:
            puts_(&o, " fw bad: ");
            puts_(&o, i->fw_why ? i->fw_why : "?");
            break;
        case BT_FW_FAILED:
            puts_(&o, " fw failed ");
            putd(&o, i->fw_done);
            putc_(&o, '/');
            putd(&o, i->fw_total);
            puts_(&o, " st ");
            if (i->fw_code < 0)
            {
                putc_(&o, '-');
                putd(&o, (unsigned)-i->fw_code);
            }
            else
                putx(&o, (unsigned)i->fw_code, 2);
            break;
        default:
            break;
        }
        if (i->state == BT_INIT)
            puts_(&o, " (starting)");
    }
    buf[o.n] = 0;
}

int bt_diag_line(char *buf, unsigned cap)
{
    if (cap < 2)
        return -1;
    char st[100];
    bt_status(st, sizeof st);
    Out o = {buf, cap, 0};
    puts_(&o, "chip: ");
    puts_(&o, st);
    if (B.attached)
    {
        puts_(&o, "; usb ");
        puts_(&o, B.t.alive(B.t.ctx) ? "up" : "DOWN");
        puts_(&o, ", command errors ");
        putd(&o, B.info.cmd_errors);
        puts_(&o, " (last code ");
        if (B.info.last_cmd_code < 0)
        {
            putc_(&o, '-');
            putd(&o, (unsigned)-B.info.last_cmd_code);
        }
        else
            putd(&o, (unsigned)B.info.last_cmd_code);
        puts_(&o, "), events dropped ");
        putd(&o, B.info.dropped);
        puts_(&o, ", hardware errors ");
        putd(&o, B.info.hw_errors);
    }
    if (o.n + 1 >= cap && cap > 1)
        o.n = cap - 1;
    buf[o.n] = 0;
    return (int)o.n;
}
