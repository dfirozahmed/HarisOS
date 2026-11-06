/* Host test of RFCOMM (kernel/bluetooth/bt_rfcomm.c) and OBEX Object Push (kernel/bluetooth/bt_obex.c).
 * The L2CAP layer below is a stand-in that records what goes out; a small fake phone answers like a real one would: UA to the SABMs, MSC, an OBEX
 * server that takes a file in pieces and says "continue". Also: the frame check sequence against frames seen on the air, damaged frames,
 * refusals and time-outs. */
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

/* ---- the layer below ---- */
static unsigned long long NOW;
static int LINK_UP = 1;
static int CHAN_STATE = BT_CHAN_FREE;
static int CHAN_RESULT;
static int TX_IDLE = 1;
static unsigned char OUT[64][1100];
static unsigned OUT_LEN[64];
static int OUT_N;
static int CLOSED_CHAN;

unsigned long long bt_now(void) { return NOW; }
BtLinkState bt_link_state(void) { return LINK_UP ? BT_LINK_UP : BT_LINK_NONE; }
int bt_chan_open(unsigned psm, int user)
{
    CHECK(psm == 3 && user == BT_USER_RFCOMM);
    CHAN_STATE = BT_CHAN_CONNECTING;
    return 1;
}
void bt_chan_close(int slot)
{
    CHECK(slot == 1);
    CHAN_STATE = BT_CHAN_FREE;
    CLOSED_CHAN++;
}
int bt_chan_state(int slot) { return slot == 1 ? CHAN_STATE : BT_CHAN_FREE; }
int bt_chan_result(int slot)
{
    (void)slot;
    return CHAN_RESULT;
}
int bt_chan_tx_idle(void) { return TX_IDLE; }
static int record(const unsigned char *d, unsigned n)
{
    if (OUT_N >= 64 || n > sizeof OUT[0])
        return -1;
    memcpy(OUT[OUT_N], d, n);
    OUT_LEN[OUT_N++] = n;
    return 0;
}
int bt_chan_queue(int slot, const unsigned char *d, unsigned n)
{
    CHECK(slot == 1 && n <= 60);
    return record(d, n);
}
int bt_chan_send(int slot, const unsigned char *d, unsigned n)
{
    CHECK(slot == 1);
    return record(d, n);
}

/* ---- a fake phone ---- */
static struct
{
    int accept;            /* 0 = refuse the file with 0xC3 */
    unsigned dlci;
    int mux_up, dlc_up;
    unsigned char got[4096];
    unsigned got_len;
    char name[64];
    unsigned announced;    /* the Length header */
    int saw_end, saw_connect, saw_disc, saw_abort;
    unsigned char obex[2048];
    unsigned obex_len;
    int puts;
    int msc_cmd_seen, msc_rsp_sent;
    unsigned peer_max;
    int stall;             /* do not answer PUTs */
} P;

static void phone_send(unsigned dlci, const unsigned char *d, unsigned n)
{
    unsigned char f[200];
    unsigned len = bt_rfcomm_build(f, sizeof f, dlci << 2 | 1, 0xEF, d, n); /* the phone answers as the non-initiator: C/R = 0 in the address... */
    CHECK(len > 0);
    bt_rfcomm_rx(f, len);
}

static void phone_obex_answer(unsigned code, const unsigned char *extra, unsigned n)
{
    unsigned char a[64];
    a[0] = (unsigned char)code;
    a[1] = 0;
    a[2] = (unsigned char)(3 + n);
    memcpy(a + 3, extra, n);
    phone_send(P.dlci, a, 3 + n);
}

