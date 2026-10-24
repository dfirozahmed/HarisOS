/* Host test for tcp.c against a scripted fake server. The network functions tcp.c calls are replaced here: frames it
 * sends are queued, and net_poll() hands them to the fake server, which answers by calling tcp_input() directly. Time
 * is fake too (net_poll advances it by 10 ms), so timeouts and retransmissions take no real time. */
#include <stdio.h>
#include <string.h>
#include "net.h"
#include "net_internal.h"
#include "tcp.h"
#include "timer.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const unsigned char MAC_C[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const unsigned char MAC_S[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
static const unsigned char IP_C[4] = {10, 0, 2, 15};
static const unsigned char IP_S[4] = {93, 184, 216, 34};
#define SRV_PORT 80

/* ---- fake clock and network ---- */
static unsigned long long NOW;
unsigned long long timer_ms(void) { return NOW; }
unsigned long long timer_us(void) { return NOW * 1000; }
int net_present(void) { return 1; }
const unsigned char *net_mac(void) { return MAC_C; }
const unsigned char *net_ip(void) { return IP_C; }
int net_arp_mac(const unsigned char ip[4], unsigned char mac[6], unsigned timeout_ms)
{
    (void)ip; (void)timeout_ms;
    memcpy(mac, MAC_S, 6);
    return 1;
}
unsigned net_next_ip_id(void) { static unsigned id; return id++; }
void net_yield(void) {}

typedef struct { unsigned char d[1600]; unsigned n; } Frame;
static Frame Q[1024];
static unsigned QN, QDONE;

int net_send(const void *frame, unsigned len)
{
    if (QN < 1024 && len <= sizeof Q[0].d)
    {
        memcpy(Q[QN].d, frame, len);
        Q[QN++].n = len;
    }
    return 0;
}

/* ---- the fake server ---- */
static struct
{
    unsigned drop_syn, drop_data; /* ignore this many SYNs / data segments */
    int refuse;                   /* answer SYN with a reset */
    int silent;                   /* never answer anything after the handshake */
    unsigned isn;                 /* its first sequence number */
    unsigned window;              /* window it advertises */
    unsigned mss;                 /* MSS option it sends */
    /* state */
    unsigned cport, rcv_nxt, snd_nxt;
    unsigned char got[20000];
    unsigned got_len;
    int client_fin;
} S;

static void srv_reset(void)
{
    memset(&S, 0, sizeof S);
    S.isn = 5000;
    S.window = 65535;
    S.mss = 1200;
}

static void srv_inject(unsigned flags, unsigned seq, unsigned ack, const void *data, unsigned len, unsigned mss)
{
    unsigned char f[1600];
    unsigned n = tcp_build(f, MAC_S, MAC_C, IP_S, IP_C, SRV_PORT, S.cport, seq, ack, flags, S.window, mss, 1,
                           (const unsigned char *)data, len);
    TcpSeg g;
    CHECK(n && tcp_parse(f, n, &g));
    tcp_input(&g);
}

/* the server sends data (and maybe FIN) at its current sequence number */
static void srv_push(const void *data, unsigned len, int fin)
{
    srv_inject(TCP_ACK | TCP_PSH | (fin ? TCP_FIN : 0), S.snd_nxt, S.rcv_nxt, data, len, 0);
    S.snd_nxt += len + (fin ? 1 : 0);
}

static void srv_on_frame(const Frame *fr)
{
    TcpSeg g;
    if (!tcp_parse(fr->d, fr->n, &g))
        return;
    if (g.flags & TCP_SYN)
    {
        if (S.drop_syn)
        {
            S.drop_syn--;
            return;
        }
        S.cport = g.src_port;
        S.rcv_nxt = g.seq + 1;
        if (S.refuse)
        {
            srv_inject(TCP_RST | TCP_ACK, 0, S.rcv_nxt, 0, 0, 0);
            return;
        }
        S.snd_nxt = S.isn;
        srv_inject(TCP_SYN | TCP_ACK, S.snd_nxt, S.rcv_nxt, 0, 0, S.mss);
        S.snd_nxt++;
        return;
    }
    if (S.silent || (g.flags & TCP_RST))
        return;
    if (g.payload_len)
    {
        if (S.drop_data)
        {
            S.drop_data--;
            return;
        }
        if (g.seq == S.rcv_nxt)
        {
            memcpy(S.got + S.got_len, g.payload, g.payload_len);
            S.got_len += g.payload_len;
            S.rcv_nxt += g.payload_len;
        }
        srv_inject(TCP_ACK, S.snd_nxt, S.rcv_nxt, 0, 0, 0);
    }
    if ((g.flags & TCP_FIN) && g.seq + g.payload_len == S.rcv_nxt)
    {
        S.rcv_nxt++;
        S.client_fin = 1;
        srv_inject(TCP_FIN | TCP_ACK, S.snd_nxt, S.rcv_nxt, 0, 0, 0); /* acknowledge and close our side too */
        S.snd_nxt++;
    }
}

