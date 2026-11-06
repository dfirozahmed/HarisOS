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
    unsigned char data[1100];
} PduCap;

static struct
{
    unsigned sent[64];     /* HCI command opcodes, in order */
    unsigned char par[64][32]; /* the first parameter bytes of each */
    unsigned npar[64];
    int nsent;
    int create_status;     /* status in the Command Status of Create Connection */
    int auth_status;       /* ... of Authentication Requested and Set Connection Encryption */
    int auto_credit;       /* report Number of Completed Packets for every ACL packet sent */
    unsigned unreported;
    AclCap acl[700];
    unsigned mtu;          /* ACL packet size the chip says it takes (Read Buffer Size) */
    int acl_fail;          /* the chip refuses ACL packets */
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
        F.npar[F.nsent] = pkt[2] < 32 ? pkt[2] : 32;
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
        const unsigned char r[8] = {0, (unsigned char)F.mtu, (unsigned char)(F.mtu >> 8), 0x40, PKTS, 0, 4, 0};
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
    case 0x0411: /* Authentication Requested */
    case 0x0413: /* Set Connection Encryption */
        cs(op, F.auth_status);
        break;
    case 0x0C01:
    case 0x0C13:
    case 0x0C56:
    case 0x040B:
    case 0x040D:
    case 0x042B:
    case 0x042C:
    case 0x042D:
    case 0x0434:
        cc(op, &ok, 1);
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
    if (F.acl_fail)
        return -1;
    if (F.nacl < 700)
    {
        AclCap *a = &F.acl[F.nacl++];
        a->handle = (pkt[0] | ((unsigned)pkt[1] << 8)) & 0x0FFF;
        a->pb = ((pkt[1] >> 4) & 3);
        a->len = pkt[2] | ((unsigned)pkt[3] << 8);
        if (a->len + 4 != len || a->len > F.mtu)
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

static unsigned NEXT_MTU = MTU; /* the packet size of the next fake chip */

static void fresh(void)
{
    bt_detach();
    memset(&F, 0, sizeof F);
    F.auto_credit = 1;
    F.clock = 5000;
    F.mtu = NEXT_MTU;
    bt_attach(&T, 0x0BDA, 0xB00A);
}

/* ---- what the host sent, as L2CAP PDUs ---- */
static PduCap PDU[700];
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
            if (NPDU >= 700)
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


/* an HCI event with these parameters, from the chip */
static void ev6(unsigned code, const unsigned char *par, unsigned n)
{
    unsigned char e[2 + 40];
    e[0] = (unsigned char)code;
    e[1] = (unsigned char)n;
    memcpy(e + 2, par, n);
    push(e);
}

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


/* ---- channels and SDP ---- */
#include "bt_sdp_fixture.h"

#define DEV_CID 0x0077 /* the device's end of the channel we open */

/* the last signalling command with this code that the host sent */
static int last_sig(unsigned code, unsigned *id, const unsigned char **d, unsigned *dl)
{
    collect();
    for (int i = NPDU - 1; i >= 0; i--)
        if (sig_of(i, id, d, dl) == (int)code)
            return 1;
    return 0;
}

/* the last PDU the host sent to this channel number (not the signalling channel): its data after the L2CAP header */
static int last_data(unsigned cid, const unsigned char **d, unsigned *dl)
{
    collect();
    for (int i = NPDU - 1; i >= 0; i--)
        if (PDU[i].len >= 4 && le16(PDU[i].data + 2) == cid)
        {
            *d = PDU[i].data + 4;
            *dl = PDU[i].len - 4;
            return 1;
        }
    return 0;
}

/* the device sends an L2CAP PDU on a channel (our end `cid`) */
static void dev_pdu(unsigned cid, const unsigned char *data, unsigned n)
{
    unsigned char pdu[1100];
    pdu[0] = (unsigned char)n;
    pdu[1] = (unsigned char)(n >> 8);
    pdu[2] = (unsigned char)cid;
    pdu[3] = (unsigned char)(cid >> 8);
    memcpy(pdu + 4, data, n);
    to_host_frag(HANDLE, 2, pdu, 4 + n, 64);
}

/* the device's Connection Response */
static void dev_conn_rsp(unsigned id, unsigned result)
{
    unsigned char r[8];
    r[0] = result == 0 || result == 1 ? DEV_CID & 0xFF : 0;
    r[1] = result == 0 || result == 1 ? DEV_CID >> 8 : 0;
    r[2] = 0x40;
    r[3] = 0;
    r[4] = (unsigned char)result;
    r[5] = 0;
    r[6] = r[7] = 0;
    dev_sig(0x03, id, r, 8);
}

/* the device's answer to an SDP request: this piece of the attribute lists and a continuation state (cl bytes) */
static void dev_sdp_answer(unsigned tid, const unsigned char *piece, unsigned n, const unsigned char *cont, unsigned cl)
{
    unsigned char b[1000];
    unsigned plen = 2 + n + 1 + cl;
    b[0] = 0x07;
    b[1] = (unsigned char)(tid >> 8);
    b[2] = (unsigned char)tid;
    b[3] = (unsigned char)(plen >> 8);
    b[4] = (unsigned char)plen;
    b[5] = (unsigned char)(n >> 8);
    b[6] = (unsigned char)n;
    memcpy(b + 7, piece, n);
    b[7 + n] = (unsigned char)cl;
    if (cl)
        memcpy(b + 8 + n, cont, cl);
    dev_pdu(0x40, b, 8 + n + cl);
}

/* a link, an SDP browse, and what the host sent for it: the id of its Connection Request */
static unsigned sdp_open_start(void)
{
    up();
    CHECK(bt_sdp_browse() == 0);
    bt_poll();
    unsigned id = 0, dl = 0;
    const unsigned char *d = 0;
    CHECK(last_sig(0x02, &id, &d, &dl) && dl == 4 && le16(d) == 1 && le16(d + 2) == 0x40); /* PSM 1 (SDP), our end 0x40 */
    return id;
}

/* the device accepts and configures; ends when the channel is open and the request has gone out. Returns the id of our Configuration Request. */
static unsigned sdp_open_finish(unsigned conn_id)
{
    dev_conn_rsp(conn_id, 0);
    bt_poll();
    unsigned cfg_id = 0, dl = 0;
    const unsigned char *d = 0;
    CHECK(last_sig(0x04, &cfg_id, &d, &dl) && dl == 4 && le16(d) == DEV_CID);
    unsigned char cq[8] = {0x40, 0, 0, 0, 1, 2, 0xA0, 0x02};
    dev_sig(0x04, 0x50, cq, 8);
    bt_poll();
    unsigned char cr[6] = {0x40, 0, 0, 0, 0, 0}; /* the answer names OUR end of the channel (0x40), as Linux and the real headset do */
    dev_sig(0x05, cfg_id, cr, 6);
    bt_poll();
    return cfg_id;
}


/* ---- A2DP / AVDTP ---- */
#define SIG_CID_DEV 0x0077   /* the device's end of the signalling channel; ours is 0x40 (the first slot) */
#define MEDIA_CID_DEV 0x0078 /* ... of the media channel; ours is 0x41 */

/* an AVDTP message from the device to our end `ours` of a channel */
static void dev_av(unsigned ours, unsigned label, unsigned mtype, unsigned sig, const unsigned char *par, unsigned n)
{
    unsigned char b[40];
    b[0] = (unsigned char)((label << 4) | mtype);
    b[1] = (unsigned char)sig;
    if (n)
        memcpy(b + 2, par, n);
    dev_pdu(ours, b, 2 + n);
}

/* the last AVDTP message of the host to the device's end `dcid`: 1 and its parts if it was a command */
static int last_av(unsigned dcid, unsigned *label, unsigned *sig, const unsigned char **par, unsigned *pn)
{
    const unsigned char *d;
    unsigned dl;
    if (!last_data(dcid, &d, &dl) || dl < 2)
        return 0;
    *label = d[0] >> 4;
    *sig = d[1] & 0x3F;
    *par = d + 2;
    *pn = dl - 2;
    return (d[0] & 3) == 0;
}

/* a channel to the device: its Connection Response, its Configuration Request (for our end), its answer to ours */
static unsigned HS_MTU = 672; /* the MTU the device asks for in its Configuration Request: what we may send in one PDU */

static void chan_handshake(unsigned our_cid, unsigned dev_cid)
{
    unsigned id = 0, dl = 0;
    const unsigned char *d = 0;
    CHECK(last_sig(0x02, &id, &d, &dl) && dl == 4 && le16(d) == 0x19 && le16(d + 2) == our_cid); /* Connection Request for AVDTP (PSM 0x19) */
    unsigned char r[8] = {(unsigned char)dev_cid, (unsigned char)(dev_cid >> 8), (unsigned char)our_cid, (unsigned char)(our_cid >> 8), 0, 0, 0, 0};
    dev_sig(0x03, id, r, 8);
    bt_poll();
    unsigned cfg_id = 0;
    CHECK(last_sig(0x04, &cfg_id, &d, &dl) && le16(d) == dev_cid);
    unsigned char cq[8] = {(unsigned char)our_cid, (unsigned char)(our_cid >> 8), 0, 0, 1, 2, (unsigned char)HS_MTU, (unsigned char)(HS_MTU >> 8)};
    dev_sig(0x04, 0x50 + (our_cid & 0x0F), cq, 8);
    bt_poll();
    unsigned char cr[6] = {(unsigned char)our_cid, (unsigned char)(our_cid >> 8), 0, 0, 0, 0};
    dev_sig(0x05, cfg_id, cr, 6);
    bt_poll();
}

/* a link, the A2DP sequence started, the signalling channel open: the host has sent Discover */
static void av_open_sig(void)
{
    up();
    CHECK(bt_avdtp_start() == 0);
    CHECK(line_is(0xA0, "a2dp: opening the signalling channel"));
    bt_poll();
    chan_handshake(0x40, SIG_CID_DEV);
}

/* the device's usual answers */
static const unsigned char SEPS[] = {0x04, 0x00, 0x0A, 0x08, 0x0C, 0x08}; /* 1: source, free. 2: sink, in use. 3: sink, free */
static const unsigned char HCAPS[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3F, 0xFF, 0x02, 0x35, 0x08, 0x00};

/* answer the last command of the host with Accept and these parameters */
static void av_accept(unsigned dcid, unsigned sig_expected, const unsigned char *par, unsigned n)
{
    unsigned label = 0, sig = 0, pn = 0;
    const unsigned char *p = 0;
    CHECK(last_av(dcid, &label, &sig, &p, &pn) && sig == sig_expected);
    dev_av(0x40, label, 2, sig, par, n);
    bt_poll();
}

/* the sequence up to a media channel being opened */
static void av_to_open(void)
{
    av_open_sig();
    unsigned label, sig, pn;
    const unsigned char *p;
    CHECK(last_av(SIG_CID_DEV, &label, &sig, &p, &pn) && sig == 0x01 && pn == 0);          /* Discover */
    CHECK(line_is(0xA0, "a2dp: asking for the stream endpoints"));
    av_accept(SIG_CID_DEV, 0x01, SEPS, sizeof SEPS);
    CHECK(line_is(0xA0, "a2dp: reading what the endpoint can do"));
    CHECK(last_av(SIG_CID_DEV, &label, &sig, &p, &pn) && sig == 0x02 && pn == 1 && p[0] == 0x0C); /* Get Capabilities of endpoint 3 */
    av_accept(SIG_CID_DEV, 0x02, HCAPS, sizeof HCAPS);
}


/* ---- A2DP audio ---- */
#include "bt_sbc_ref.h"

/* a link with a running stream (the sink offers these capabilities): everything up to the sink accepting Start */
static void av_stream_caps(const unsigned char *caps, unsigned n)
{
    av_open_sig();
    av_accept(SIG_CID_DEV, 0x01, SEPS, sizeof SEPS);
    av_accept(SIG_CID_DEV, 0x02, caps, n);
    av_accept(SIG_CID_DEV, 0x03, 0, 0);
    av_accept(SIG_CID_DEV, 0x06, 0, 0);
    chan_handshake(0x41, MEDIA_CID_DEV);
    av_accept(SIG_CID_DEV, 0x07, 0, 0);
    CHECK(bt_avdtp_streaming());
}

/* the capabilities of the real headset (Til8): bitpool 2 .. 38, so frames of 89 bytes and 7 of them in a 672 byte packet */
static void av_stream(void)
{
    static const unsigned char CAPS38[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3F, 0xFF, 0x02, 0x26};
    av_stream_caps(CAPS38, sizeof CAPS38);
}

/* the media packets sent so far: indexes into PDU[] */
static int media_idx[600];

static int media_packets(void)
{
    collect();
    int n = 0;
    for (int i = 0; i < NPDU; i++)
        if (PDU[i].len >= 4 && le16(PDU[i].data + 2) == MEDIA_CID_DEV && n < 600)
            media_idx[n++] = i;
    return n;
}

/* time passes: the clock moves in steps and the main loop polls */
static void run_ms(unsigned ms, unsigned step)
{
    for (unsigned t = 0; t < ms; t += step)
    {
        F.clock += step;
        bt_poll();
    }
}

static unsigned be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }

/* decodes every media packet into PCM (left channel) with the reference decoder; checks the RTP and payload headers on the way.
 * Returns the number of sample frames, and fills *packets and *frames. */
static short AUD[200000];

static int decode_media(int n, int *frames_total, unsigned fb)
{
    RefDec d;
    ref_init(&d);
    unsigned seq0 = 0, ts0 = 0;
    int samples = 0;
    *frames_total = 0;
    for (int k = 0; k < n; k++)
    {
        const PduCap *pd = &PDU[media_idx[k]];
        const unsigned char *p = pd->data + 4;
        unsigned plen = pd->len - 4;
        CHECK(le16(pd->data) == plen);
        CHECK(p[0] == 0x80 && p[1] == 0x60);                 /* RTP version 2, payload type 96, no marker */
        CHECK(be32(p + 8) == 0x48415249u);                  /* the same source id throughout */
        unsigned seq = ((unsigned)p[2] << 8) | p[3], ts = be32(p + 4);
        if (k == 0)
        {
            seq0 = seq;
            ts0 = ts;
            CHECK(seq == 0 && ts == 0);
        }
        else
        {
            CHECK(seq == ((seq0 + (unsigned)k) & 0xFFFF));
        }
        unsigned nf = p[12] & 15;
        CHECK((p[12] & 0xF0) == 0 && nf >= 1 && nf <= 15); /* not fragmented, a frame count */
        CHECK(plen == 13 + nf * fb);
        CHECK(ts == ts0 + (unsigned)*frames_total * 128);   /* the time stamp counts samples */
        for (unsigned f = 0; f < nf; f++)
        {
            RefFrame fi;
            CHECK(ref_decode(&d, p + 13 + f * fb, fb, AUD + samples * 2, &fi) == (int)fb && fi.crc_ok);
            samples += 128;
            (*frames_total)++;
        }
    }
    return samples;
}

/* sign changes of the decoded tune between two sample positions: twice the frequency times the time */
static int crossings(int from, int to)
{
    int c = 0;
    for (int i = from + 1; i < to; i++)
        c += (AUD[2 * (i - 1)] < 0) != (AUD[2 * i] < 0);
    return c;
}

int main(void)
{
    char buf[256];

    /* 1. no chip */
    bt_detach();
    CHECK(bt_link_connect(ADDR) == BT_E_NOT_UP);
    CHECK(bt_link_state() == BT_LINK_NONE);
    CHECK(line_is(0, "idle: no link yet"));
    CHECK(bt_link_line(5, buf, sizeof buf) == -1 && bt_link_line(0, buf, 1) == -1); /* line 4 is the SDP state; services start at 5 */

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
        CHECK(bt_acl_send(HANDLE, pdu, 44) == BT_E_PARTIAL); /* 20 + 20 fit in the two credits; the third piece has to wait: it cannot be taken back */
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


    /* ================= channels and SDP ================= */
    {
        /* the records of a headset as the device would send them */
        Buf recs = {{0}, 0}, lists = {{0}, 0};
        rec_a2dp(&recs);
        rec_headset(&recs);
        add_seq(&lists, &recs, 0);
        unsigned half = lists.n / 2;

        /* 16. the whole story: channel, request, an answer in two pieces (continuation), channel closed */
        unsigned conn_id = sdp_open_start();
        CHECK(line_is(4, "sdp: opening a channel to the service directory"));
        CHECK(bt_sdp_browse() == BT_E_LINK_BUSY);
        dev_conn_rsp(conn_id, 0);
        bt_poll();
        unsigned cfg_id = 0, id, dl;
        const unsigned char *d;
        CHECK(last_sig(0x04, &cfg_id, &d, &dl) && dl == 4 && le16(d) == DEV_CID && le16(d + 2) == 0); /* our Configuration Request: no options */
        unsigned char cq[8] = {0x40, 0, 0, 0, 1, 2, 0xA0, 0x02}; /* the device's: for our end 0x40, MTU option 672 */
        dev_sig(0x04, 0x50, cq, 8);
        bt_poll();
        CHECK(last_sig(0x05, &id, &d, &dl) && id == 0x50 && dl == 6 && le16(d) == DEV_CID && le16(d + 2) == 0 && le16(d + 4) == 0); /* we accept */
        CHECK(line_is(4, "sdp: opening a channel to the service directory")); /* ours is not acknowledged yet */
        unsigned char cr[6] = {0x40, 0, 0, 0, 0, 0};
        dev_sig(0x05, cfg_id, cr, 6);
        bt_poll();
        CHECK(line_is(4, "sdp: asking, 0 bytes so far"));
        {
            unsigned char exp[64];
            unsigned en = bt_sdp_request(exp, sizeof exp, 1, 400, 0, 0);
            CHECK(last_data(DEV_CID, &d, &dl) && dl == en && memcmp(d, exp, en) == 0); /* the request, transaction 1 */
        }
        /* the first piece comes with a bookmark: the device wants us to ask again */
        static const unsigned char bm[2] = {0xAA, 0xBB};
        F.clock += 40;
        dev_sdp_answer(1, lists.b, half, bm, 2);
        bt_poll();
        {
            char want[64];
            snprintf(want, sizeof want, "sdp: asking, %u bytes so far", half); /* the first piece is kept while the rest is asked for */
            CHECK(line_is(4, want));
        }
        {
            unsigned char exp[64];
            unsigned en = bt_sdp_request(exp, sizeof exp, 2, 400, bm, 2);
            CHECK(last_data(DEV_CID, &d, &dl) && dl == en && memcmp(d, exp, en) == 0); /* transaction 2, with the bookmark */
        }
        dev_sdp_answer(2, lists.b + half, lists.n - half, 0, 0);
        bt_poll();
        CHECK(line_is(4, "sdp: 2 services"));
        CHECK(line_is(5, "service: A2DP sink 0x110b; profile A2DP v1.2; psm 0x19; \"Audio Sink\""));
        CHECK(line_is(6, "service: headset 0x1108, unknown 0x1203; profile headset v1.2; rfcomm channel 1; \"Headset\""));
        CHECK(bt_link_line(7, buf, sizeof buf) == -1);
        /* the channel is closed from our side: Disconnection Request with the two ends */
        CHECK(last_sig(0x06, &id, &d, &dl) && dl == 4 && le16(d) == DEV_CID && le16(d + 2) == 0x40);
        unsigned disc_id = id;
        dev_sig(0x07, disc_id, d, 4);
        bt_poll();
        /* a second browse starts from nothing */
        CHECK(bt_sdp_browse() == 0);
        CHECK(line_is(4, "sdp: opening a channel to the service directory") && bt_link_line(5, buf, sizeof buf) == -1);
        bt_poll();
        CHECK(last_sig(0x02, &id, &d, &dl) && le16(d + 2) == 0x40); /* the channel number is free again */

        /* 16b. answers with the wrong identifier belong to another request: they are ignored */
        conn_id = sdp_open_start();
        dev_conn_rsp((conn_id + 1) & 0xFF, 0);
        bt_poll();
        CHECK(line_is(4, "sdp: opening a channel to the service directory"));
        collect();
        {
            int configs = 0;
            for (int i = 0; i < NPDU; i++)
                configs += sig_of(i, &id, &d, &dl) == 0x04;
            CHECK(configs == 0); /* no Configuration Request: the channel was not accepted */
        }
        dev_conn_rsp(conn_id, 0); /* the right one */
        bt_poll();
        CHECK(last_sig(0x04, &cfg_id, &d, &dl));
        {
            unsigned char cq3[8] = {0x40, 0, 0, 0, 1, 2, 0xA0, 0x02};
            dev_sig(0x04, 0x50, cq3, 8);
            unsigned char cr3[6] = {0x40, 0, 0, 0, 0, 0};
            dev_sig(0x05, (cfg_id + 1) & 0xFF, cr3, 6); /* acknowledges some other Configuration Request */
            bt_poll();
            CHECK(line_is(4, "sdp: opening a channel to the service directory"));
            dev_sig(0x05, cfg_id, cr3, 6);
            bt_poll();
            CHECK(line_is(4, "sdp: asking, 0 bytes so far"));
        }

        /* 16c. the Configuration Response names OUR end of the channel. A device that sends its own channel number is tolerated (the
         * identifier says which request it answers); a number that is neither does not count */
        {
            static const unsigned cids[3] = {DEV_CID, 0x0123, 0x0040}; /* its own, unrelated, ours */
            static const int opens[3] = {1, 0, 1};
            for (int k = 0; k < 3; k++)
            {
                conn_id = sdp_open_start();
                dev_conn_rsp(conn_id, 0);
                bt_poll();
                CHECK(last_sig(0x04, &cfg_id, &d, &dl));
                unsigned char cqz[8] = {0x40, 0, 0, 0, 1, 2, 0xA0, 0x02};
                dev_sig(0x04, 0x50, cqz, 8);
                unsigned char crz[6] = {(unsigned char)cids[k], (unsigned char)(cids[k] >> 8), 0, 0, 0, 0};
                dev_sig(0x05, cfg_id, crz, 6);
                bt_poll();
                CHECK(line_is(4, opens[k] ? "sdp: asking, 0 bytes so far" : "sdp: opening a channel to the service directory"));
            }
        }

        /* 17. a channel that is refused: PSM not supported, security block, no resources */
        static const struct { unsigned result; const char *line; } REFUSALS[] = {
            {2, "sdp: failed: channel refused (PSM not supported)"},
            {3, "sdp: failed: channel refused (security block)"},
            {4, "sdp: failed: channel refused (no resources)"},
            {9, "sdp: failed: channel refused (code 9)"}};
        for (unsigned i = 0; i < 4; i++)
        {
            conn_id = sdp_open_start();
            dev_conn_rsp(conn_id, REFUSALS[i].result);
            bt_poll();
            CHECK(line_is(4, REFUSALS[i].line));
        }
        /* pending first, then yes */
        conn_id = sdp_open_start();
        dev_conn_rsp(conn_id, 1);
        bt_poll();
        CHECK(line_is(4, "sdp: opening a channel to the service directory"));
        F.clock += 4000; /* the time limit starts again with every "pending" */
        dev_conn_rsp(conn_id, 1);
        F.clock += 4000;
        bt_poll();
        CHECK(line_is(4, "sdp: opening a channel to the service directory"));
        cfg_id = sdp_open_finish(conn_id);
        (void)cfg_id;
        CHECK(line_is(4, "sdp: asking, 0 bytes so far"));
        /* the device refuses our configuration */
        conn_id = sdp_open_start();
        dev_conn_rsp(conn_id, 0);
        bt_poll();
        CHECK(last_sig(0x04, &cfg_id, &d, &dl));
        unsigned char bad[6] = {0x40, 0, 0, 0, 2, 0};
        dev_sig(0x05, cfg_id, bad, 6);
        bt_poll();
        CHECK(line_is(4, "sdp: failed: channel refused (configuration refused, result 2)"));
        /* a Command Reject for our request */
        conn_id = sdp_open_start();
        unsigned char rej[2] = {0, 0};
        dev_sig(0x01, conn_id, rej, 2);
        bt_poll();
        CHECK(line_is(4, "sdp: failed: channel refused (rejected)"));
        /* nobody answers the Connection Request */
        conn_id = sdp_open_start();
        F.clock += 5500;
        bt_poll();
        CHECK(line_is(4, "sdp: opening a channel to the service directory")); /* 5.5 s is not enough: a device may take its time */
        F.clock += 5000;
        bt_poll();
        CHECK(line_is(4, "sdp: failed: no answer to the connection request"));
        /* the device answered the Connection Request but never configured its side / never accepted ours */
        conn_id = sdp_open_start();
        dev_conn_rsp(conn_id, 0);
        bt_poll();
        F.clock += 10500;
        bt_poll();
        CHECK(line_is(4, "sdp: failed: the configuration did not start"));
        conn_id = sdp_open_start();
        dev_conn_rsp(conn_id, 0);
        bt_poll();
        {
            unsigned char cqx[8] = {0x40, 0, 0, 0, 1, 2, 0xA0, 0x02};
            dev_sig(0x04, 0x50, cqx, 8); /* the device configures its side, ours is not accepted */
            bt_poll();
        }
        F.clock += 10500;
        bt_poll();
        CHECK(line_is(4, "sdp: failed: the device never accepted our configuration"));
        conn_id = sdp_open_start();
        dev_conn_rsp(conn_id, 0);
        bt_poll();
        {
            last_sig(0x04, &cfg_id, &d, &dl);
            unsigned char crx[6] = {0x40, 0, 0, 0, 0, 0};
            dev_sig(0x05, cfg_id, crx, 6); /* the device accepts ours but never sends its own */
            bt_poll();
        }
        F.clock += 10500;
        bt_poll();
        CHECK(line_is(4, "sdp: failed: the device never sent its configuration"));

        /* 18. the channel is open but the answer does not come, or is wrong */
        conn_id = sdp_open_start();
        sdp_open_finish(conn_id);
        F.clock += 5500;
        bt_poll();
        CHECK(line_is(4, "sdp: failed: no answer"));
        CHECK(last_sig(0x06, &id, &d, &dl)); /* the channel is closed all the same */
        conn_id = sdp_open_start();
        sdp_open_finish(conn_id);
        {
            unsigned char err[7] = {0x01, 0, 1, 0, 2, 0, 3}; /* error response, transaction 1, error code 3 */
            dev_pdu(0x40, err, 7);
            bt_poll();
            CHECK(line_is(4, "sdp: failed: the device refused (code 3)"));
        }
        conn_id = sdp_open_start();
        sdp_open_finish(conn_id);
        dev_sdp_answer(99, lists.b, lists.n, 0, 0); /* an answer to a question we did not ask */
        bt_poll();
        CHECK(line_is(4, "sdp: asking, 0 bytes so far"));
        {
            unsigned char odd[5] = {0x03, 0, 1, 0, 0}; /* some other PDU */
            dev_pdu(0x40, odd, 5);
            bt_poll();
            CHECK(line_is(4, "sdp: failed: unexpected answer (code 3)"));
        }
        conn_id = sdp_open_start();
        sdp_open_finish(conn_id);
        {
            unsigned char junk[3] = {0x09, 0x00, 0x01}; /* complete, but not a list of records */
            dev_sdp_answer(1, junk, 3, 0, 0);
            bt_poll();
            CHECK(line_is(4, "sdp: failed: answer not understood"));
        }
        conn_id = sdp_open_start();
        sdp_open_finish(conn_id);
        {
            /* piece after piece with bookmarks until it is too much */
            unsigned char piece[400];
            memset(piece, 0x35, sizeof piece);
            unsigned tid = 1;
            for (int i = 0; i < 6; i++)
            {
                dev_sdp_answer(tid++, piece, 400, bm, 2);
                bt_poll();
            }
            CHECK(line_is(4, "sdp: asking, 2400 bytes so far"));
            dev_sdp_answer(tid++, piece, 400, bm, 2);
            bt_poll();
            CHECK(line_is(4, "sdp: failed: answer too long"));
        }
        conn_id = sdp_open_start();
        sdp_open_finish(conn_id);
        {
            unsigned char b[40];
            memset(b, 0, sizeof b);
            b[0] = 0x07; b[1] = 0; b[2] = 1; b[3] = 0; b[4] = 6; b[5] = 0; b[6] = 40; /* says 40 bytes of data in 6 bytes of parameters */
            dev_pdu(0x40, b, 11);
            bt_poll();
            CHECK(line_is(4, "sdp: failed: answer too short"));
        }
        conn_id = sdp_open_start();
        sdp_open_finish(conn_id);
        {
            unsigned char b[40];
            memset(b, 0, sizeof b);
            b[0] = 0x07; b[1] = 0; b[2] = 1; b[3] = 0; b[4] = 22; b[5] = 0; b[6] = 0; b[7] = 20; /* a bookmark of 20 bytes: more than the 16 allowed */
            dev_pdu(0x40, b, 5 + 22);
            bt_poll();
            CHECK(line_is(4, "sdp: failed: bad continuation"));
        }

        /* 19. the link ends while we ask */
        conn_id = sdp_open_start();
        sdp_open_finish(conn_id);
        ev_disc_complete(HANDLE, 0x13);
        bt_poll();
        CHECK(line_is(4, "sdp: failed: link ended"));
        CHECK(bt_sdp_browse() == BT_E_NOT_UP); /* no link */

        /* 20. the device closes the channel itself, and things about channels that do not exist */
        conn_id = sdp_open_start();
        sdp_open_finish(conn_id);
        {
            unsigned char dr[4] = {0x40, 0, DEV_CID & 0xFF, DEV_CID >> 8}; /* Disconnection Request: our end, its end */
            dev_sig(0x06, 0x61, dr, 4);
            bt_poll();
            CHECK(last_sig(0x07, &id, &d, &dl) && id == 0x61 && dl == 4); /* answered with a Disconnection Response */
            unsigned char data[5] = {0x07, 0, 1, 0, 0};
            dev_pdu(0x40, data, 5); /* data for the channel that is gone: nothing happens */
            dev_pdu(0x55, data, 5); /* data for a channel number nobody has */
            bt_poll();
        }
        up();
        {
            unsigned char cq2[4] = {0x40, 0, 0, 0}; /* Configuration Request for a channel we never opened: Command Reject as before */
            dev_sig(0x04, 0x62, cq2, 4);
            bt_poll();
            CHECK(last_sig(0x01, &id, &d, &dl) && id == 0x62);
            unsigned char r5[6] = {0x77, 0, 0, 0, 0, 0}; /* answers for requests we never made: ignored */
            dev_sig(0x03, 0x63, r5, 6);
            dev_sig(0x05, 0x64, r5, 6);
            dev_sig(0x07, 0x65, r5, 4);
            bt_poll();
            CHECK(bt_link_state() == BT_LINK_UP);
        }
    }


    /* ================= pairing ================= */
    {
        static const unsigned char KEY[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
        unsigned char par[40];
        unsigned id, dl;
        const unsigned char *d;
        (void)id; (void)d; (void)dl;

        /* 21. Secure Simple Pairing, "Just Works": every step in order, with the right answers */
        bt_pair_forget();
        up();
        CHECK(line_is(0x80, "pair: -"));
        CHECK(bt_pair_start() == 0);
        CHECK(line_is(0x80, "pair: authenticating"));
        CHECK(bt_pair_start() == BT_E_LINK_BUSY);
        bt_poll();
        CHECK(F.nsent >= 4 && F.sent[F.nsent - 4] == 0x0C01 && F.sent[F.nsent - 3] == 0x0C56 && F.sent[F.nsent - 2] == 0x0C13 && F.sent[F.nsent - 1] == 0x0411);
        CHECK(F.npar[F.nsent - 4] == 8 && F.par[F.nsent - 4][5] == 0x5F && F.par[F.nsent - 4][6] == 0x3F && F.par[F.nsent - 4][7] == 0x1C); /* SSP events enabled */
        CHECK(F.npar[F.nsent - 3] == 1 && F.par[F.nsent - 3][0] == 1);                                                                      /* SSP mode on */
        CHECK(memcmp(F.par[F.nsent - 2], "HarisOS\0", 8) == 0);                                                                             /* our name */
        CHECK(F.npar[F.nsent - 1] == 2 && le16(F.par[F.nsent - 1]) == HANDLE);                                                              /* Authentication Requested */
        ev6(0x17, ADDR_LSB, 6); /* Link Key Request: we have none */
        bt_poll();
        CHECK(last_cmd() == 0x040C && memcmp(F.par[F.nsent - 1], ADDR_LSB, 6) == 0);
        ev6(0x31, ADDR_LSB, 6); /* IO Capability Request */
        bt_poll();
        CHECK(last_cmd() == 0x042B && F.npar[F.nsent - 1] == 9 && memcmp(F.par[F.nsent - 1], ADDR_LSB, 6) == 0);
        CHECK(F.par[F.nsent - 1][6] == 0x03 && F.par[F.nsent - 1][7] == 0x00 && F.par[F.nsent - 1][8] == 0x04); /* no input/output, no OOB, general bonding */
        memcpy(par, ADDR_LSB, 6);
        par[6] = 0x03; par[7] = 0; par[8] = 0x04;
        ev6(0x32, par, 9); /* IO Capability Response: the device has no input or output either */
        bt_poll();
        CHECK(line_is(0x81, "pair: device io capability 3, simple pairing -"));
        memcpy(par, ADDR_LSB, 6);
        par[6] = 0x78; par[7] = 0x56; par[8] = 0x34; par[9] = 0x12;
        ev6(0x33, par, 10); /* User Confirmation Request: accept */
        bt_poll();
        CHECK(last_cmd() == 0x042C && memcmp(F.par[F.nsent - 1], ADDR_LSB, 6) == 0);
        par[0] = 0;
        memcpy(par + 1, ADDR_LSB, 6);
        ev6(0x36, par, 7); /* Simple Pairing Complete: success */
        bt_poll();
        CHECK(line_is(0x81, "pair: device io capability 3, simple pairing status 0x00"));
        memcpy(par, ADDR_LSB, 6);
        memcpy(par + 6, KEY, 16);
        par[22] = 4; /* unauthenticated combination key (P-192) */
        ev6(0x18, par, 23); /* Link Key Notification */
        bt_poll();
        CHECK(line_is(0x80, "pair: authenticating"));
        par[0] = 0; par[1] = HANDLE & 0xFF; par[2] = HANDLE >> 8;
        ev6(0x06, par, 3); /* Authentication Complete */
        bt_poll();
        CHECK(line_is(0x80, "pair: encrypting the link"));
        CHECK(last_cmd() == 0x0413 && le16(F.par[F.nsent - 1]) == HANDLE && F.par[F.nsent - 1][2] == 1);
        par[0] = 0; par[1] = HANDLE & 0xFF; par[2] = HANDLE >> 8; par[3] = 1;
        ev6(0x08, par, 4); /* Encryption Change */
        bt_poll();
        CHECK(line_is(0x80, "pair: done, unauthenticated key (P-192), link encrypted"));
        CHECK(bt_link_line(0x80, buf, sizeof buf) > 0 && bt_link_line(0x82, buf, sizeof buf) == -1);

        /* 22. the next link to the same device starts with the stored key */
        CHECK(bt_link_disconnect() == 0);
        bt_poll();
        ev_disc_complete(HANDLE, 0x16);
        bt_poll();
        CHECK(line_is(0x80, "pair: done, unauthenticated key (P-192), link encrypted")); /* the result stays until the next try */
        CHECK(bt_link_connect(ADDR) == 0);
        ev_conn_complete(0, HANDLE, ADDR_LSB);
        bt_poll();
        CHECK(bt_pair_start() == 0); /* a finished pairing does not stop a new one */
        bt_poll();
        ev6(0x17, ADDR_LSB, 6);
        bt_poll();
        CHECK(last_cmd() == 0x040B && F.npar[F.nsent - 1] == 22 && memcmp(F.par[F.nsent - 1], ADDR_LSB, 6) == 0 && memcmp(F.par[F.nsent - 1] + 6, KEY, 16) == 0);
        par[0] = 0; par[1] = HANDLE & 0xFF; par[2] = HANDLE >> 8;
        ev6(0x06, par, 3);
        bt_poll();
        par[3] = 1;
        ev6(0x08, par, 4);
        bt_poll();
        CHECK(line_is(0x80, "pair: done, unauthenticated key (P-192), link encrypted"));
        CHECK(line_is(0x81, "pair: device io capability -, simple pairing -")); /* no pairing was needed */
        /* a Link Key Request for another device does not get our key */
        {
            const unsigned char other[6] = {9, 8, 7, 6, 5, 4};
            ev6(0x17, other, 6);
            bt_poll();
            CHECK(last_cmd() == 0x040C && memcmp(F.par[F.nsent - 1], other, 6) == 0);
        }
        /* and without being asked to pair, the stored key is still used when the device starts authenticating */
        ev6(0x17, ADDR_LSB, 6);
        bt_poll();
        CHECK(last_cmd() == 0x040B);

        /* 23. the device lost its key: the stored one is refused once, then pairing is done again from nothing */
        up(); /* a new fake chip; the stored key survives in the host */
        CHECK(bt_pair_start() == 0);
        bt_poll();
        CHECK(F.nsent >= 4 && F.sent[F.nsent - 3] == 0x0C56); /* a chip that was reset gets its pairing settings again */
        ev6(0x17, ADDR_LSB, 6);
        bt_poll();
        CHECK(last_cmd() == 0x040B);
        int auths = 0;
        for (int i = 0; i < F.nsent; i++)
            auths += F.sent[i] == 0x0411;
        par[0] = 0x06; par[1] = HANDLE & 0xFF; par[2] = HANDLE >> 8; /* Authentication Complete: PIN or key missing */
        ev6(0x06, par, 3);
        bt_poll();
        int auths2 = 0;
        for (int i = 0; i < F.nsent; i++)
            auths2 += F.sent[i] == 0x0411;
        CHECK(auths2 == auths + 1 && line_is(0x80, "pair: authenticating")); /* asked again at once */
        ev6(0x17, ADDR_LSB, 6);
        bt_poll();
        CHECK(last_cmd() == 0x040C); /* this time without the old key */
        CHECK(line_is(0x81, "pair: device io capability -, simple pairing -, the stored key was refused once"));
        ev6(0x06, par, 3); /* ... and refused again: that is a failure */
        bt_poll();
        CHECK(line_is(0x80, "pair: failed: authentication failed (status 0x06)"));

        /* 24. the old PIN, for devices without secure simple pairing */
        bt_pair_forget();
        up();
        CHECK(bt_pair_start() == 0);
        bt_poll();
        ev6(0x16, ADDR_LSB, 6); /* PIN Code Request */
        bt_poll();
        CHECK(last_cmd() == 0x040D && F.npar[F.nsent - 1] == 23 && memcmp(F.par[F.nsent - 1], ADDR_LSB, 6) == 0);
        CHECK(F.par[F.nsent - 1][6] == 4 && memcmp(F.par[F.nsent - 1] + 7, "0000", 4) == 0 && F.par[F.nsent - 1][11] == 0 && F.par[F.nsent - 1][22] == 0);
        CHECK(line_is(0x80, "pair: authenticating (old style, PIN 0000)"));
        memcpy(par, ADDR_LSB, 6);
        memcpy(par + 6, KEY, 16);
        par[22] = 0; /* combination key */
        ev6(0x18, par, 23);
        par[0] = 0; par[1] = HANDLE & 0xFF; par[2] = HANDLE >> 8;
        ev6(0x06, par, 3);
        bt_poll();
        par[3] = 1;
        ev6(0x08, par, 4);
        bt_poll();
        CHECK(line_is(0x80, "pair: done, combination key, link encrypted (PIN 0000)"));

        /* 25. what can go wrong */
        bt_pair_forget();
        up();
        CHECK(bt_pair_start() == 0);
        bt_poll();
        par[0] = 0x18; /* Simple Pairing Complete: the device does not allow pairing (status 0x18) */
        memcpy(par + 1, ADDR_LSB, 6);
        ev6(0x36, par, 7);
        bt_poll();
        CHECK(line_is(0x81, "pair: device io capability -, simple pairing status 0x18"));
        par[0] = 0x05; par[1] = HANDLE & 0xFF; par[2] = HANDLE >> 8;
        ev6(0x06, par, 3); /* authentication failure (wrong PIN, device refuses) */
        bt_poll();
        CHECK(line_is(0x80, "pair: failed: authentication failed (status 0x05)"));
        up();
        CHECK(bt_pair_start() == 0);
        bt_poll();
        par[0] = 0; par[1] = 0x99; par[2] = 0x09; /* another link's Authentication Complete: not ours */
        ev6(0x06, par, 3);
        bt_poll();
        CHECK(line_is(0x80, "pair: authenticating"));
        par[1] = HANDLE & 0xFF; par[2] = HANDLE >> 8;
        ev6(0x06, par, 3);
        bt_poll();
        par[0] = 0x03; par[3] = 0;
        ev6(0x08, par, 4); /* the encryption does not start */
        bt_poll();
        CHECK(line_is(0x80, "pair: failed: encryption failed (status 0x03)"));
        up();
        CHECK(bt_pair_start() == 0);
        bt_poll();
        par[0] = 0; par[3] = 0;
        ev6(0x06, par, 3);
        bt_poll();
        ev6(0x08, par, 4); /* "success" but encryption off: not accepted as secure */
        bt_poll();
        CHECK(line_is(0x80, "pair: failed: encryption failed"));
        up();
        F.auth_status = 0x0C; /* the chip refuses Authentication Requested */
        CHECK(bt_pair_start() == 0);
        bt_poll();
        CHECK(line_is(0x80, "pair: failed: the chip refused (status 0x0c)"));
        up();
        CHECK(bt_pair_start() == 0);
        bt_poll();
        F.clock += 31000; /* nobody answers */
        bt_poll();
        CHECK(line_is(0x80, "pair: failed: time-out"));
        up();
        CHECK(bt_pair_start() == 0);
        bt_poll();
        ev_disc_complete(HANDLE, 0x13);
        bt_poll();
        CHECK(line_is(0x80, "pair: failed: link ended"));
        CHECK(bt_pair_start() == BT_E_NOT_UP);

        /* 26. a pairing nobody here asked for is refused, a stored key answers anyway */
        bt_pair_forget();
        up();
        ev6(0x31, ADDR_LSB, 6); /* IO Capability Request */
        bt_poll();
        CHECK(last_cmd() == 0x0434 && F.npar[F.nsent - 1] == 7 && memcmp(F.par[F.nsent - 1], ADDR_LSB, 6) == 0 && F.par[F.nsent - 1][6] == 0x18);
        memcpy(par, ADDR_LSB, 6);
        par[6] = par[7] = par[8] = par[9] = 0;
        ev6(0x33, par, 10); /* User Confirmation Request */
        bt_poll();
        CHECK(last_cmd() == 0x042D);
        ev6(0x16, ADDR_LSB, 6); /* PIN Code Request */
        bt_poll();
        CHECK(last_cmd() == 0x040E);
        CHECK(bt_link_line(3, buf, sizeof buf) > 0 && strstr(buf, "PIN requests 1"));
        /* events that are too short are not read */
        ev6(0x17, ADDR_LSB, 3);
        ev6(0x18, ADDR_LSB, 6);
        ev6(0x33, ADDR_LSB, 2);
        bt_poll();
        CHECK(bt_link_state() == BT_LINK_UP);
        CHECK(bt_pair_start() == 0 && line_is(0x80, "pair: authenticating"));
        bt_pair_forget();
    }


    /* ================= diagnostics ================= */
    {
        /* the log of what was said on the signalling channel, and the sending statistics */
        up();
        CHECK(line_is(0x90, "sig: ->08 ->0a/2 ->0a/3"));
        CHECK(line_is(0x91, "acl tx: sent 3, not taken 0, credits 1 of 2, packet size 20"));
        {
            unsigned a = 0, b = 0, c = 0;
            const unsigned char *d2;
            unsigned dl2;
            collect();
            sig_of(0, &a, &d2, &dl2);
            sig_of(1, &b, &d2, &dl2);
            sig_of(2, &c, &d2, &dl2);
            unsigned char x[8] = {2, 0, 0, 0, 0xA8, 0, 0, 0};
            dev_sig(0x0B, b, x, 8);
            unsigned char rej[2] = {0, 0};
            dev_sig(0x01, c, rej, 2);
            bt_poll();
        }
        CHECK(line_is(0x90, "sig: ->08 ->0a/2 ->0a/3 <-0b/2 <-01"));
        CHECK(bt_link_line(0x92, buf, sizeof buf) == -1);
        /* only the last ten are shown, oldest first */
        for (int i = 0; i < 14; i++)
        {
            unsigned char t2[2] = {2, 0};
            dev_sig(0x0A, 0x70 + i, t2, 2);
            bt_poll();
        }
        CHECK(bt_link_line(0x90, buf, sizeof buf) > 0 && strncmp(buf, "sig: ", 5) == 0);
        {
            int n_arrows = 0;
            for (const char *q = buf; *q; q++)
                n_arrows += (q[0] == '-' && q[1] == '>') || (q[0] == '<' && q[1] == '-');
            CHECK(n_arrows == 10);
        }

        /* the chip does not take a packet right now (no credits): nothing is lost, it goes out when there is room */
        fresh();
        F.auto_credit = 0;
        CHECK(bt_link_connect(ADDR) == 0);
        ev_conn_complete(0, HANDLE, ADDR_LSB);
        bt_poll();
        CHECK(F.nacl == 2); /* two credits: echo and the first Information Request; the third waited */
        CHECK(line_is(0x91, "acl tx: sent 2, not taken 1, credits 0 of 2, packet size 20"));
        F.auto_credit = 1; /* the chip reports the two packets as done */
        bt_poll();
        bt_poll();
        CHECK(F.nacl == 3);
        collect();
        {
            unsigned id3 = 0, dl3 = 0;
            const unsigned char *d3;
            CHECK(sig_of(2, &id3, &d3, &dl3) == 0x0A && le16(d3) == 3); /* the third is the one that waited: not lost, not doubled */
        }
        CHECK(line_is(0x91, "acl tx: sent 3, not taken 1, credits 1 of 2, packet size 20"));
    }


    /* ================= A2DP (AVDTP) ================= */
    {
        unsigned label, sig, pn;
        const unsigned char *p;

        /* 27. the whole sequence: channel, Discover, Get Capabilities, Set Configuration, Open, media channel, Start; then Close */
        av_to_open();
        CHECK(line_is(0xA0, "a2dp: setting the codec"));
        CHECK(last_av(SIG_CID_DEV, &label, &sig, &p, &pn) && sig == 0x03 && pn == 12);
        {
            static const unsigned char want[] = {0x0C, 0x04, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x11, 0x15, 0x02, 0x33};
            CHECK(memcmp(p, want, 12) == 0); /* endpoint 3, ours 1, 48 kHz joint stereo, 16 blocks, 8 subbands, loudness, bitpool 2 .. 51 */
        }
        CHECK(line_is(0xA1, "a2dp: sink endpoint 3 offers SBC 48/44.1 kHz joint/stereo/dual/mono, bitpool 2-53"));
        CHECK(line_is(0xA2, "a2dp: configuration -")); /* chosen, but not "configured" until the sink accepts */
        av_accept(SIG_CID_DEV, 0x03, 0, 0);
        CHECK(line_is(0xA2, "a2dp: configured 48 kHz joint stereo, 16 blocks, 8 subbands, loudness, bitpool 51"));
        CHECK(line_is(0xA0, "a2dp: opening the stream"));
        CHECK(last_av(SIG_CID_DEV, &label, &sig, &p, &pn) && sig == 0x06 && pn == 1 && p[0] == 0x0C); /* Open */
        av_accept(SIG_CID_DEV, 0x06, 0, 0);
        CHECK(line_is(0xA0, "a2dp: opening the media channel"));
        chan_handshake(0x41, MEDIA_CID_DEV); /* the second channel to PSM 0x19 */
        CHECK(line_is(0xA0, "a2dp: starting the stream"));
        CHECK(last_av(SIG_CID_DEV, &label, &sig, &p, &pn) && sig == 0x07 && pn == 1 && p[0] == 0x0C); /* Start */
        CHECK(!bt_avdtp_streaming());
        av_accept(SIG_CID_DEV, 0x07, 0, 0);
        CHECK(line_is(0xA0, "a2dp: streaming (the sink accepted Start)"));
        CHECK(bt_avdtp_streaming());
        CHECK(bt_avdtp_start() == BT_E_LINK_BUSY);
        CHECK(bt_link_line(0xA3, buf, sizeof buf) == -1);
        /* the device may send media control packets on the media channel: nothing to do with them */
        {
            unsigned char junk[6] = {1, 2, 3, 4, 5, 6};
            dev_pdu(0x41, junk, 6);
            bt_poll();
            CHECK(bt_avdtp_streaming());
        }
        /* close: Close, the device accepts, both channels are disconnected */
        CHECK(bt_avdtp_stop() == 0);
        bt_poll();
        CHECK(line_is(0xA0, "a2dp: closing the stream"));
        CHECK(last_av(SIG_CID_DEV, &label, &sig, &p, &pn) && sig == 0x08 && pn == 1 && p[0] == 0x0C);
        av_accept(SIG_CID_DEV, 0x08, 0, 0);
        CHECK(line_is(0xA0, "a2dp: closed"));
        {
            unsigned idd, dld;
            const unsigned char *dd;
            CHECK(last_sig(0x06, &idd, &dd, &dld) && dld == 4); /* Disconnection Requests for the channels */
            collect();
            int discs = 0;
            for (int i = 0; i < NPDU; i++)
                discs += sig_of(i, &idd, &dd, &dld) == 0x06;
            CHECK(discs == 2);
        }
        CHECK(!bt_avdtp_streaming());
        CHECK(bt_avdtp_start() == 0); /* and it can be done again */

        /* 28. a label belongs to its command: answers with another label or signal are ignored; labels count up */
        av_open_sig();
        {
            unsigned l1 = 0, s1 = 0, n1 = 0;
            const unsigned char *p1;
            CHECK(last_av(SIG_CID_DEV, &l1, &s1, &p1, &n1) && s1 == 1);
            dev_av(0x40, (l1 + 1) & 15, 2, 1, SEPS, sizeof SEPS); /* wrong label */
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: asking for the stream endpoints"));
            dev_av(0x40, l1, 2, 2, SEPS, sizeof SEPS); /* wrong signal */
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: asking for the stream endpoints"));
            dev_av(0x40, l1, 2, 1, SEPS, sizeof SEPS);
            bt_poll();
            unsigned l2 = 0, s2 = 0, n2 = 0;
            const unsigned char *p2;
            CHECK(last_av(SIG_CID_DEV, &l2, &s2, &p2, &n2) && s2 == 2 && l2 == ((l1 + 1) & 15)); /* the next command has the next label */
        }

        /* 29. the device asks us something: general reject, as for any command we do not understand */
        {
            unsigned char par[1] = {0};
            dev_av(0x40, 5, 0, 1, par, 0); /* a Discover command from the device */
            bt_poll();
            const unsigned char *d;
            unsigned dl;
            CHECK(last_data(SIG_CID_DEV, &d, &dl) && dl == 2 && d[0] == ((5 << 4) | 1) && d[1] == 0x01); /* label 5, general reject, signal 1 */
            (void)par;
        }

        /* 30. things that go wrong, one by one */
        /* the signalling channel is refused: security block */
        up();
        CHECK(bt_avdtp_start() == 0);
        bt_poll();
        {
            unsigned id = 0, dl = 0;
            const unsigned char *d = 0;
            CHECK(last_sig(0x02, &id, &d, &dl));
            dev_conn_rsp(id, 3);
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: failed: the signalling channel was refused (0x03)"));
        }
        /* Discover is rejected / not answered / answered with nonsense / lists no free sink */
        av_open_sig();
        {
            unsigned l = 0, sg = 0, n = 0;
            const unsigned char *pp;
            CHECK(last_av(SIG_CID_DEV, &l, &sg, &pp, &n));
            unsigned char err[1] = {0x19};
            dev_av(0x40, l, 3, 1, err, 1);
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: failed: Discover was refused (0x19)"));
        }
        av_open_sig();
        F.clock += 6500;
        bt_poll();
        CHECK(line_is(0xA0, "a2dp: failed: Discover got no answer"));
        av_open_sig();
        {
            static const unsigned char odd[3] = {0x04, 0x00, 0x08};
            av_accept(SIG_CID_DEV, 0x01, odd, 3);
            CHECK(line_is(0xA0, "a2dp: failed: the Discover answer is damaged"));
        }
        av_open_sig();
        {
            static const unsigned char none[4] = {0x04, 0x00, 0x0A, 0x08}; /* a source and a sink that is in use */
            av_accept(SIG_CID_DEV, 0x01, none, 4);
            CHECK(line_is(0xA0, "a2dp: failed: the device has no free audio sink endpoint"));
        }
        av_open_sig();
        {
            unsigned l = 0, sg = 0, n = 0;
            const unsigned char *pp;
            CHECK(last_av(SIG_CID_DEV, &l, &sg, &pp, &n));
            dev_av(0x40, l, 1, 1, 0, 0); /* general reject: the device does not know the command */
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: failed: the device does not know this command (0x01)"));
        }
        /* Get Capabilities refused: the other form of the question, Get All Capabilities, is tried once */
        av_open_sig();
        av_accept(SIG_CID_DEV, 0x01, SEPS, sizeof SEPS);
        {
            unsigned l = 0, sg = 0, n = 0;
            const unsigned char *pp;
            CHECK(last_av(SIG_CID_DEV, &l, &sg, &pp, &n) && sg == 0x02);
            unsigned char err[1] = {0x19};
            dev_av(0x40, l, 3, 2, err, 1);
            bt_poll();
            CHECK(last_av(SIG_CID_DEV, &l, &sg, &pp, &n) && sg == 0x0C && n == 1 && pp[0] == 0x0C); /* Get All Capabilities */
            CHECK(line_is(0xA0, "a2dp: reading what the endpoint can do"));
            av_accept(SIG_CID_DEV, 0x0C, HCAPS, sizeof HCAPS);
            CHECK(line_is(0xA0, "a2dp: setting the codec"));
        }
        av_open_sig();
        av_accept(SIG_CID_DEV, 0x01, SEPS, sizeof SEPS);
        {
            unsigned l = 0, sg = 0, n = 0;
            const unsigned char *pp;
            unsigned char err[1] = {0x19};
            CHECK(last_av(SIG_CID_DEV, &l, &sg, &pp, &n));
            dev_av(0x40, l, 3, 2, err, 1);
            bt_poll();
            CHECK(last_av(SIG_CID_DEV, &l, &sg, &pp, &n) && sg == 0x0C);
            dev_av(0x40, l, 3, 0x0C, err, 1); /* refused again: that is the end */
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: failed: Get All Capabilities was refused (0x19)"));
        }
        /* capabilities we cannot use */
        {
            static const unsigned char c16[] = {0x07, 0x06, 0x00, 0x00, 0x8F, 0xFF, 0x02, 0x35}; /* 16 kHz only */
            static const unsigned char cmo[] = {0x07, 0x06, 0x00, 0x00, 0x3C, 0xFF, 0x02, 0x35}; /* dual channel and mono only */
            static const unsigned char cbp[] = {0x07, 0x06, 0x00, 0x00, 0x3F, 0xFF, 0x3C, 0x46}; /* bitpool 60 .. 70 */
            static const unsigned char cnc[] = {0x01, 0x00};                                     /* no codec at all */
            static const unsigned char ccut[] = {0x07, 0x06, 0x00};                              /* damaged */
            static const struct { const unsigned char *c; unsigned n; const char *line; } BAD[] = {
                {c16, sizeof c16, "a2dp: failed: no sampling rate in common (48 or 44.1 kHz)"},
                {cmo, sizeof cmo, "a2dp: failed: no channel mode in common (joint stereo or stereo)"},
                {cbp, sizeof cbp, "a2dp: failed: no usable bitpool"},
                {cnc, sizeof cnc, "a2dp: failed: the endpoint has no SBC codec"},
                {ccut, sizeof ccut, "a2dp: failed: the capabilities are damaged"}};
            for (unsigned i = 0; i < sizeof BAD / sizeof BAD[0]; i++)
            {
                av_open_sig();
                av_accept(SIG_CID_DEV, 0x01, SEPS, sizeof SEPS);
                av_accept(SIG_CID_DEV, 0x02, BAD[i].c, BAD[i].n);
                CHECK(line_is(0xA0, BAD[i].line));
            }
        }
        /* Set Configuration refused: the error is the last byte (the category comes before it) */
        av_to_open();
        {
            unsigned char err[2] = {0x07, 0x29};
            unsigned l = 0, sg = 0, n = 0;
            const unsigned char *pp;
            CHECK(last_av(SIG_CID_DEV, &l, &sg, &pp, &n) && sg == 0x03);
            dev_av(0x40, l, 3, 3, err, 2);
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: failed: Set Configuration was refused (0x29)"));
        }
        /* Open refused, Start refused, the media channel refused or never opened */
        av_to_open();
        av_accept(SIG_CID_DEV, 0x03, 0, 0);
        {
            unsigned char err[1] = {0x31};
            unsigned l = 0, sg = 0, n = 0;
            const unsigned char *pp;
            CHECK(last_av(SIG_CID_DEV, &l, &sg, &pp, &n) && sg == 0x06);
            dev_av(0x40, l, 3, 6, err, 1);
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: failed: Open was refused (0x31)"));
        }
        av_to_open();
        av_accept(SIG_CID_DEV, 0x03, 0, 0);
        av_accept(SIG_CID_DEV, 0x06, 0, 0);
        {
            unsigned id = 0, dl = 0;
            const unsigned char *d = 0;
            CHECK(last_sig(0x02, &id, &d, &dl) && le16(d + 2) == 0x41);
            unsigned char r[8] = {0, 0, 0x41, 0, 3, 0, 0, 0}; /* the media channel: security block */
            dev_sig(0x03, id, r, 8);
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: failed: the media channel was refused (0x03)"));
        }
        av_to_open();
        av_accept(SIG_CID_DEV, 0x03, 0, 0);
        av_accept(SIG_CID_DEV, 0x06, 0, 0);
        F.clock += 12500; /* nobody answers the second Connection Request */
        bt_poll();
        CHECK(line_is(0xA0, "a2dp: failed: the media channel did not open"));
        av_to_open();
        av_accept(SIG_CID_DEV, 0x03, 0, 0);
        av_accept(SIG_CID_DEV, 0x06, 0, 0);
        chan_handshake(0x41, MEDIA_CID_DEV);
        {
            unsigned char err[1] = {0x31};
            unsigned l = 0, sg = 0, n = 0;
            const unsigned char *pp;
            CHECK(last_av(SIG_CID_DEV, &l, &sg, &pp, &n) && sg == 0x07);
            dev_av(0x40, l, 3, 7, err, 1);
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: failed: Start was refused (0x31)"));
        }
        /* the signalling channel does not open in time */
        up();
        CHECK(bt_avdtp_start() == 0);
        bt_poll();
        F.clock += 12500;
        bt_poll();
        CHECK(line_is(0xA0, "a2dp: failed: the signalling channel did not open"));

        /* 31. the device closes a channel, or the link ends, while the stream is on */
        av_to_open();
        av_accept(SIG_CID_DEV, 0x03, 0, 0);
        av_accept(SIG_CID_DEV, 0x06, 0, 0);
        chan_handshake(0x41, MEDIA_CID_DEV);
        av_accept(SIG_CID_DEV, 0x07, 0, 0);
        CHECK(bt_avdtp_streaming());
        {
            unsigned char dr[4] = {0x41, 0, MEDIA_CID_DEV & 0xFF, MEDIA_CID_DEV >> 8}; /* Disconnection Request for the media channel */
            dev_sig(0x06, 0x66, dr, 4);
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: failed: the device closed the media channel"));
        }
        av_to_open();
        av_accept(SIG_CID_DEV, 0x03, 0, 0);
        {
            unsigned char dr[4] = {0x40, 0, SIG_CID_DEV & 0xFF, SIG_CID_DEV >> 8}; /* ... the signalling channel */
            dev_sig(0x06, 0x67, dr, 4);
            bt_poll();
            CHECK(line_is(0xA0, "a2dp: failed: the device closed the signalling channel"));
        }
        av_to_open();
        ev_disc_complete(HANDLE, 0x13);
        bt_poll();
        CHECK(line_is(0xA0, "a2dp: failed: link ended"));
        CHECK(bt_avdtp_start() == BT_E_NOT_UP);
        /* stopping when nothing runs, or before the stream is open, is harmless */
        CHECK(bt_avdtp_stop() == 0);
        av_open_sig();
        CHECK(bt_avdtp_stop() == 0);
        bt_poll();
        CHECK(line_is(0xA0, "a2dp: closed"));
        /* Close is not answered: after 3 s the channels are closed anyway */
        av_to_open();
        av_accept(SIG_CID_DEV, 0x03, 0, 0);
        av_accept(SIG_CID_DEV, 0x06, 0, 0);
        chan_handshake(0x41, MEDIA_CID_DEV);
        av_accept(SIG_CID_DEV, 0x07, 0, 0);
        CHECK(bt_avdtp_stop() == 0);
        bt_poll();
        F.clock += 3500;
        bt_poll();
        CHECK(line_is(0xA0, "a2dp: closed"));
        /* before anything: all lines say so */
        bt_detach();
        CHECK(bt_avdtp_start() == BT_E_NOT_UP);
    }


    /* ================= A2DP audio ================= */
    {
        NEXT_MTU = 1021; /* a chip that takes big packets: a media packet is one */

        /* 32. no stream: no sound */
        up();
        CHECK(bt_a2dp_tone(1) == BT_E_NOT_UP);
        CHECK(line_is(0xB0, "audio: -"));

        /* 33. a tune on a running stream: packets of 7 frames (what a 672 byte MTU takes of 89 byte frames), sent as the clock runs */
        av_stream();
        CHECK(bt_a2dp_tone(0) == -1 && bt_a2dp_tone(61) == -1);
        F.nacl = 0;
        CHECK(bt_a2dp_tone(1) == 0);
        CHECK(line_is(0xB0, "audio: playing the test tune, 0 packets of 7 frames (0 ms of sound), 0 underruns, 0 send errors"));
        CHECK(bt_a2dp_tone(1) == BT_E_LINK_BUSY);
        bt_poll(); /* the first step sends what the lead asks for: 60 ms of sound = 4 packets (18.7 ms each) */
        CHECK(media_packets() == 4);
        run_ms(1000, 10);
        int n = media_packets();
        CHECK(n == 54); /* 1 s of sound = 375 frames = 53 packets of 7 and one of 4 */
        CHECK(line_is(0xB0, "audio: tune finished, 54 packets of 7 frames (1000 ms of sound), 0 underruns, 0 send errors"));
        int frames;
        int samples = decode_media(n, &frames, 89);
        CHECK(frames == 375 && samples == 375 * 128);
        CHECK(PDU[media_idx[53]].data[4 + 12] == 4); /* the last packet has the frames that were left */
        CHECK(PDU[media_idx[0]].len == 4 + 13 + 7 * 89);
        /* what was sent is the tune: 523 Hz for 300 ms, 659 Hz, 784 Hz, then the first again (delay 73 samples of the codec) */
        int d = 73;
        int c1 = crossings(d + 2400, d + 12000), c2 = crossings(d + 14400 + 2400, d + 14400 + 12000), c3 = crossings(d + 28800 + 2400, d + 28800 + 12000);
        CHECK(c1 >= 2 * 523.25 * 0.2 - 3 && c1 <= 2 * 523.25 * 0.2 + 3); /* 9600 samples = 200 ms */
        CHECK(c2 >= 2 * 659.26 * 0.2 - 3 && c2 <= 2 * 659.26 * 0.2 + 3);
        CHECK(c3 >= 2 * 783.99 * 0.2 - 3 && c3 <= 2 * 783.99 * 0.2 + 3);
        int peak = 0;
        for (int i = 3000; i < 12000; i++)
            peak = abs(AUD[2 * i]) > peak ? abs(AUD[2 * i]) : peak;
        CHECK(peak > 7800 && peak < 8600); /* a quarter of full scale */
        CHECK(abs(AUD[2 * (d + 0)]) < 600); /* it starts softly: no click */
        /* the chip line (0xC0): the status and the USB link, for finding out why a command did not go */
        CHECK(bt_link_line(0xC0, buf, sizeof buf) > 0 && strncmp(buf, "chip: BT 0bda:b00a", 18) == 0 && strstr(buf, "usb up, command errors 0"));
        /* nothing more is sent after the end */
        run_ms(500, 10);
        CHECK(media_packets() == 54);
        /* the tune can be played again; the packet numbers start again */
        F.nacl = 0;
        CHECK(bt_a2dp_tone(1) == 0);
        bt_poll();
        CHECK(media_packets() == 4);

        /* 34. pacing: after t ms about (t + 60 ms) of sound has been sent */
        av_stream();
        F.nacl = 0;
        CHECK(bt_a2dp_tone(10) == 0);
        bt_poll();
        run_ms(1000, 10);
        n = media_packets();
        CHECK(n >= 56 && n <= 58); /* 1060 ms / 18.67 ms */
        run_ms(2000, 10);
        n = media_packets();
        CHECK(n >= 56 + 106 - 3 && n <= 56 + 106 + 4);
        /* a stop in the middle: the sound stops, the stream is closed */
        CHECK(bt_avdtp_stop() == 0);
        int before_stop = media_packets();
        run_ms(500, 10);
        CHECK(media_packets() == before_stop);
        CHECK(bt_link_line(0xB0, buf, sizeof buf) > 0 && strstr(buf, "audio: tune finished") && strstr(buf, "0 send errors"));
        {
            unsigned l = 0, sg = 0, pn = 0;
            const unsigned char *pp;
            CHECK(last_av(SIG_CID_DEV, &l, &sg, &pp, &pn) && sg == 0x08); /* Close */
        }

        /* 35. the system was busy: a long gap does not make a burst of packets, the clock starts again */
        av_stream();
        F.nacl = 0;
        CHECK(bt_a2dp_tone(10) == 0);
        bt_poll();
        run_ms(200, 10);
        int before = media_packets();
        F.clock += 2000; /* the main loop was stuck for two seconds */
        bt_poll();
        int after = media_packets();
        CHECK(after - before <= 4 && after - before >= 1);
        CHECK(bt_link_line(0xB0, buf, sizeof buf) > 0 && strstr(buf, "1 underruns"));
        run_ms(500, 10);
        CHECK(media_packets() - after >= 20 && media_packets() - after <= 30); /* and then it runs at the right pace again */

        /* 35b. a gap that is not long enough to be an underrun: the catching up is limited to a few packets per step */
        av_stream();
        F.nacl = 0;
        CHECK(bt_a2dp_tone(10) == 0);
        bt_poll();
        run_ms(200, 10);
        before = media_packets();
        F.clock += 200;
        bt_poll();
        after = media_packets();
        CHECK(after - before == 4); /* 200 ms behind would be 14 packets at once */
        CHECK(bt_link_line(0xB0, buf, sizeof buf) > 0 && strstr(buf, "0 underruns"));
        run_ms(100, 10);
        CHECK(media_packets() - after >= 6); /* the rest follows in the next steps */

        /* 35c. very small frames: no more than 15 in a packet (the count has four bits) */
        {
            static const unsigned char CAPS5[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3F, 0xFF, 0x02, 0x05}; /* bitpool up to 5: frames of 23 bytes */
            av_stream_caps(CAPS5, sizeof CAPS5);
            F.nacl = 0;
            CHECK(bt_a2dp_tone(1) == 0);
            bt_poll();
            CHECK(media_packets() >= 1);
            CHECK(PDU[media_idx[0]].data[4 + 12] == 15 && PDU[media_idx[0]].len == 4 + 13 + 15 * 23);
        }

        /* 35d. the notes end softly: no click between them (a note that stopped at full level would jump to the next one) */
        av_stream();
        F.nacl = 0;
        CHECK(bt_a2dp_tone(1) == 0);
        bt_poll();
        run_ms(1000, 10);
        n = media_packets();
        samples = decode_media(n, &frames, 89);
        CHECK(samples > 20000);
        {
            int edge = 73 + 14400; /* the end of the first note, where the codec delay is added */
            int maxend = 0, maxstart = 0;
            for (int i = edge - 20; i < edge; i++)
                maxend = abs(AUD[2 * i]) > maxend ? abs(AUD[2 * i]) : maxend;
            for (int i = edge; i < edge + 20; i++)
                maxstart = abs(AUD[2 * i]) > maxstart ? abs(AUD[2 * i]) : maxstart;
            CHECK(maxend < 1200 && maxstart < 1200);
            CHECK(abs(AUD[2 * (edge - 1)]) < 300 && abs(AUD[2 * edge]) < 300);
        }

        /* 36. the chip does not take packets: errors are counted, after twenty the sound is given up */
        av_stream();
        F.nacl = 0;
        CHECK(bt_a2dp_tone(10) == 0);
        F.acl_fail = 1;
        run_ms(500, 10);
        CHECK(bt_link_line(0xB0, buf, sizeof buf) > 0 && strstr(buf, "failed: the packets are not taken"));
        F.acl_fail = 0;

        /* 37. the stream ends while sound plays */
        av_stream();
        CHECK(bt_a2dp_tone(10) == 0);
        bt_poll();
        {
            unsigned char dr[4] = {0x41, 0, MEDIA_CID_DEV & 0xFF, MEDIA_CID_DEV >> 8}; /* the device closes the media channel */
            dev_sig(0x06, 0x66, dr, 4);
            bt_poll();
        }
        CHECK(bt_link_line(0xB0, buf, sizeof buf) > 0 && strstr(buf, "audio: failed: the stream is not running"));
        av_stream();
        CHECK(bt_a2dp_tone(10) == 0);
        bt_poll();
        ev_disc_complete(HANDLE, 0x13);
        bt_poll();
        CHECK(bt_link_line(0xB0, buf, sizeof buf) > 0 && strstr(buf, "audio: failed"));

        /* 38. a small packet size from the device: fewer frames in a packet, or none fit */
        HS_MTU = 150;
        av_stream();
        F.nacl = 0;
        CHECK(bt_a2dp_tone(1) == 0);
        bt_poll();
        run_ms(1000, 10);
        n = media_packets();
        CHECK(n == 375); /* one frame (89 + 13 bytes) per packet */
        CHECK(PDU[media_idx[0]].len == 4 + 13 + 89 && PDU[media_idx[0]].data[4 + 12] == 1);
        HS_MTU = 100;
        av_stream();
        CHECK(bt_a2dp_tone(1) == -1); /* not even one frame fits */
        HS_MTU = 672;

        /* 39. 44.1 kHz: the time stamps and the pace follow that rate */
        {
            static const unsigned char CAPS441[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x2F, 0xFF, 0x02, 0x26}; /* 44.1 kHz only, bitpool up to 38 */
            av_open_sig();
            av_accept(SIG_CID_DEV, 0x01, SEPS, sizeof SEPS);
            av_accept(SIG_CID_DEV, 0x02, CAPS441, sizeof CAPS441);
            av_accept(SIG_CID_DEV, 0x03, 0, 0);
            av_accept(SIG_CID_DEV, 0x06, 0, 0);
            chan_handshake(0x41, MEDIA_CID_DEV);
            av_accept(SIG_CID_DEV, 0x07, 0, 0);
            CHECK(bt_avdtp_streaming());
            F.nacl = 0;
            CHECK(bt_a2dp_tone(1) == 0);
            bt_poll();
            run_ms(1000, 10);
            n = media_packets();
            /* 1 s = 44100 samples = 344.5 frames of 128: 49 packets of 7 frames and one of 1 (frame 345 is cut: whole frames only) */
            CHECK(n == 50);
            CHECK(PDU[media_idx[0]].len == 4 + 13 + 7 * 89 && PDU[media_idx[49]].data[4 + 12] == 1); /* 49 packets of 7 frames, then the one frame that is left */
            CHECK(be32(PDU[media_idx[1]].data + 4 + 4) == 7 * 128); /* the time stamp counts samples */
            /* the notes keep their pitch: 523 Hz is the same number of cycles in the same time, at the lower rate there are fewer samples */
            samples = decode_media(n, &frames, 89);
            CHECK(frames == 344);
            int dd = 73, w0 = dd + (int)(44100 * 0.01), w1 = dd + (int)(44100 * 0.29); /* 10 ms .. 290 ms of the first note */
            int cr = crossings(w0, w1);
            CHECK(cr >= 2 * 523.25 * 0.28 - 3 && cr <= 2 * 523.25 * 0.28 + 3);
        }
        /* ... and the pace follows the rate: 1 s of real time is 1060 ms of sound = 52 packets of 20.3 ms (56.8 at 48 kHz) */
        {
            static const unsigned char CAPS441[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x2F, 0xFF, 0x02, 0x26};
            av_stream_caps(CAPS441, sizeof CAPS441);
            F.nacl = 0;
            CHECK(bt_a2dp_tone(10) == 0);
            bt_poll();
            run_ms(1000, 10);
            n = media_packets();
            CHECK(n >= 51 && n <= 53);
        }
        NEXT_MTU = MTU;
    }

    printf("bt_link: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