static void phone_handle_obex(void)
{
    while (P.obex_len >= 3)
    {
        unsigned len = (unsigned)P.obex[1] << 8 | P.obex[2];
        if (P.obex_len < len)
            return;
        unsigned op = P.obex[0];
        if (op == 0x80)
        {
            P.saw_connect = 1;
            unsigned char x[] = {0x10, 0x00, (unsigned char)(P.peer_max >> 8), (unsigned char)P.peer_max, 0xCB, 0x00, 0x00, 0x00, 0x2A};
            phone_obex_answer(0xA0, x, sizeof x);
        }
        else if (op == 0x02 || op == 0x82)
        {
            P.puts++;
            unsigned i = 3;
            int cid_ok = 0;
            while (i < len)
            {
                unsigned id = P.obex[i], hl;
                if ((id >> 6) == 0 || (id >> 6) == 1)
                    hl = (unsigned)P.obex[i + 1] << 8 | P.obex[i + 2];
                else if ((id >> 6) == 2)
                    hl = 2;
                else
                    hl = 5;
                if (id == 0xCB && P.obex[i + 4] == 0x2A)
                    cid_ok = 1;
                if (id == 0x01)
                {
                    unsigned k = 0;
                    for (unsigned c = i + 3; c + 1 < i + hl && k < 62; c += 2)
                        P.name[k++] = (char)P.obex[c + 1];
                    P.name[k] = 0;
                }
                if (id == 0xC3)
                    P.announced = (unsigned)P.obex[i + 1] << 24 | (unsigned)P.obex[i + 2] << 16 | (unsigned)P.obex[i + 3] << 8 | P.obex[i + 4];
                if (id == 0x48 || id == 0x49)
                {
                    memcpy(P.got + P.got_len, P.obex + i + 3, hl - 3);
                    P.got_len += hl - 3;
                    if (id == 0x49)
                        P.saw_end = 1;
                }
                i += hl;
            }
            CHECK(cid_ok); /* the connection id from the CONNECT answer is on every request */
            CHECK(len <= P.peer_max);
            if (!P.stall)
            {
                if (!P.accept)
                    phone_obex_answer(0xC3, 0, 0);
                else
                    phone_obex_answer(op == 0x82 ? 0xA0 : 0x90, 0, 0);
            }
        }
        else if (op == 0x81)
        {
            P.saw_disc = 1;
            phone_obex_answer(0xA0, 0, 0);
        }
        else if (op == 0xFF)
            P.saw_abort = 1;
        memmove(P.obex, P.obex + len, P.obex_len - len);
        P.obex_len -= len;
    }
}

/* what our side sent: look at every frame and answer it */
static void phone_process(void)
{
    for (int i = 0; i < OUT_N;)
    {
        unsigned char f[1100];
        unsigned n = OUT_LEN[i];
        memcpy(f, OUT[i], n);
        /* remove the item first: answering may record new ones */
        memmove(OUT[i], OUT[i + 1], (size_t)(OUT_N - i - 1) * sizeof OUT[0]);
        memmove(&OUT_LEN[i], &OUT_LEN[i + 1], (size_t)(OUT_N - i - 1) * sizeof OUT_LEN[0]);
        OUT_N--;
        BtRfFrame fr;
        CHECK(bt_rfcomm_parse(f, n, &fr) == 0);
        unsigned type = fr.ctrl & ~0x10u;
        if (type == 0x2F) /* SABM */
        {
            unsigned char ua[8];
            unsigned l = bt_rfcomm_build(ua, sizeof ua, fr.dlci << 2 | 3, 0x73, 0, 0);
            if (fr.dlci == 0)
                P.mux_up = 1;
            else
            {
                CHECK(P.mux_up);
                P.dlc_up = 1;
                P.dlci = fr.dlci;
            }
            bt_rfcomm_rx(ua, l);
        }
        else if (type == 0x43) /* DISC */
        {
            unsigned char ua[8];
            unsigned l = bt_rfcomm_build(ua, sizeof ua, fr.dlci << 2 | 3, 0x73, 0, 0);
            bt_rfcomm_rx(ua, l);
        }
        else if (type == 0xEF && fr.dlci == 0) /* multiplexer message */
        {
            if (fr.data[0] == 0xE3)
            {
                P.msc_cmd_seen = 1;
                CHECK(fr.data[1] == 5 && fr.data[2] == (P.dlci << 2 | 3) && fr.data[3] == 0x8D);
                unsigned char r[4] = {0xE1, 5, fr.data[2], 0x8D};
                phone_send(0, r, 4);
                /* and its own MSC command, which must be answered */
                unsigned char c[4] = {0xE3, 5, (unsigned char)(P.dlci << 2 | 3), 0x8D};
                phone_send(0, c, 4);
                P.msc_rsp_sent = 1;
            }
        }
        else if (type == 0xEF) /* data */
        {
            CHECK(fr.len <= BT_RF_MAX_FRAME);
            CHECK(P.obex_len + fr.len <= sizeof P.obex);
            memcpy(P.obex + P.obex_len, fr.data, fr.len);
            P.obex_len += fr.len;
            phone_handle_obex();
        }
    }
}

