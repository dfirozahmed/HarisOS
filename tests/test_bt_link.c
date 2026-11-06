/* Host test of the Bluetooth ACL link and L2CAP signalling (kernel/bluetooth/bt_link.c, and the ACL part of bt_hci.c) against a fake chip
 * and a fake device that answers on the signalling channel. */
#include <stdio.h>
#include <string.h>

#include "bluetooth.h"

static int failures, checks;
#define CHECK(c)                                                  \
    do                                                            \
    {                                                             \
        checks++;                                                 \
        if (!(c))                                                 \
        {                                                         \
            failures++;                                           \
            printf("FAIL line %d: %s\n", __LINE__, #c);           \
        }                                                         \
    } while (0)

#define MTU 20   /* the fake chip takes small ACL packets: long PDUs must be cut */
#define PKTS 2   /* ... and holds only two of them */
#define HANDLE 0x0123

/* ---- the fake chip ---- */
typedef struct
{
    unsigned handle, pb, len;
    unsigned char data[1100];
} AclCap;

typedef struct
{
    unsigned len;
    unsigned char data[400];
} PduCap;

static struct
{
    unsigned sent[64];     /* HCI command opcodes, in order */
    unsigned char par[64][16]; /* the first parameter bytes of each */
    unsigned npar[64];
    int nsent;
    int create_status;     /* status in the Command Status of Create Connection */
    int auto_credit;       /* report Number of Completed Packets for every ACL packet sent */
    unsigned unreported;
    AclCap acl[64];
    int nacl;
    unsigned char pend[270];
    int have;
    unsigned long long clock;
} F;

static void cc(unsigned op, const unsigned char *ret, int rl)
{
    F.pend[0] = 0x0E;
    F.pend[1] = (unsigned char)(3 + rl);
    F.pend[2] = 1;
    F.pend[3] = (unsigned char)op;
    F.pend[4] = (unsigned char)(op >> 8);
    memcpy(F.pend + 5, ret, (size_t)rl);
    F.have = 5 + rl;
}

static void cs(unsigned op, int status)
{
    F.pend[0] = 0x0F;
    F.pend[1] = 4;
    F.pend[2] = (unsigned char)status;
    F.pend[3] = 1;
    F.pend[4] = (unsigned char)op;
    F.pend[5] = (unsigned char)(op >> 8);
    F.have = 6;
}

static int fake_send(void *ctx, const unsigned char *pkt, unsigned len)
{
    (void)ctx;
    unsigned op = pkt[0] | ((unsigned)pkt[1] << 8);
    if (F.nsent < 64)
    {
        F.sent[F.nsent] = op;
        F.npar[F.nsent] = pkt[2] < 16 ? pkt[2] : 16;
        memcpy(F.par[F.nsent], pkt + 3, F.npar[F.nsent]);
        F.nsent++;
    }
    (void)len;
    unsigned char ok = 0;
    switch (op)
    {
    case 0x1001:
    {
        const unsigned char r[9] = {0, 8, 0x0C, 0, 8, 0x5D, 0, 0x21, 0x88};
        cc(op, r, 9);
        break;
    }
    case 0x1005:
    {
        const unsigned char r[8] = {0, MTU, 0, 0x40, PKTS, 0, 4, 0};
        cc(op, r, 8);
        break;
    }
    case 0x1009:
    {
        const unsigned char r[7] = {0, 1, 2, 3, 4, 5, 6};
        cc(op, r, 7);
        break;
    }
    case 0x0405:
        cs(op, F.create_status);
        break;
    case 0x0401: /* Inquiry */
        cs(op, 0);
        break;
    case 0x0406:
        cs(op, 0);
        break;
    case 0x0408:
    case 0x040C:
    case 0x040E:
        cc(op, &ok, 1);
        break;
    case 0x0C03:
        cc(op, &ok, 1);
        break;
    default:
    {
        unsigned char bad = 1;
        cc(op, &bad, 1);
    }
    }
    return 0;
}

static int fake_send_acl(void *ctx, const unsigned char *pkt, unsigned len)
{
    (void)ctx;
    if (F.nacl < 64)
    {
        AclCap *a = &F.acl[F.nacl++];
        a->handle = (pkt[0] | ((unsigned)pkt[1] << 8)) & 0x0FFF;
        a->pb = ((pkt[1] >> 4) & 3);
        a->len = pkt[2] | ((unsigned)pkt[3] << 8);
        if (a->len + 4 != len || a->len > MTU)
            return -1; /* the host must keep to the chip's packet size */
        memcpy(a->data, pkt + 4, a->len);
    }
    F.unreported++;
    return 0;
}

static void push(const unsigned char *e) { bt_rx_event(e, (unsigned)e[1] + 2u); }

static void fake_wait(void *ctx)
{
    (void)ctx;
    if (F.have)
    {
        bt_rx_event(F.pend, (unsigned)F.have);
        F.have = 0;
    }
    if (F.auto_credit && F.unreported)
    {
        unsigned char e[7] = {0x13, 5, 1, HANDLE & 0xFF, HANDLE >> 8, (unsigned char)F.unreported, (unsigned char)(F.unreported >> 8)};
        F.unreported = 0;
        push(e);
    }
}

static int fake_alive(void *ctx)
{
    (void)ctx;
    return 1;
}

static unsigned long long fake_now(void *ctx)
{
    (void)ctx;
    return F.clock;
}

static const BtTransport T = {fake_send, fake_wait, fake_alive, 0, fake_now, fake_send_acl};

static void fresh(void)
{
    bt_detach();
    memset(&F, 0, sizeof F);
    F.auto_credit = 1;
    F.clock = 5000;
    bt_attach(&T, 0x0BDA, 0xB00A);
}

/* ---- what the host sent, as L2CAP PDUs ---- */
static PduCap PDU[40];
static int NPDU;

static void collect(void)
{
    NPDU = 0;
    int open = 0;
    for (int i = 0; i < F.nacl; i++)
    {
        const AclCap *a = &F.acl[i];
        if (a->pb == 0)
        {
            if (NPDU >= 40)
                return;
            PDU[NPDU].len = 0;
            NPDU++;
            open = 1;
        }
        if (!open)
            continue;
        PduCap *p = &PDU[NPDU - 1];
        memcpy(p->data + p->len, a->data, a->len);
        p->len += a->len;
    }
}

static unsigned le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }

/* the signalling command number n of the PDU i: returns its code, fills id and data pointer */
static int sig_of(int i, unsigned *id, const unsigned char **data, unsigned *dlen)
{
    if (i >= NPDU || PDU[i].len < 8 || le16(PDU[i].data) + 4u != PDU[i].len || le16(PDU[i].data + 2) != 1)
        return -1;
    *id = PDU[i].data[5];
    *dlen = le16(PDU[i].data + 6);
    *data = PDU[i].data + 8;
    return PDU[i].data[4];
}

/* ---- the device speaking: ACL packets to the host, cut into pieces like USB packets ---- */
static void to_host_frag(unsigned handle, unsigned pb, const unsigned char *d, unsigned n, unsigned usb_piece)
{
    unsigned char pkt[1100];
    pkt[0] = (unsigned char)handle;
    pkt[1] = (unsigned char)(((handle >> 8) & 0x0F) | (pb << 4));
    pkt[2] = (unsigned char)n;
    pkt[3] = (unsigned char)(n >> 8);
    memcpy(pkt + 4, d, n);
    for (unsigned off = 0; off < n + 4; off += usb_piece)
    {
        unsigned c = n + 4 - off < usb_piece ? n + 4 - off : usb_piece;
        bt_rx_acl_chunk(pkt + off, c);
    }
}

/* one signalling command from the device (cid 1) as a single ACL packet */
static void dev_sig(unsigned code, unsigned id, const unsigned char *d, unsigned n)
{
    unsigned char pdu[100];
    pdu[0] = (unsigned char)(4 + n);
    pdu[1] = 0;
    pdu[2] = 1;
    pdu[3] = 0;
    pdu[4] = (unsigned char)code;
    pdu[5] = (unsigned char)id;
    pdu[6] = (unsigned char)n;
    pdu[7] = 0;
    memcpy(pdu + 8, d, n);
    to_host_frag(HANDLE, 2, pdu, 8 + n, 64);
}

