/* Bluetooth HCI core: command / event exchange with the chip and the start-up sequence (stage 1). See include/bluetooth.h. */
#include "bluetooth.h"

/* HCI opcodes: OGF (6 bits) << 10 | OCF (10 bits). Core specification, volume 4 part E. */
#define OP_RESET 0x0C03            /* OGF 3 (controller & baseband), OCF 3 */
#define OP_READ_LOCAL_VERSION 0x1001 /* OGF 4 (informational parameters), OCF 1 */
#define OP_READ_BD_ADDR 0x1009     /* OGF 4, OCF 9 */
#define OP_RTL_READ_ROM_VERSION 0xFC6D /* OGF 0x3F (vendor), OCF 0x6D: Realtek, as in Linux btrtl.c */

/* HCI event codes */
#define EV_COMMAND_COMPLETE 0x0E
#define EV_COMMAND_STATUS 0x0F
#define EV_HARDWARE_ERROR 0x10

#define USB_VID_REALTEK 0x0BDA

#define EVQ_N 16
#define EVQ_BYTES 260 /* 2 header bytes + at most 255 parameter bytes */

typedef struct
{
    unsigned short len;
    unsigned char data[EVQ_BYTES];
} EvSlot;

static struct
{
    BtTransport t;
    int attached;
    BtInfo info;
    EvSlot q[EVQ_N];
    unsigned head, tail; /* free-running counters: head - tail = events waiting */
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

/* An event nobody is waiting for. Later stages (inquiry results, connections, ...) hook in here. */
static void on_event(const unsigned char *e, unsigned len)
{
    (void)len;
    if (e[0] == EV_HARDWARE_ERROR)
        B.info.hw_errors++;
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
        if (!B.t.alive(B.t.ctx))
            return BT_E_TRANSPORT;
        B.t.wait_step(B.t.ctx);
    }
    return BT_E_TIMEOUT;
}

static void fail(const char *why, int code)
{
    B.info.state = BT_FAILED;
    B.info.why = why;
    B.info.why_code = code;
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

    unsigned char r[16];
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

    B.info.state = BT_HCI_UP;
}

void bt_detach(void)
{
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
        if (i->state == BT_INIT)
            puts_(&o, " (starting)");
    }
    buf[o.n] = 0;
}