static void run(int steps)
{
    for (int i = 0; i < steps; i++)
    {
        NOW += 20;
        if (CHAN_STATE == BT_CHAN_CONNECTING)
            CHAN_STATE = BT_CHAN_OPEN;
        bt_rfcomm_step(NOW);
        bt_obex_step(NOW);
        phone_process();
    }
}

static int read_file(void *ctx, unsigned off, unsigned char *buf, unsigned n)
{
    const unsigned char *src = ctx;
    for (unsigned i = 0; i < n; i++)
        buf[i] = src[off + i];
    return (int)n;
}

static void fresh(void)
{
    bt_obex_reset(); /* what an earlier case left running or half closed */
    bt_rfcomm_reset();
    memset(&P, 0, sizeof P);
    P.accept = 1;
    P.peer_max = 500;
    OUT_N = 0;
    CHAN_STATE = BT_CHAN_FREE;
    CHAN_RESULT = 0;
    LINK_UP = 1;
    TX_IDLE = 1;
}

/* ---- the server side: a phone sends us files ---- */
static int SK_FAIL_BEGIN, SK_FAIL_WRITE, SK_BEGAN, SK_ENDED, SK_OK;
static unsigned char SK_DATA[24000];
static unsigned SK_LEN, SK_SIZE;
static char SK_NAME[64];

static int sk_begin(void *c, const char *name, unsigned size)
{
    (void)c;
    if (SK_FAIL_BEGIN)
        return -1;
    SK_BEGAN++;
    SK_SIZE = size;
    strncpy(SK_NAME, name, sizeof SK_NAME - 1);
    SK_LEN = 0;
    return 0;
}

static int sk_write(void *c, const unsigned char *d, unsigned n)
{
    (void)c;
    if (SK_FAIL_WRITE)
        return -1;
    memcpy(SK_DATA + SK_LEN, d, n);
    SK_LEN += n;
    return 0;
}

static void sk_end(void *c, int ok)
{
    (void)c;
    SK_ENDED++;
    SK_OK = ok;
}

static const BtObexSink SINK = {sk_begin, sk_write, sk_end, 0};

#define DLCI_SRV (BT_SERVE_CHANNEL << 1)

/* the phone's frame to us, then the main loop runs a few times */
static void ph_frame(unsigned addr, unsigned ctrl, const unsigned char *d, unsigned n)
{
    unsigned char f[1200];
    unsigned len = bt_rfcomm_build(f, sizeof f, addr, ctrl, d, n);
    CHECK(len > 0);
    bt_rfcomm_rx(f, len);
    for (int i = 0; i < 3; i++)
    {
        NOW += 20;
        bt_rfcomm_step(NOW);
        bt_obex_step(NOW);
    }
}

static void ph_obex(const unsigned char *d, unsigned n) { ph_frame(DLCI_SRV << 2 | 3, 0xEF, d, n); }

/* the last frame we sent: its first data byte (the OBEX response code), or -1 */
static int resp_code(void)
{
    BtRfFrame fr;
    if (OUT_N == 0 || bt_rfcomm_parse(OUT[OUT_N - 1], OUT_LEN[OUT_N - 1], &fr) != 0 || fr.dlci != DLCI_SRV || fr.len < 3)
        return -1;
    return fr.data[0];
}

static void srv_start(void)
{
    fresh();
    SK_FAIL_BEGIN = SK_FAIL_WRITE = SK_BEGAN = SK_ENDED = SK_OK = 0;
    SK_LEN = 0;
    bt_obex_serve(&SINK);
    CHECK(bt_rfcomm_accept(1, bt_obex_srv_rx) == 0 && bt_rfcomm_is_server());
    CHAN_STATE = BT_CHAN_OPEN;
    ph_frame(0x03, 0x3F, 0, 0);                  /* SABM DLCI 0 */
    ph_frame(DLCI_SRV << 2 | 3, 0x3F, 0, 0);     /* SABM our channel */
    CHECK(bt_rfcomm_state() == BT_RF_OPEN);
    OUT_N = 0;
}