static void ev_conn_complete(int status, unsigned handle, const unsigned char *addr_lsb)
{
    unsigned char e[2 + 11] = {0x03, 11, (unsigned char)status, (unsigned char)handle, (unsigned char)(handle >> 8)};
    memcpy(e + 5, addr_lsb, 6);
    e[11] = 1; /* ACL */
    e[12] = 0;
    push(e);
}

static void ev_disc_complete(unsigned handle, int reason)
{
    unsigned char e[2 + 4] = {0x05, 4, 0, (unsigned char)handle, (unsigned char)(handle >> 8), (unsigned char)reason};
    push(e);
}

static const unsigned char ADDR[6] = {0xd3, 0x04, 0xdf, 0x0c, 0x47, 0x41}; /* as people write it */
static const unsigned char ADDR_LSB[6] = {0x41, 0x47, 0x0c, 0xdf, 0x04, 0xd3};

static int line_is(unsigned i, const char *want)
{
    char buf[256];
    int n = bt_link_line(i, buf, sizeof buf);
    if (n < 0 || strcmp(buf, want) != 0)
    {
        printf("  line %u: got \"%s\" (%d), want \"%s\"\n", i, n < 0 ? "-" : buf, n, want);
        return 0;
    }
    return n == (int)strlen(want);
}

static int has_cmd(unsigned op)
{
    for (int i = 0; i < F.nsent; i++)
        if (F.sent[i] == op)
            return 1;
    return 0;
}

static int last_cmd(void) { return F.nsent ? (int)F.sent[F.nsent - 1] : -1; }

/* connect and bring the link up */
static void up(void)
{
    fresh();
    CHECK(bt_link_connect(ADDR) == 0);
    ev_conn_complete(0, HANDLE, ADDR_LSB);
    bt_poll();
}