int net_poll(void)
{
    NOW += 10;
    while (QDONE < QN)
        srv_on_frame(&Q[QDONE++]);
    tcp_tick();
    return 0;
}

/* ---- helpers ---- */
static void begin(void)
{
    srv_reset();
    QN = QDONE = 0;
}

static unsigned count_frames(unsigned flags_all, unsigned flags_none, int with_data)
{
    unsigned c = 0;
    for (unsigned i = 0; i < QN; i++)
    {
        TcpSeg g;
        if (tcp_parse(Q[i].d, Q[i].n, &g) && (g.flags & flags_all) == flags_all && !(g.flags & flags_none) &&
            (!with_data || g.payload_len))
            c++;
    }
    return c;
}

static int last_seg(TcpSeg *g)
{
    return QN && tcp_parse(Q[QN - 1].d, Q[QN - 1].n, g);
}

static unsigned read_all(int h, unsigned char *buf, unsigned max, unsigned wait_ms)
{
    unsigned total = 0;
    int n;
    while (total < max && (n = tcp_recv(h, buf + total, max - total, wait_ms)) > 0)
        total += (unsigned)n;
    return total;
}

int main(void)
{
    unsigned char buf[20000];
    TcpSeg g;
    int h;

    /* 1. handshake, request, answer with FIN, orderly close */
    begin();
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    CHECK(h == 0);
    CHECK(count_frames(TCP_SYN, TCP_ACK, 0) == 1);
    CHECK(tcp_parse(Q[0].d, Q[0].n, &g) && g.mss == 1460 && g.ack == 0 && g.dst_port == SRV_PORT);
    CHECK(last_seg(&g) && g.flags == TCP_ACK && g.ack == S.isn + 1); /* the third handshake message */
    const char req[] = "GET / HTTP/1.0\r\n\r\n";
    CHECK(tcp_send(h, req, sizeof req - 1, 5000) == (int)sizeof req - 1);
    CHECK(S.got_len == sizeof req - 1 && memcmp(S.got, req, S.got_len) == 0);
    CHECK(tcp_recv(h, buf, 10, 100) == -2); /* nothing yet: a timeout, not an error */
    srv_push("HELLO", 5, 1);
    CHECK(tcp_recv(h, buf, 100, 1000) == 5 && memcmp(buf, "HELLO", 5) == 0);
    CHECK(tcp_recv(h, buf, 100, 1000) == 0); /* the FIN: end of stream */
    CHECK(tcp_recv(h, buf, 100, 1000) == 0); /* and it stays that way */
    CHECK(last_seg(&g) && g.flags == TCP_ACK && g.ack == S.snd_nxt); /* the FIN was acknowledged (counts as a byte) */
    tcp_close(h);
    CHECK(S.client_fin);
    CHECK(tcp_recv(h, buf, 1, 10) == -1); /* handle is gone */
    h = tcp_connect(IP_S, SRV_PORT, 5000); /* the slot is free again */
    CHECK(h == 0);
    tcp_close(h);

    /* 2. the first SYN is lost: sent again after one second */
    begin();
    S.drop_syn = 1;
    unsigned long long t0 = NOW;
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    CHECK(h == 0 && count_frames(TCP_SYN, TCP_ACK, 0) == 2 && NOW - t0 >= 1000 && NOW - t0 < 1200);
    tcp_close(h);

    /* ... and the server never answers: gives up after the timeout */
    begin();
    S.drop_syn = 100;
    t0 = NOW;
    CHECK(tcp_connect(IP_S, SRV_PORT, 2500) == -1 && NOW - t0 >= 2500 && NOW - t0 < 2700);
    CHECK(count_frames(TCP_SYN, TCP_ACK, 0) == 2); /* at 0 and 1000 ms; the next would be at 3000 */

    /* 3. connection refused (reset in answer to the SYN) */
    begin();
    S.refuse = 1;
    CHECK(tcp_connect(IP_S, SRV_PORT, 5000) == -1);
    S.refuse = 0;
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    CHECK(h == 0); /* the failed attempt did not keep its slot */
    tcp_close(h);

    /* 4. 5000 bytes with a server MSS of 1200: split into segments of at most 1200, arrives complete */
    begin();
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    unsigned char big[5000];
    for (unsigned i = 0; i < sizeof big; i++)
        big[i] = (unsigned char)(i * 7 + 3);
    CHECK(tcp_send(h, big, sizeof big, 5000) == (int)sizeof big);
    CHECK(S.got_len == sizeof big && memcmp(S.got, big, sizeof big) == 0);
    CHECK(count_frames(TCP_ACK | TCP_PSH, 0, 1) == 5);
    int max_seg = 0;
    for (unsigned i = 0; i < QN; i++)
        if (tcp_parse(Q[i].d, Q[i].n, &g) && (int)g.payload_len > max_seg)
            max_seg = (int)g.payload_len;
    CHECK(max_seg == 1200);
    tcp_close(h);

    /* 5. a data segment is lost: sent again, once, identical */
    begin();
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    S.drop_data = 1;
    t0 = NOW;
    CHECK(tcp_send(h, "ping", 4, 5000) == 4);
    CHECK(S.got_len == 4 && memcmp(S.got, "ping", 4) == 0 && NOW - t0 >= 1000);
    CHECK(count_frames(TCP_PSH, 0, 1) == 2);
    tcp_close(h);

    /* 6. out of order and duplicates: a gap is not buffered, the answer is a duplicate ACK of the old number */
    begin();
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    unsigned first = S.snd_nxt;
    srv_inject(TCP_ACK | TCP_PSH, first + 5, S.rcv_nxt, "BBBBB", 5, 0); /* second part arrives first */
    CHECK(last_seg(&g) && g.flags == TCP_ACK && g.ack == first);
    CHECK(tcp_recv(h, buf, 100, 50) == -2);
    srv_inject(TCP_ACK | TCP_PSH, first, S.rcv_nxt, "AAAAA", 5, 0);
    srv_inject(TCP_ACK | TCP_PSH, first + 5, S.rcv_nxt, "BBBBB", 5, 0); /* sent again */
    srv_inject(TCP_ACK | TCP_PSH, first, S.rcv_nxt, "AAAAA", 5, 0);     /* a stale duplicate */
    srv_inject(TCP_ACK | TCP_PSH, first + 3, S.rcv_nxt, "AABBBBB", 7, 0); /* overlaps what we have: only new bytes count */
    S.snd_nxt = first + 10;
    CHECK(read_all(h, buf, 100, 50) == 10 && memcmp(buf, "AAAAABBBBB", 10) == 0);
    CHECK(last_seg(&g) && g.ack == first + 10);
    tcp_close(h);

    /* 7. reset: only one at the right sequence number counts */
    begin();
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    srv_inject(TCP_RST, S.snd_nxt + 100000, 0, 0, 0, 0); /* far outside the window: ignored */
    CHECK(tcp_recv(h, buf, 10, 50) == -2);
    srv_inject(TCP_RST, S.snd_nxt, 0, 0, 0, 0);
    CHECK(tcp_recv(h, buf, 10, 50) == -1);
    CHECK(tcp_send(h, "x", 1, 50) == -1);
    tcp_close(h);

    /* 8. sequence numbers wrap around 2^32 */
    begin();
    S.isn = 0xFFFFFFFCu;
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    CHECK(h == 0);
    srv_push("HELLO", 5, 0); /* bytes before and after the wrap */
    CHECK(read_all(h, buf, 5, 50) == 5 && memcmp(buf, "HELLO", 5) == 0);
    CHECK(last_seg(&g) && g.ack == 0xFFFFFFFDu + 5); /* wrapped to 2 */
    CHECK(tcp_send(h, "after", 5, 1000) == 5 && S.got_len == 5);
    tcp_close(h);

    /* 9. the server's window limits what we send: 3 bytes at a time */
    begin();
    S.window = 3;
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    CHECK(tcp_send(h, "0123456789", 10, 5000) == 10 && S.got_len == 10 && memcmp(S.got, "0123456789", 10) == 0);
    max_seg = 0;
    for (unsigned i = 0; i < QN; i++)
        if (tcp_parse(Q[i].d, Q[i].n, &g) && (int)g.payload_len > max_seg)
            max_seg = (int)g.payload_len;
    CHECK(max_seg == 3);
    tcp_close(h);

    /* 10. our receive buffer (8192) fills up: the rest is refused, the window says 0, reading opens it again */
    begin();
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    unsigned char chunk[1000];
    for (unsigned i = 0; i < sizeof chunk; i++)
        chunk[i] = (unsigned char)i;
    unsigned seq0 = S.snd_nxt, sent = 0;
    for (int i = 0; i < 9; i++) /* 9000 bytes offered, in order, without anybody reading */
    {
        srv_inject(TCP_ACK | TCP_PSH, seq0 + sent, S.rcv_nxt, chunk, sizeof chunk, 0);
        sent += sizeof chunk;
    }
    CHECK(last_seg(&g) && g.flags == TCP_ACK);
    unsigned acked = g.ack - seq0;
    CHECK(acked == 8192 && g.window == 0); /* a partial segment fit, the one after it did not */
    int got1 = (int)read_all(h, buf, sizeof buf, 20);
    CHECK(got1 == 8192);
    CHECK(last_seg(&g) && g.window > 0); /* reading sent a window update */
    srv_inject(TCP_ACK | TCP_PSH, seq0 + acked, S.rcv_nxt, chunk + (acked % 1000), 9000 - acked, 0); /* the sender repeats */
    CHECK(read_all(h, buf + got1, sizeof buf - got1, 20) == 9000 - acked);
    for (unsigned i = 0; i < 9000; i++)
        if (buf[i] != (unsigned char)(i % 1000))
        {
            CHECK(!"stream content");
            break;
        }
    S.snd_nxt = seq0 + 9000;
    tcp_close(h);

    /* 11. a segment for a connection we do not have is answered with a reset */
    begin();
    {
        unsigned char f[200];
        unsigned n = tcp_build(f, MAC_S, MAC_C, IP_S, IP_C, 1234, 4321, 777, 0, TCP_SYN, 100, 0, 1, 0, 0);
        TcpSeg s;
        CHECK(tcp_parse(f, n, &s));
        tcp_input(&s);
        CHECK(last_seg(&g) && g.flags == (TCP_RST | TCP_ACK) && g.seq == 0 && g.ack == 778);
        CHECK(g.src_port == 4321 && g.dst_port == 1234 && memcmp(g.dst_mac, MAC_S, 6) == 0);
        unsigned before = QN;
        n = tcp_build(f, MAC_S, MAC_C, IP_S, IP_C, 1234, 4321, 777, 55, TCP_ACK, 100, 0, 1, (const unsigned char *)"x", 1);
        tcp_parse(f, n, &s);
        tcp_input(&s);
        CHECK(QN == before + 1 && last_seg(&g) && g.flags == TCP_RST && g.seq == 55); /* with ACK: seq = their ack */
        before = QN;
        n = tcp_build(f, MAC_S, MAC_C, IP_S, IP_C, 1234, 4321, 777, 0, TCP_RST, 0, 0, 1, 0, 0);
        tcp_parse(f, n, &s);
        tcp_input(&s);
        CHECK(QN == before); /* never answer a reset with a reset */
    }

    /* 12. the server disappears: after the retries are used up the connection is dead */
    begin();
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    S.silent = 1;
    t0 = NOW;
    CHECK(tcp_send(h, "hello?", 6, 200000) == -1);
    CHECK(NOW - t0 >= 30000 && count_frames(TCP_PSH, 0, 1) == 7); /* first try + 6 repeats: 1+2+4+8+8+8+8 s */
    CHECK(tcp_recv(h, buf, 1, 10) == -1);
    tcp_close(h);

    /* 13. close while the server already closed: CLOSE_WAIT -> LAST_ACK -> closed */
    begin();
    h = tcp_connect(IP_S, SRV_PORT, 5000);
    srv_push("bye", 3, 1);
    CHECK(read_all(h, buf, 3, 50) == 3 && tcp_recv(h, buf, 1, 50) == 0);
    CHECK(tcp_send(h, "still here", 10, 1000) == 10 && S.got_len == 10); /* a half-closed connection may still send */
    tcp_close(h);
    CHECK(S.client_fin);
    CHECK(count_frames(TCP_FIN, TCP_RST, 0) == 1);
    CHECK(count_frames(TCP_RST, 0, 0) == 0); /* it closed cleanly, no reset needed */

    if (fails == 0)
        printf("tcp tests passed\n");
    return fails ? 1 : 0;
}