static void srv_tests(void)
{
    static const unsigned short NAME[] = {'a', '.', 'b', 'i', 'n', 0};
    unsigned char pkt[600];
    unsigned n;

    /* the opening handshake as seen from the phone */
    fresh();
    bt_obex_serve(&SINK);
    CHECK(bt_rfcomm_accept(1, bt_obex_srv_rx) == 0);
    CHECK(bt_rfcomm_accept(1, bt_obex_srv_rx) == BT_E_LINK_BUSY);
    CHAN_STATE = BT_CHAN_OPEN;
    ph_frame(0x03, 0x3F, 0, 0);
    {
        BtRfFrame fr;
        CHECK(OUT_N >= 1 && bt_rfcomm_parse(OUT[0], OUT_LEN[0], &fr) == 0 && fr.addr == 0x03 && (fr.ctrl & ~0x10u) == 0x63);
    }
    OUT_N = 0;
    /* the phone asks for parameter negotiation with credit flow control: the answer says no credits */
    {
        unsigned char pn[10] = {0x20 << 2 | 3, 8 << 1 | 1, DLCI_SRV, 0xF0, 0, 0, 0xE8, 0x03, 0, 7}; /* a frame size of 1000 */
        ph_frame(0x03, 0xEF, pn, 10);
        BtRfFrame fr;
        CHECK(OUT_N == 1 && bt_rfcomm_parse(OUT[0], OUT_LEN[0], &fr) == 0 && fr.data[0] == (0x20 << 2 | 1) && fr.data[3] == 0 && fr.data[9] == 0);
        CHECK(fr.data[6] == (BT_RF_MAX_N1 & 0xFF) && fr.data[7] == (BT_RF_MAX_N1 >> 8)); /* the frame size is cut to what one L2CAP PDU holds */
    }
    OUT_N = 0;
    /* a channel other than ours: DM */
    ph_frame(5 << 3 | 3, 0x3F, 0, 0);
    {
        BtRfFrame fr;
        CHECK(OUT_N == 1 && bt_rfcomm_parse(OUT[0], OUT_LEN[0], &fr) == 0 && (fr.ctrl & ~0x10u) == 0x0F);
    }
    OUT_N = 0;
    ph_frame(DLCI_SRV << 2 | 3, 0x3F, 0, 0);
    CHECK(bt_rfcomm_state() == BT_RF_OPEN);

    /* connect, a file in three packets */
    srv_start();
    {
        const unsigned char con[] = {0x80, 0, 7, 0x10, 0, 0x04, 0x00};
        ph_obex(con, sizeof con);
        CHECK(resp_code() == 0xA0);
        unsigned char body[300];
        for (int i = 0; i < 300; i++)
            body[i] = (unsigned char)(i ^ 0x5A);
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, NAME, 6, 450, 1, 0, body, 200);
        ph_obex(pkt, n);
        CHECK(resp_code() == 0x90 && SK_BEGAN == 1 && strcmp(SK_NAME, "a.bin") == 0 && SK_SIZE == 450 && SK_LEN == 200);
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, 0, 0, 0, 0, 0, body, 200);
        ph_obex(pkt, n);
        CHECK(resp_code() == 0x90 && SK_LEN == 400);
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, 0, 0, 0, 0, 1, body, 50);
        ph_obex(pkt, n);
        CHECK(resp_code() == 0xA0 && SK_LEN == 450 && SK_ENDED == 1 && SK_OK == 1);
        CHECK(memcmp(SK_DATA, body, 200) == 0 && memcmp(SK_DATA + 200, body, 200) == 0 && memcmp(SK_DATA + 400, body, 50) == 0);
        /* a second file on the same connection, then the goodbye */
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, NAME, 6, 3, 1, 1, (const unsigned char *)"xyz", 3);
        ph_obex(pkt, n);
        CHECK(resp_code() == 0xA0 && SK_BEGAN == 2 && SK_ENDED == 2 && SK_LEN == 3);
        const unsigned char dis[] = {0x81, 0, 3};
        ph_obex(dis, sizeof dis);
        CHECK(resp_code() == 0xA0);
    }

    /* a big packet, cut into frames of 600 bytes as a phone does after a bigger frame size was agreed */
    srv_start();
    {
        static unsigned char big[20000], bigpkt[21000];
        for (unsigned i = 0; i < sizeof big; i++)
            big[i] = (unsigned char)(i * 7 + (i >> 8));
        unsigned m = bt_obex_put_packet(bigpkt, sizeof bigpkt, 0, NAME, 6, sizeof big, 1, 1, big, sizeof big);
        CHECK(m > sizeof big);
        for (unsigned off = 0; off < m; off += 600)
            ph_obex(bigpkt + off, m - off < 600 ? m - off : 600);
        CHECK(resp_code() == 0xA0 && SK_BEGAN == 1 && SK_LEN == sizeof big && SK_ENDED == 1 && SK_OK == 1);
        CHECK(memcmp(SK_DATA, big, sizeof big) == 0);
    }

    /* requests that are refused */
    srv_start();
    {
        /* no name */
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, NAME, 6, 3, 0, 1, (const unsigned char *)"abc", 3);
        ph_obex(pkt, n);
        CHECK(resp_code() == 0xC0 && SK_BEGAN == 0);
        /* a name and nothing else: a request to delete the file */
        unsigned char del[] = {0x82, 0, 14, 0x01, 0, 11, 0, 'a', 0, '.', 0, 'b', 0, 0};
        del[2] = sizeof del;
        del[5] = sizeof del - 3;
        ph_obex(del, sizeof del);
        CHECK(resp_code() == 0xC3 && SK_BEGAN == 0);
        /* the sink does not want it (no drive) */
        SK_FAIL_BEGIN = 1;
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, NAME, 6, 3, 1, 1, (const unsigned char *)"abc", 3);
        ph_obex(pkt, n);
        CHECK(resp_code() == 0xD0);
        SK_FAIL_BEGIN = 0;
        /* a request we do not know */
        const unsigned char get[] = {0x83, 0, 3};
        ph_obex(get, sizeof get);
        CHECK(resp_code() == 0xD1);
        /* a damaged header list */
        unsigned char bad[] = {0x82, 0, 9, 0x01, 0xFF, 0xFF, 0, 0, 0};
        ph_obex(bad, sizeof bad);
        CHECK(resp_code() == 0xC0);
        /* a packet longer than we said we take is dropped; the next good one is answered */
        unsigned char huge[8] = {0x02, 0xFF, 0xFF};
        ph_obex(huge, sizeof huge);
        OUT_N = 0;
        const unsigned char con[] = {0x80, 0, 7, 0x10, 0, 0x04, 0x00};
        ph_obex(con, sizeof con);
        CHECK(resp_code() == 0xA0);
    }

    /* the drive fails in the middle; the phone breaks off; nothing comes for a long time */
    srv_start();
    {
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, NAME, 6, 100, 1, 0, (const unsigned char *)"abc", 3);
        ph_obex(pkt, n);
        CHECK(resp_code() == 0x90 && SK_BEGAN == 1);
        SK_FAIL_WRITE = 1;
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, 0, 0, 0, 0, 0, (const unsigned char *)"def", 3);
        ph_obex(pkt, n);
        CHECK(resp_code() == 0xD0 && SK_ENDED == 1 && SK_OK == 0);
        SK_FAIL_WRITE = 0;
    }
    srv_start();
    {
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, NAME, 6, 100, 1, 0, (const unsigned char *)"abc", 3);
        ph_obex(pkt, n);
        const unsigned char abort_[] = {0xFF, 0, 3};
        ph_obex(abort_, sizeof abort_);
        CHECK(resp_code() == 0xA0 && SK_ENDED == 1 && SK_OK == 0);
    }
    srv_start();
    {
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, NAME, 6, 100, 1, 0, (const unsigned char *)"abc", 3);
        ph_obex(pkt, n);
        NOW += 31000;
        bt_obex_step(NOW);
        CHECK(SK_ENDED == 1 && SK_OK == 0);
    }
    srv_start();
    {
        n = bt_obex_put_packet(pkt, sizeof pkt, 0, NAME, 6, 100, 1, 0, (const unsigned char *)"abc", 3);
        ph_obex(pkt, n);
        bt_rfcomm_reset(); /* the link ended */
        bt_obex_step(NOW);
        CHECK(SK_ENDED == 1 && SK_OK == 0);
        BtServeInfo si;
        bt_serve_info(&si);
        CHECK(si.on && !si.receiving);
        bt_obex_serve(0);
        bt_serve_info(&si);
        CHECK(!si.on);
    }
    /* switched off: nothing is taken */
    fresh();
    bt_obex_serve(0);
    bt_obex_srv_rx((const unsigned char *)"\x80\x00\x07", 3);
    bt_obex_step(NOW);
    CHECK(OUT_N == 0);
}