int main(void)
{
    char buf[256];

    /* 1. no chip */
    bt_detach();
    CHECK(bt_link_connect(ADDR) == BT_E_NOT_UP);
    CHECK(bt_link_state() == BT_LINK_NONE);
    CHECK(line_is(0, "idle: no link yet"));
    CHECK(bt_link_line(4, buf, sizeof buf) == -1 && bt_link_line(0, buf, 1) == -1);

    /* 2. Read Buffer Size was read at start-up */
    fresh();
    CHECK(bt_info()->acl_mtu == MTU && bt_info()->acl_pkts == PKTS);

    /* 3. connecting: the command, then the link comes up and the probe goes out */
    CHECK(bt_link_connect(ADDR) == 0);
    CHECK(bt_link_state() == BT_LINK_CONNECTING);
    CHECK(last_cmd() == 0x0405);
    {
        const unsigned char *p = F.par[F.nsent - 1];
        CHECK(F.npar[F.nsent - 1] == 13);
        CHECK(memcmp(p, ADDR_LSB, 6) == 0);          /* address, least significant byte first */
        CHECK(p[6] == 0x18 && p[7] == 0xCC);         /* DM1 DH1 DM3 DH3 DM5 DH5 */
        CHECK(p[8] == 1 && p[9] == 0 && p[10] == 0 && p[11] == 0 && p[12] == 1); /* R1, no clock offset known, role switch allowed */
    }
    CHECK(line_is(0, "connecting to d3:04:df:0c:47:41"));
    CHECK(bt_link_connect(ADDR) == BT_E_LINK_BUSY);
    bt_poll(); /* nothing yet */
    CHECK(bt_link_state() == BT_LINK_CONNECTING);
    ev_conn_complete(0, HANDLE, ADDR_LSB);
    bt_poll();
    CHECK(bt_link_state() == BT_LINK_UP);
    CHECK(line_is(0, "connected to d3:04:df:0c:47:41, handle 0x123"));
    CHECK(F.nacl == 3); /* Echo Request + two Information Requests, in this order; two credits only, the third waited for the chip's report */
    collect();
    CHECK(NPDU == 3);
    unsigned id1 = 0, id2 = 0, id3 = 0, dl;
    const unsigned char *d;
    CHECK(sig_of(0, &id1, &d, &dl) == 0x08 && dl == 7 && memcmp(d, "HarisOS", 7) == 0);
    CHECK(sig_of(1, &id2, &d, &dl) == 0x0A && dl == 2 && le16(d) == 2);
    CHECK(sig_of(2, &id3, &d, &dl) == 0x0A && dl == 2 && le16(d) == 3);
    CHECK(id1 && id2 && id3 && id1 != id2 && id2 != id3 && id1 != id3);
    CHECK(F.acl[0].handle == HANDLE && F.acl[0].pb == 0); /* the first piece of a PDU is flagged "start" */
    CHECK(line_is(1, "echo: waiting"));
    CHECK(line_is(2, "info: waiting"));

    /* 4. the device answers; its answers come in USB sized pieces (9 bytes), the first one with flag "start, flushable" */
    F.clock += 12;
    {
        unsigned char r[7];
        memcpy(r, "HarisOS", 7);
        dev_sig(0x09, id1, r, 7);
        unsigned char x[8] = {2, 0, 0, 0, 0xA8, 0, 0, 0}; /* type 2, success, features 0x000000A8 */
        dev_sig(0x0B, id2, x, 8);
        unsigned char f[12] = {3, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0}; /* type 3, success, fixed channels: bit 1 */
        dev_sig(0x0B, id3, f, 12);
    }
    bt_poll();
    CHECK(line_is(1, "echo: answered, 7 bytes back in 12 ms"));
    CHECK(line_is(2, "info: extended features 0x000000a8, fixed channels 0x02"));
    CHECK(line_is(3, "acl: received 3 (other 0), device commands 3, link key requests 0, PIN requests 0"));

    /* 5. the device sends requests of its own: every request gets an answer, answers get none */
    int before = F.nacl;
    {
        unsigned char e[3] = {'a', 'b', 'c'};
        dev_sig(0x08, 0x31, e, 3);                       /* Echo Request */
        bt_poll(); /* the main loop polls often: the queue for ACL packets holds eight */
        unsigned char t2[2] = {2, 0}, t3[2] = {3, 0}, t9[2] = {9, 0};
        dev_sig(0x0A, 0x32, t2, 2);                      /* Information Request: extended features */
        dev_sig(0x0A, 0x33, t3, 2);                      /* ... fixed channels */
        dev_sig(0x0A, 0x34, t9, 2);                      /* ... something we do not know */
        unsigned char cr[4] = {0x19, 0x00, 0x40, 0x00};  /* Connection Request: PSM 0x19, source cid 0x40 */
        dev_sig(0x02, 0x35, cr, 4);
        unsigned char cf[4] = {0x41, 0x00, 0x00, 0x00};  /* Configuration Request for a channel we do not have */
        bt_poll();
        dev_sig(0x04, 0x36, cf, 4);
        dev_sig(0x06, 0x37, cf, 4);                      /* Disconnection Request, same */
        dev_sig(0x20, 0x38, 0, 0);                       /* a request code nobody defined */
        dev_sig(0x21, 0x39, 0, 0);                       /* an answer code nobody defined: no reply */
        dev_sig(0x03, 0x3A, cr, 4);                      /* an answer: no reply */
    }
    bt_poll();
    collect();
    CHECK(F.nacl - before == 8); /* eight replies: echo, three information, connection, configuration, disconnection, unknown request */
    {
        unsigned id;
        int k = 3; /* the first three PDUs are our probe */
        CHECK(sig_of(k, &id, &d, &dl) == 0x09 && id == 0x31 && dl == 3 && memcmp(d, "abc", 3) == 0);
        k++;
        CHECK(sig_of(k, &id, &d, &dl) == 0x0B && id == 0x32 && dl == 8 && le16(d) == 2 && le16(d + 2) == 0 && d[4] == 0 && d[7] == 0);
        k++;
        CHECK(sig_of(k, &id, &d, &dl) == 0x0B && id == 0x33 && dl == 12 && le16(d) == 3 && le16(d + 2) == 0 && d[4] == 0x02 && d[5] == 0);
        k++;
        CHECK(sig_of(k, &id, &d, &dl) == 0x0B && id == 0x34 && dl == 4 && le16(d) == 9 && le16(d + 2) == 1); /* not supported */
        k++;
        CHECK(sig_of(k, &id, &d, &dl) == 0x03 && id == 0x35 && dl == 8 && le16(d) == 0 && le16(d + 2) == 0x40 && le16(d + 4) == 2); /* PSM not supported */
        k++;
        CHECK(sig_of(k, &id, &d, &dl) == 0x01 && id == 0x36 && dl == 6 && le16(d) == 2);                                            /* invalid CID */
        k++;
        CHECK(sig_of(k, &id, &d, &dl) == 0x01 && id == 0x37 && dl == 6 && le16(d) == 2);
        k++;
        CHECK(sig_of(k, &id, &d, &dl) == 0x01 && id == 0x38 && dl == 2 && le16(d) == 0);                                            /* not understood */
        k++;
        CHECK(NPDU == k);
    }

    /* 6. two commands in one PDU, and a PDU cut into two ACL fragments ("start" then "continue") */
    before = F.nacl;
    {
        unsigned char pdu[40];
        /* header len 8+4 = 12 wait: two echo requests of 0 and 2 bytes: (4+0) + (4+2) = 10 */
        pdu[0] = 10; pdu[1] = 0; pdu[2] = 1; pdu[3] = 0;
        pdu[4] = 0x08; pdu[5] = 0x41; pdu[6] = 0; pdu[7] = 0;
        pdu[8] = 0x08; pdu[9] = 0x42; pdu[10] = 2; pdu[11] = 0; pdu[12] = 'x'; pdu[13] = 'y';
        to_host_frag(HANDLE, 2, pdu, 4 + 10, 7);                 /* one ACL packet, in 7 byte USB pieces */
        unsigned char pdu2[20];
        pdu2[0] = 8; pdu2[1] = 0; pdu2[2] = 1; pdu2[3] = 0;
        pdu2[4] = 0x08; pdu2[5] = 0x43; pdu2[6] = 4; pdu2[7] = 0; pdu2[8] = 'w'; pdu2[9] = 'x'; pdu2[10] = 'y'; pdu2[11] = 'z';
        to_host_frag(HANDLE, 2, pdu2, 6, 64);                    /* first fragment: L2CAP header and 2 bytes */
        to_host_frag(HANDLE, 1, pdu2 + 6, 6, 64);                /* continuation */
    }
    bt_poll();
    collect();
    CHECK(F.nacl - before == 3);
    {
        unsigned id;
        CHECK(sig_of(NPDU - 3, &id, &d, &dl) == 0x09 && id == 0x41 && dl == 0);
        CHECK(sig_of(NPDU - 2, &id, &d, &dl) == 0x09 && id == 0x42 && dl == 2 && memcmp(d, "xy", 2) == 0);
        CHECK(sig_of(NPDU - 1, &id, &d, &dl) == 0x09 && id == 0x43 && dl == 4 && memcmp(d, "wxyz", 4) == 0);
    }

    /* 7. things for another handle, for the wrong PB flag, a PDU that is too long: counted, not used */
    before = F.nacl;
    {
        unsigned char e[4] = {0};
        unsigned char pdu[8] = {4, 0, 1, 0, 0x08, 0x50, 0, 0};
        to_host_frag(0x0456, 2, pdu, 8, 64);   /* another link */
        to_host_frag(HANDLE, 3, pdu, 8, 64);   /* reserved flag value */
        to_host_frag(HANDLE, 1, pdu, 8, 64);   /* a continuation with nothing to continue */
        unsigned char big[8] = {0xFF, 0x7F, 1, 0, 0, 0, 0, 0}; /* a PDU of 32767 bytes: longer than we keep */
        to_host_frag(HANDLE, 2, big, 8, 64);
        to_host_frag(HANDLE, 1, e, 4, 64);
    }
    bt_poll();
    CHECK(F.nacl == before);
    CHECK(bt_link_line(3, buf, sizeof buf) > 0 && strstr(buf, "(other 1)")); /* only the packet of the other handle counts as "other" */

    /* 8. pairing requests are refused politely: the chip must not be left waiting */
    {
        unsigned char key[2 + 6] = {0x17, 6, 0x41, 0x47, 0x0c, 0xdf, 0x04, 0xd3};
        push(key);
        bt_poll();
        CHECK(last_cmd() == 0x040C && memcmp(F.par[F.nsent - 1], ADDR_LSB, 6) == 0);
        unsigned char pin[2 + 6] = {0x16, 6, 0x41, 0x47, 0x0c, 0xdf, 0x04, 0xd3};
        push(pin);
        bt_poll();
        CHECK(last_cmd() == 0x040E && memcmp(F.par[F.nsent - 1], ADDR_LSB, 6) == 0);
        CHECK(bt_link_line(3, buf, sizeof buf) > 0 && strstr(buf, "link key requests 1, PIN requests 1"));
        unsigned char auth[2 + 3] = {0x06, 3, 0x05, HANDLE & 0xFF, HANDLE >> 8}; /* Authentication Complete: failure */
        push(auth);
        bt_poll();
        CHECK(bt_link_line(3, buf, sizeof buf) > 0 && strstr(buf, "authentication 0x05"));
    }

    /* 9. we end the link */
    CHECK(bt_link_disconnect() == 0);
    bt_poll();
    CHECK(bt_link_state() == BT_LINK_CLOSING && last_cmd() == 0x0406);
    CHECK(le16(F.par[F.nsent - 1]) == HANDLE && F.par[F.nsent - 1][2] == 0x13);
    CHECK(line_is(0, "closing the link to d3:04:df:0c:47:41"));
    ev_disc_complete(HANDLE, 0x16);
    bt_poll();
    CHECK(bt_link_state() == BT_LINK_NONE);
    CHECK(line_is(0, "closed: reason 0x16 we ended it"));
    /* ACL packets after the end are not for anybody */
    {
        unsigned char r[7] = {'H', 'a', 'r', 'i', 's', 'O', 'S'};
        dev_sig(0x09, 1, r, 7);
        bt_poll();
        CHECK(bt_link_state() == BT_LINK_NONE);
    }
    /* and a new link can be made */
    CHECK(bt_link_connect(ADDR) == 0);
    CHECK(bt_link_line(0, buf, sizeof buf) > 0 && strncmp(buf, "connecting", 10) == 0);

    /* 10. the device refuses or does not answer */
    ev_conn_complete(4, 0, ADDR_LSB);
    bt_poll();
    CHECK(bt_link_state() == BT_LINK_FAILED);
    CHECK(line_is(0, "failed: status 0x04 page timeout: the device did not answer"));
    fresh();
    CHECK(bt_link_connect(ADDR) == 0);
    F.clock += 26000; /* nobody answers */
    bt_poll();
    CHECK(bt_link_state() == BT_LINK_FAILED && has_cmd(0x0408)); /* the paging is cancelled */
    CHECK(line_is(0, "failed: -2 gave up waiting"));
    /* Connection Complete of another device is not ours */
    fresh();
    CHECK(bt_link_connect(ADDR) == 0);
    {
        const unsigned char other[6] = {1, 2, 3, 4, 5, 6};
        ev_conn_complete(0, 0x77, other);
        bt_poll();
        CHECK(bt_link_state() == BT_LINK_CONNECTING);
    }
    /* we give up on purpose while paging */
    CHECK(bt_link_disconnect() == 0);
    bt_poll();
    CHECK(bt_link_state() == BT_LINK_FAILED && has_cmd(0x0408));
    CHECK(line_is(0, "failed: status 0x16 we ended it"));
    /* the chip refuses Create Connection */
    fresh();
    F.create_status = 0x0C;
    CHECK(bt_link_connect(ADDR) == 0x0C);
    CHECK(bt_link_state() == BT_LINK_FAILED && line_is(0, "failed: status 0x0c command disallowed"));
    F.create_status = 0;
    CHECK(bt_link_connect(ADDR) == 0); /* and it can be tried again */

    /* 11. the probe gets no answer, or a refusal */
    up();
    F.clock += 3500;
    bt_poll();
    CHECK(line_is(1, "echo: no answer"));
    CHECK(line_is(2, "info: extended features not reported, fixed channels not reported"));
    up();
    collect();
    {
        unsigned a = 0, b = 0, c = 0;
        sig_of(0, &a, &d, &dl);
        sig_of(1, &b, &d, &dl);
        sig_of(2, &c, &d, &dl);
        unsigned char rej[2] = {0, 0};
        dev_sig(0x01, a, rej, 2);                   /* Command Reject for the echo */
        unsigned char x[4] = {2, 0, 1, 0};          /* not supported */
        dev_sig(0x0B, b, x, 4);
        unsigned char y[4] = {3, 0, 1, 0};
        dev_sig(0x0B, c, y, 4);
    }
    bt_poll();
    CHECK(line_is(1, "echo: rejected by the device (command reject)"));
    CHECK(line_is(2, "info: extended features not reported, fixed channels not reported"));
    /* an answer with a wrong identifier is nothing */
    up();
    {
        unsigned char r[7] = {'H', 'a', 'r', 'i', 's', 'O', 'S'};
        dev_sig(0x09, 0xEE, r, 7);
        bt_poll();
        CHECK(line_is(1, "echo: waiting"));
    }

    /* 12. the device ends the link; the chip's buffers are free again */
    up();
    ev_disc_complete(HANDLE, 0x13);
    bt_poll();
    CHECK(bt_link_state() == BT_LINK_NONE && line_is(0, "closed: reason 0x13 the device ended it"));
    {
        /* another device's Disconnection Complete is not ours: bring the link up again and try */
        CHECK(bt_link_connect(ADDR) == 0);
        ev_conn_complete(0, HANDLE, ADDR_LSB);
        bt_poll();
        CHECK(bt_link_state() == BT_LINK_UP);
        ev_disc_complete(0x0999, 0x13);
        bt_poll();
        CHECK(bt_link_state() == BT_LINK_UP);
    }

    /* 12b. the handle is 12 bits; the four bits above it in the event are not part of it (the chip must not confuse us with them) */
    fresh();
    CHECK(bt_link_connect(ADDR) == 0);
    ev_conn_complete(0, 0x3000 | HANDLE, ADDR_LSB);
    bt_poll();
    CHECK(bt_link_state() == BT_LINK_UP && line_is(0, "connected to d3:04:df:0c:47:41, handle 0x123"));
    {
        collect();
        unsigned a = 0, b = 0, c = 0;
        sig_of(0, &a, &d, &dl);
        sig_of(1, &b, &d, &dl);
        sig_of(2, &c, &d, &dl);
        unsigned char r[7] = {'H', 'a', 'r', 'i', 's', 'O', 'S'};
        dev_sig(0x09, a, r, 7); /* the device uses handle 0x123 */
        bt_poll();
        CHECK(line_is(1, "echo: answered, 7 bytes back in 0 ms"));
        ev_disc_complete(0x3000 | HANDLE, 0x13);
        bt_poll();
        CHECK(bt_link_state() == BT_LINK_NONE);
    }

    /* 13. no scan and link at the same time */
    fresh();
    CHECK(bt_scan_start() == 0);
    CHECK(bt_link_connect(ADDR) == BT_E_SCANNING);

    /* 14. flow control: without credits a send waits, and gives up after a second */
    fresh();
    F.auto_credit = 0;
    {
        unsigned char pdu[44];
        for (int i = 0; i < 44; i++)
            pdu[i] = (unsigned char)i;
        pdu[0] = 40; pdu[1] = 0; pdu[2] = 1; pdu[3] = 0; /* an L2CAP PDU of 44 bytes in all */
        CHECK(bt_acl_send(HANDLE, pdu, 44) == BT_E_TIMEOUT); /* 20 + 20 fit in the two credits; the third piece has to wait */
        CHECK(F.nacl == 2);
        F.auto_credit = 1;
        F.nacl = 0;
        CHECK(bt_acl_send(HANDLE, pdu, 44) == 0);
        CHECK(F.nacl == 3);
        CHECK(F.acl[0].pb == 0 && F.acl[1].pb == 1 && F.acl[2].pb == 1);
        CHECK(F.acl[0].len == 20 && F.acl[1].len == 20 && F.acl[2].len == 4);
        unsigned char back[44];
        memcpy(back, F.acl[0].data, 20);
        memcpy(back + 20, F.acl[1].data, 20);
        memcpy(back + 40, F.acl[2].data, 4);
        CHECK(memcmp(back, pdu, 44) == 0);
        /* a PDU that fills the packets exactly: no empty piece at the end */
        F.nacl = 0;
        CHECK(bt_acl_send(HANDLE, pdu, 40) == 0 && F.nacl == 2 && F.acl[1].len == 20);
        CHECK(bt_info()->acl_tx == 7);
    }
    bt_detach();
    CHECK(bt_acl_send(HANDLE, (const unsigned char *)"xxxxxxxx", 8) == BT_E_BUSY);

    /* 15. the streams: events and ACL data arrive in USB sized pieces, in any size */
    fresh();
    {
        unsigned char ev[2 + 5] = {0x13, 5, 1, HANDLE & 0xFF, HANDLE >> 8, 1, 0}; /* one credit back: harmless here */
        unsigned before_ev = bt_info()->events;
        for (unsigned piece = 1; piece <= 7; piece++)
            for (unsigned off = 0; off < sizeof ev; off += piece)
                bt_rx_event_chunk(ev + off, sizeof ev - off < piece ? (unsigned)sizeof ev - off : piece);
        CHECK(bt_info()->events - before_ev == 7);
        unsigned char two[14];
        memcpy(two, ev, 7);
        memcpy(two + 7, ev, 7);
        bt_rx_event_chunk(two, 14);               /* two events in one piece */
        CHECK(bt_info()->events - before_ev == 9);
        bt_rx_event_chunk(0, 0);                  /* an empty piece */
        CHECK(bt_info()->events - before_ev == 9);
        unsigned char junk[300];
        memset(junk, 0xAA, sizeof junk);
        bt_rx_event_chunk(junk, 300);             /* more than any event: dropped, the stream starts again */
        bt_rx_event_chunk(ev, 7);
        CHECK(bt_info()->events - before_ev == 10);

        unsigned acl0 = bt_info()->acl_rx;
        unsigned char a[4 + 3] = {0x23, 0x21, 3, 0, 1, 2, 3};
        for (unsigned piece = 1; piece <= 7; piece++)
            for (unsigned off = 0; off < sizeof a; off += piece)
                bt_rx_acl_chunk(a + off, sizeof a - off < piece ? (unsigned)sizeof a - off : piece);
        CHECK(bt_info()->acl_rx - acl0 == 7);
        unsigned char zlp = 0;
        bt_rx_acl_chunk(&zlp, 0);                 /* a zero length packet ends a transfer, nothing more */
        CHECK(bt_info()->acl_rx - acl0 == 7);
        unsigned char bad[4] = {0x23, 0x21, 0xFF, 0xFF}; /* length 65535: nonsense */
        bt_rx_acl_chunk(bad, 4);
        bt_rx_acl_chunk(a, 7);                    /* the stream recovered */
        CHECK(bt_info()->acl_rx - acl0 == 8);
        /* more packets than the queue holds: dropped and counted, no crash */
        for (int i = 0; i < 20; i++)
            bt_rx_acl_chunk(a, 7);
        CHECK(bt_info()->acl_dropped > 0);
        unsigned char wrong[8] = {0x23, 0x21, 3, 0, 1, 2, 3, 4};
        unsigned rx = bt_info()->acl_rx;
        bt_rx_acl(wrong, 8);                      /* length field says 3 but 4 bytes follow */
        bt_rx_acl(wrong, 3);
        CHECK(bt_info()->acl_rx == rx);
    }

    printf("bt_link: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