int main(void)
{
    /* ---- frame check sequence and framing ---- */
    {
        const unsigned char sabm0[] = {0x03, 0x3F, 0x01};
        CHECK(bt_rfcomm_fcs(sabm0, 3) == 0x1C); /* "03 3F 01 1C": SABM on DLCI 0 as seen in every capture */
        const unsigned char ua0[] = {0x03, 0x73, 0x01};
        CHECK(bt_rfcomm_fcs(ua0, 3) == 0xD7);   /* "03 73 01 D7" */
        unsigned char f[300];
        unsigned n = bt_rfcomm_build(f, sizeof f, 0x03, 0x3F, 0, 0);
        CHECK(n == 4 && f[0] == 3 && f[1] == 0x3F && f[2] == 1 && f[3] == 0x1C);
        /* the check byte makes the CRC of everything 0xCF */
        unsigned c = 0xFF;
        for (unsigned i = 0; i < n; i++)
        {
            c ^= f[i];
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (c >> 1) ^ 0xE0 : c >> 1;
        }
        CHECK(c == 0xCF);
        /* UIH: the length is not part of the check */
        unsigned char d[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        n = bt_rfcomm_build(f, sizeof f, 0x0B, 0xEF, d, 10);
        CHECK(n == 14 && f[2] == (10 << 1 | 1) && f[13] == bt_rfcomm_fcs(f, 2));
        BtRfFrame fr;
        CHECK(bt_rfcomm_parse(f, n, &fr) == 0 && fr.dlci == 2 && fr.cr == 1 && fr.len == 10 && fr.data[9] == 10);
        /* a two byte length */
        unsigned char big[200];
        for (int i = 0; i < 200; i++)
            big[i] = (unsigned char)i;
        n = bt_rfcomm_build(f, sizeof f, 0x0B, 0xEF, big, 200);
        CHECK(n == 205 && (f[2] & 1) == 0); /* address, control, two length bytes, 200 data, check */
        CHECK(bt_rfcomm_parse(f, n, &fr) == 0 && fr.len == 200 && fr.data[199] == 199);
        CHECK(bt_rfcomm_build(f, 10, 0x0B, 0xEF, big, 200) == 0);
        /* every cut-off and every single damaged byte is refused or at least harmless */
        n = bt_rfcomm_build(f, sizeof f, 0x0B, 0xEF, d, 10);
        for (unsigned cut = 0; cut < n; cut++)
            CHECK(bt_rfcomm_parse(f, cut, &fr) == -1);
        for (unsigned i = 0; i < n; i++)
        {
            unsigned char g[300];
            memcpy(g, f, n);
            g[i] ^= 0x04;
            int r = bt_rfcomm_parse(g, n, &fr);
            if (i == 2 || i == n - 1 || i < 2)
                CHECK(r == -1);
            else
                CHECK(r == 0); /* a data byte: the check does not cover it for UIH frames (the lower layer does) */
        }
    }

    /* ---- opening, sending, closing RFCOMM ---- */
    {
        fresh();
        CHECK(bt_rfcomm_open(0) != 0 && bt_rfcomm_open(31) != 0);
        LINK_UP = 0;
        CHECK(bt_rfcomm_open(9) == BT_E_NOT_UP);
        LINK_UP = 1;
        CHECK(bt_rfcomm_open(9) == 0 && bt_rfcomm_state() == BT_RF_OPENING);
        CHECK(bt_rfcomm_open(9) == BT_E_LINK_BUSY);
        run(10);
        CHECK(bt_rfcomm_state() == BT_RF_OPEN && P.mux_up && P.dlc_up && P.dlci == 18);
        CHECK(P.msc_cmd_seen && P.msc_rsp_sent);
        unsigned char data[300];
        for (int i = 0; i < 300; i++)
            data[i] = (unsigned char)(i * 3);
        CHECK(bt_rfcomm_send(data, 300) == 300);
        CHECK(OUT_N == 3 && OUT_LEN[0] == 127 + 4 && OUT_LEN[2] == 46 + 4);
        OUT_N = 0;
        bt_rfcomm_close();
        CHECK(bt_rfcomm_state() == BT_RF_CLOSING);
        run(10);
        CHECK(bt_rfcomm_state() == BT_RF_CLOSED);
        CHECK(CHAN_STATE == BT_CHAN_FREE);
        CHECK(bt_rfcomm_send(data, 3) == -1);
    }

    /* ---- the L2CAP channel is refused ---- */
    {
        fresh();
        CHECK(bt_rfcomm_open(5) == 0);
        CHAN_STATE = BT_CHAN_FREE; /* the stand-in turns CONNECTING into OPEN; here the device said no */
        CHAN_RESULT = 2;
        bt_rfcomm_step(NOW + 1);
        int code = 0;
        CHECK(bt_rfcomm_state() == BT_RF_FAILED && bt_rfcomm_why(&code)[0] && code == 2);
        /* and it can be tried again */
        CHECK(bt_rfcomm_open(5) == 0 && bt_rfcomm_state() == BT_RF_OPENING);
        /* nobody answers the SABM */
        CHAN_STATE = BT_CHAN_OPEN;
        NOW += 20;
        bt_rfcomm_step(NOW);
        OUT_N = 0;
        NOW += 11000;
        bt_rfcomm_step(NOW);
        CHECK(bt_rfcomm_state() == BT_RF_FAILED);
    }

    /* ---- the device answers DM (no such channel) ---- */
    {
        fresh();
        CHECK(bt_rfcomm_open(7) == 0);
        CHAN_STATE = BT_CHAN_OPEN;
        NOW += 20;
        bt_rfcomm_step(NOW);
        OUT_N = 0;
        unsigned char ua[8];
        unsigned l = bt_rfcomm_build(ua, sizeof ua, 0x03, 0x73, 0, 0);
        bt_rfcomm_rx(ua, l);
        NOW += 20;
        bt_rfcomm_step(NOW);
        OUT_N = 0;
        unsigned char dm[8];
        l = bt_rfcomm_build(dm, sizeof dm, 14 << 2 | 3, 0x1F, 0, 0);
        bt_rfcomm_rx(dm, l);
        CHECK(bt_rfcomm_state() == BT_RF_FAILED);
        /* garbage after the failure changes nothing */
        bt_rfcomm_rx(ua, l);
        CHECK(bt_rfcomm_state() == BT_RF_FAILED);
    }

    /* ---- sending a file ---- */
    {
        fresh();
        unsigned char file[3000];
        for (unsigned i = 0; i < sizeof file; i++)
            file[i] = (unsigned char)(i * 7 + (i >> 8));
        CHECK(bt_obex_send("photo.jpg", sizeof file, read_file, file, 9) == 0);
        CHECK(bt_obex_state() == BT_OBEX_RUNNING);
        CHECK(bt_obex_send("x", 1, read_file, file, 9) == BT_E_LINK_BUSY);
        run(400);
        unsigned done, total;
        bt_obex_progress(&done, &total);
        CHECK(bt_obex_state() == BT_OBEX_DONE);
        CHECK(done == sizeof file && total == sizeof file);
        CHECK(P.saw_connect && P.saw_disc && P.saw_end);
        CHECK(P.got_len == sizeof file && memcmp(P.got, file, sizeof file) == 0);
        CHECK(strcmp(P.name, "photo.jpg") == 0 && P.announced == sizeof file);
        CHECK(P.puts >= 7); /* 3000 bytes in packets of at most 500 */
        char line[80];
        CHECK(bt_obex_line(line, sizeof line) > 0 && strstr(line, "done"));
        CHECK(bt_rfcomm_state() == BT_RF_CLOSED || bt_rfcomm_state() == BT_RF_CLOSING);
    }

    /* ---- an empty file, a big device packet, a name with other letters ---- */
    {
        fresh();
        P.peer_max = 4000;
        CHECK(bt_obex_send("empty.txt", 0, read_file, "", 9) == 0);
        run(100);
        CHECK(bt_obex_state() == BT_OBEX_DONE && P.puts == 1 && P.saw_end && P.got_len == 0 && P.announced == 0);
        fresh();
        P.peer_max = 4000;
        unsigned char file[2500];
        memset(file, 0x5A, sizeof file);
        CHECK(bt_obex_send("\xC3\xA9t\xC3\xA9", sizeof file, read_file, file, 3) == 0);
        run(300);
        CHECK(bt_obex_state() == BT_OBEX_DONE && P.got_len == sizeof file);
        CHECK(P.puts == 3); /* our own packets stop at 1000 bytes */
        CHECK(P.name[0] == (char)0xE9 && P.name[1] == 't' && P.name[2] == (char)0xE9); /* low byte of each UTF-16 letter */
    }

    /* ---- the phone's user says no ---- */
    {
        fresh();
        P.accept = 0;
        unsigned char file[600];
        memset(file, 1, sizeof file);
        CHECK(bt_obex_send("a.bin", sizeof file, read_file, file, 9) == 0);
        run(200);
        int code = 0;
        CHECK(bt_obex_state() == BT_OBEX_FAILED && bt_obex_why(&code)[0] && code == 0xC3);
        char line[80];
        CHECK(bt_obex_line(line, sizeof line) > 0 && strstr(line, "failed"));
    }

    /* ---- no answer to the PUT: the time-out, long for the first one ---- */
    {
        fresh();
        P.stall = 1;
        unsigned char file[100];
        memset(file, 1, sizeof file);
        CHECK(bt_obex_send("a.bin", sizeof file, read_file, file, 9) == 0);
        run(50);
        CHECK(bt_obex_state() == BT_OBEX_RUNNING && P.puts == 1);
        NOW += 30000; /* the user of the phone is still reading the question */
        bt_obex_step(NOW);
        CHECK(bt_obex_state() == BT_OBEX_RUNNING);
        NOW += 40000;
        bt_obex_step(NOW);
        int code = 0;
        CHECK(bt_obex_state() == BT_OBEX_FAILED && code == 0 && bt_obex_why(&code)[0] && code == BT_E_TIMEOUT);
    }

    /* ---- cancel, link loss, a file that cannot be read ---- */
    {
        fresh();
        P.stall = 1;
        unsigned char file[100];
        memset(file, 1, sizeof file);
        CHECK(bt_obex_send("a.bin", sizeof file, read_file, file, 9) == 0);
        run(50);
        bt_obex_cancel();
        run(5);
        CHECK(bt_obex_state() == BT_OBEX_FAILED && P.saw_abort);
        fresh();
        CHECK(bt_obex_send("a.bin", sizeof file, read_file, file, 9) == 0);
        run(3);
        bt_obex_reset();
        bt_rfcomm_reset();
        CHECK(bt_obex_state() == BT_OBEX_FAILED);
        CHECK(bt_obex_send("a.bin", sizeof file, read_file, file, 9) == 0); /* and a new one can start */
        fresh();
        CHECK(bt_obex_send("", 5, read_file, file, 9) < 0 && bt_obex_send("a", 5, 0, file, 9) < 0);
    }

    /* ---- the device sends control messages of its own ---- */
    {
        fresh();
        CHECK(bt_rfcomm_open(9) == 0);
        run(10);
        CHECK(bt_rfcomm_state() == BT_RF_OPEN);
        OUT_N = 0;
        unsigned char pn[10] = {0}; /* PN command: type 0x20, length 8 */
        pn[0] = 0x20 << 2 | 3;
        pn[1] = 8 << 1 | 1;
        pn[2] = 18;
        pn[3] = 0xF0; /* credit based flow control requested */
        pn[6] = 0xFF; /* a frame size of 255 */
        pn[7] = 0;
        pn[8] = 0;
        pn[9] = 0;
        unsigned char m[12];
        memcpy(m, pn, 10);
        phone_send(0, m, 10);
        CHECK(OUT_N == 1);
        BtRfFrame fr;
        CHECK(bt_rfcomm_parse(OUT[0], OUT_LEN[0], &fr) == 0 && fr.dlci == 0 && fr.data[0] == (0x20 << 2 | 1) && fr.data[2 + 1] == 0);
        OUT_N = 0;
        unsigned char foff[2] = {0x18 << 2 | 3, 1};
        phone_send(0, foff, 2);
        CHECK(bt_rfcomm_send((const unsigned char *)"x", 1) == -1); /* flow off: nothing goes out */
        unsigned char fon[2] = {0x28 << 2 | 3, 1};
        phone_send(0, fon, 2);
        CHECK(bt_rfcomm_send((const unsigned char *)"x", 1) == 1);
        OUT_N = 0;
        unsigned char odd[2] = {0x3F << 2 | 3, 1}; /* a message type we do not know */
        phone_send(0, odd, 2);
        CHECK(OUT_N == 1);
        /* the device asks for a channel of ours: refused */
        unsigned char sabm[8];
        unsigned l = bt_rfcomm_build(sabm, sizeof sabm, 4 << 2 | 1, 0x3F, 0, 0);
        OUT_N = 0;
        bt_rfcomm_rx(sabm, l);
        CHECK(OUT_N == 1 && bt_rfcomm_parse(OUT[0], OUT_LEN[0], &fr) == 0 && (fr.ctrl & ~0x10u) == 0x0F);
        /* the device closes the channel */
        unsigned char disc[8];
        l = bt_rfcomm_build(disc, sizeof disc, 18 << 2 | 1, 0x53, 0, 0);
        bt_rfcomm_rx(disc, l);
        CHECK(bt_rfcomm_state() == BT_RF_CLOSED);
    }

    srv_tests();

    printf("bt_obex / bt_rfcomm: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
