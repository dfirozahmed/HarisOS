/* TCP client. See include/tcp.h.
 *
 * TCP in short: a connection is a pair of byte streams, one each way. Every byte has a 32-bit sequence number; a
 * segment says "my first byte is number SEQ" and "I have received everything before number ACK", plus how much more
 * it can take (WINDOW). The open handshake is SYN -> SYN+ACK -> ACK, the close is FIN -> ACK in each direction.
 * Segments can be lost, so everything that uses sequence space (SYN, data, FIN) is kept and sent again after a
 * timeout, which doubles each time. Sequence numbers wrap around at 2^32, so they are compared by the sign of the
 * difference: (int)(a - b) < 0 means a is before b.
 *
 * Here one segment is in flight at a time (stop and wait): simple, correct, and slower than a real stack. */
#include "net.h"
#include "net_internal.h"
#include "tcp.h"
#include "timer.h"

#define RCV_BUF 8192
#define RTO_INITIAL_MS 1000
#define RTO_MAX_MS 8000
#define MAX_RETRIES 6
#define DEFAULT_MSS 536 /* what a peer that sends no MSS option can take (RFC 9293) */

typedef enum
{
    ST_CLOSED,
    ST_SYN_SENT,
    ST_ESTABLISHED,
    ST_FIN_WAIT_1, /* we sent FIN, not yet acknowledged */
    ST_FIN_WAIT_2, /* our FIN was acknowledged, waiting for the peer's FIN */
    ST_CLOSE_WAIT, /* the peer closed; we may still send */
    ST_CLOSING,    /* both sent FIN at the same time */
    ST_LAST_ACK    /* peer closed first, then we sent FIN */
} State;

typedef struct
{
    int used; /* handle given out by tcp_connect, until tcp_close */
    State state;
    unsigned char ip[4], mac[6];
    unsigned port, lport;
    unsigned snd_una; /* oldest sequence number not yet acknowledged */
    unsigned snd_nxt; /* next sequence number we will use */
    unsigned rcv_nxt; /* next sequence number we expect from the peer */
    unsigned peer_wnd, peer_mss;
    /* the one segment in flight, kept for retransmission */
    int rtx;
    unsigned rtx_flags, rtx_len, rto;
    unsigned char rtx_data[TCP_MAX_PAYLOAD];
    unsigned long long rtx_at; /* when to send it again */
    int retries;
    /* received bytes the program has not read yet: a ring */
    unsigned char rcv[RCV_BUF];
    unsigned rcv_head, rcv_count;
    int rcv_fin; /* the peer's FIN arrived and everything before it was received */
    int err;     /* reset, or too many retransmissions */
} Sock;

static Sock SOCKS[TCP_MAX_SOCKETS];
static unsigned NEXT_PORT = 40000;

static int seq_lt(unsigned a, unsigned b) { return (int)(a - b) < 0; }
static int seq_le(unsigned a, unsigned b) { return (int)(a - b) <= 0; }

static void copy(unsigned char *d, const unsigned char *s, unsigned n)
{
    volatile unsigned char *vd = d;
    const volatile unsigned char *vs = s;
    for (unsigned i = 0; i < n; i++)
        vd[i] = vs[i];
}

static int same_ip(const unsigned char *a, const unsigned char *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static unsigned rcv_window(const Sock *s) { return RCV_BUF - s->rcv_count; }

/* Builds and sends one segment with the given sequence number. The ACK field is always rcv_nxt; whether it counts
 * is up to the ACK flag. */
static void send_raw(Sock *s, unsigned seq, unsigned flags, const unsigned char *data, unsigned len)
{
    unsigned char f[ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN + 4 + TCP_MAX_PAYLOAD];
    unsigned n = tcp_build(f, net_mac(), s->mac, net_ip(), s->ip, s->lport, s->port, seq, s->rcv_nxt, flags,
                           rcv_window(s), (flags & TCP_SYN) ? TCP_MAX_PAYLOAD : 0, net_next_ip_id(), data, len);
    if (n)
        net_send(f, n);
}

static void send_ack(Sock *s) { send_raw(s, s->snd_nxt, TCP_ACK, 0, 0); }

/* Sends a segment that uses sequence space (SYN, data, FIN) and keeps it until it is acknowledged. Only one at a
 * time: the caller waits until rtx is clear. */
static void emit(Sock *s, unsigned flags, const unsigned char *data, unsigned len)
{
    if (len)
        copy(s->rtx_data, data, len);
    s->rtx = 1;
    s->rtx_flags = flags;
    s->rtx_len = len;
    s->rto = RTO_INITIAL_MS;
    s->rtx_at = timer_ms() + s->rto;
    s->retries = 0;
    send_raw(s, s->snd_nxt, flags, s->rtx_data, len);
    s->snd_nxt += len + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);
}

static void abort_conn(Sock *s)
{
    s->err = 1;
    s->state = ST_CLOSED;
    s->rtx = 0;
}

/* Answers a segment for a port nobody listens on (or a connection we no longer have) with a reset, so the peer
 * stops. Never answer a reset with a reset. */
static void send_rst_for(const TcpSeg *g)
{
    unsigned char f[ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN];
    unsigned seq = 0, ack = 0, flags = TCP_RST;
    if (g->flags & TCP_ACK)
        seq = g->ack; /* the reset must look like it comes at the sequence number the peer expects */
    else
    {
        flags |= TCP_ACK;
        ack = g->seq + g->payload_len + ((g->flags & TCP_SYN) ? 1 : 0) + ((g->flags & TCP_FIN) ? 1 : 0);
    }
    unsigned n = tcp_build(f, net_mac(), g->src_mac, net_ip(), g->src_ip, g->dst_port, g->src_port, seq, ack, flags, 0,
                           0, net_next_ip_id(), 0, 0);
    if (n)
        net_send(f, n);
}

static Sock *find(const TcpSeg *g)
{
    for (int i = 0; i < TCP_MAX_SOCKETS; i++)
    {
        Sock *s = &SOCKS[i];
        if (s->used && s->state != ST_CLOSED && s->lport == g->dst_port && s->port == g->src_port &&
            same_ip(s->ip, g->src_ip))
            return s;
    }
    return 0;
}

/* The peer acknowledged up to `ack`. */
static void handle_ack(Sock *s, const TcpSeg *g)
{
    if (seq_lt(s->snd_una, g->ack) && seq_le(g->ack, s->snd_nxt))
    {
        s->snd_una = g->ack;
        if (s->snd_una == s->snd_nxt) /* our one segment is fully acknowledged */
            s->rtx = 0;
    }
    if (!seq_lt(g->ack, s->snd_una) && seq_le(g->ack, s->snd_nxt))
        s->peer_wnd = g->window;

    int all_acked = s->snd_una == s->snd_nxt; /* with FIN_WAIT_1 / CLOSING / LAST_ACK the FIN is the last thing sent */
    if (s->state == ST_FIN_WAIT_1 && all_acked)
        s->state = ST_FIN_WAIT_2;
    else if ((s->state == ST_CLOSING || s->state == ST_LAST_ACK) && all_acked)
    {
        s->state = ST_CLOSED; /* no TIME_WAIT: a lost last ACK makes the peer repeat its FIN and get a reset */
        s->rtx = 0;
    }
}

void tcp_input(const TcpSeg *g)
{
    Sock *s = find(g);
    if (!s)
    {
        if (!(g->flags & TCP_RST))
            send_rst_for(g);
        return;
    }

    if (s->state == ST_SYN_SENT)
    {
        if ((g->flags & TCP_ACK) && g->ack != s->snd_nxt) /* acknowledges something we never sent */
        {
            if (!(g->flags & TCP_RST))
                send_rst_for(g);
            return;
        }
        if (g->flags & TCP_RST)
        {
            if (g->flags & TCP_ACK) /* a refusal to our SYN */
                abort_conn(s);
            return;
        }
        if ((g->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK))
        {
            s->rcv_nxt = g->seq + 1;
            s->snd_una = g->ack;
            s->rtx = 0;
            s->peer_wnd = g->window;
            s->peer_mss = g->mss ? g->mss : DEFAULT_MSS;
            if (s->peer_mss > TCP_MAX_PAYLOAD)
                s->peer_mss = TCP_MAX_PAYLOAD;
            s->state = ST_ESTABLISHED;
            send_ack(s);
        }
        return;
    }

    /* the other states: the connection is synchronised */
    if (g->flags & TCP_RST)
    {
        /* only a reset inside the receive window counts, or anybody could kill the connection */
        unsigned wnd = rcv_window(s) ? rcv_window(s) : 1;
        if ((int)(g->seq - s->rcv_nxt) >= 0 && g->seq - s->rcv_nxt < wnd)
            abort_conn(s);
        return;
    }
    if (g->flags & TCP_SYN) /* a repeated SYN+ACK (our ACK was lost): answer again, nothing else */
    {
        send_ack(s);
        return;
    }
    if (!(g->flags & TCP_ACK))
        return;
    if (seq_lt(s->snd_nxt, g->ack)) /* acknowledges data we never sent */
    {
        send_ack(s);
        return;
    }
    handle_ack(s, g);
    if (s->state == ST_CLOSED)
        return;

    /* data and FIN. d = how many bytes at the start of this segment we already have */
    const unsigned char *p = g->payload;
    unsigned n = g->payload_len;
    int fin = (g->flags & TCP_FIN) != 0;
    if (n == 0 && !fin)
        return; /* a plain ACK */
    int d = (int)(s->rcv_nxt - g->seq);
    if (d < 0) /* a gap: bytes before this segment are missing. Drop it; the duplicate ACK tells the sender */
    {
        send_ack(s);
        return;
    }
    if ((unsigned)d >= n + (fin ? 1u : 0u)) /* all of it is old (a retransmission we already took) */
    {
        send_ack(s);
        return;
    }
    p += (unsigned)d < n ? (unsigned)d : n;
    n -= (unsigned)d < n ? (unsigned)d : n;

    if (s->state != ST_ESTABLISHED && s->state != ST_FIN_WAIT_1 && s->state != ST_FIN_WAIT_2)
    {
        send_ack(s); /* the peer already closed: no more data is allowed */
        return;
    }
    unsigned room = rcv_window(s);
    unsigned take = n < room ? n : room;
    for (unsigned i = 0; i < take; i++)
        s->rcv[(s->rcv_head + s->rcv_count + i) % RCV_BUF] = p[i];
    s->rcv_count += take;
    s->rcv_nxt += take;
    if (fin && take == n) /* the FIN comes after the data; only count it when all data fitted */
    {
        s->rcv_nxt++;
        s->rcv_fin = 1;
        if (s->state == ST_ESTABLISHED)
            s->state = ST_CLOSE_WAIT;
        else if (s->state == ST_FIN_WAIT_1)
            s->state = ST_CLOSING;
        else if (s->state == ST_FIN_WAIT_2)
            s->state = ST_CLOSED;
    }
    send_ack(s); /* the ACK number tells the sender what arrived; if the buffer was full it will send the rest again */
}

void tcp_tick(void)
{
    unsigned long long now = timer_ms();
    for (int i = 0; i < TCP_MAX_SOCKETS; i++)
    {
        Sock *s = &SOCKS[i];
        if (!s->used || s->state == ST_CLOSED || !s->rtx || now < s->rtx_at)
            continue;
        if (s->retries >= MAX_RETRIES)
        {
            abort_conn(s);
            continue;
        }
        send_raw(s, s->snd_una, s->rtx_flags, s->rtx_data, s->rtx_len);
        s->retries++;
        s->rto = s->rto * 2 > RTO_MAX_MS ? RTO_MAX_MS : s->rto * 2;
        s->rtx_at = now + s->rto;
    }
}

static int valid(int h) { return h >= 0 && h < TCP_MAX_SOCKETS && SOCKS[h].used; }

int tcp_connect(const unsigned char ip[4], unsigned port, unsigned timeout_ms)
{
    int h = -1;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++)
        if (!SOCKS[i].used)
        {
            h = i;
            break;
        }
    if (h < 0 || !net_present())
        return -1;
    Sock *s = &SOCKS[h];
    unsigned char mac[6];
    if (!net_arp_mac(ip, mac, 1000))
        return -1;

    volatile unsigned char *z = (volatile unsigned char *)s; /* no memset in a freestanding build */
    for (unsigned i = 0; i < sizeof *s; i++)
        z[i] = 0;
    s->used = 1;
    copy(s->ip, ip, 4);
    copy(s->mac, mac, 6);
    s->port = port;
    s->lport = NEXT_PORT++;
    if (NEXT_PORT > 60000)
        NEXT_PORT = 40000;
    /* the first sequence number: from the clock, so a new connection on a reused port does not mix with an old one
     * (not unpredictable enough for security, which is fine for now) */
    s->snd_una = s->snd_nxt = (unsigned)timer_us() * 250u + s->lport;
    s->state = ST_SYN_SENT;
    s->peer_mss = DEFAULT_MSS;
    emit(s, TCP_SYN, 0, 0);

    unsigned long long end = timer_ms() + timeout_ms;
    while (s->state == ST_SYN_SENT && timer_ms() < end)
        net_poll();
    if (s->state != ST_ESTABLISHED)
    {
        s->used = 0;
        s->state = ST_CLOSED;
        return -1;
    }
    return h;
}

/* Waits (polling) until the segment in flight is acknowledged. 1 if it was, 0 on timeout or a dead connection. */
static int wait_acked(Sock *s, unsigned long long end)
{
    while (s->rtx && s->state != ST_CLOSED && timer_ms() < end)
        net_poll();
    return !s->rtx && !s->err;
}

int tcp_send(int h, const void *data, unsigned len, unsigned timeout_ms)
{
    if (!valid(h))
        return -1;
    Sock *s = &SOCKS[h];
    const unsigned char *p = (const unsigned char *)data;
    unsigned long long end = timer_ms() + timeout_ms;
    unsigned done = 0;
    while (done < len)
    {
        if (!wait_acked(s, end))
            return -1;
        if (s->state != ST_ESTABLISHED && s->state != ST_CLOSE_WAIT)
            return -1;
        if (s->peer_wnd == 0) /* the peer cannot take anything now: wait for a window update */
        {
            if (timer_ms() >= end)
                return -1;
            net_poll();
            continue;
        }
        unsigned chunk = len - done;
        if (chunk > s->peer_mss)
            chunk = s->peer_mss;
        if (chunk > s->peer_wnd)
            chunk = s->peer_wnd;
        emit(s, TCP_ACK | TCP_PSH, p + done, chunk);
        done += chunk;
    }
    return wait_acked(s, end) ? (int)len : -1;
}

int tcp_recv(int h, void *buf, unsigned max, unsigned timeout_ms)
{
    if (!valid(h))
        return -1;
    Sock *s = &SOCKS[h];
    unsigned char *out = (unsigned char *)buf;
    unsigned long long end = timer_ms() + timeout_ms;
    for (;;)
    {
        if (s->rcv_count > 0 && max > 0)
        {
            unsigned before = rcv_window(s);
            unsigned n = s->rcv_count < max ? s->rcv_count : max;
            for (unsigned i = 0; i < n; i++)
                out[i] = s->rcv[(s->rcv_head + i) % RCV_BUF];
            s->rcv_head = (s->rcv_head + n) % RCV_BUF;
            s->rcv_count -= n;
            /* the peer may have stopped because our window was small: tell it there is room again */
            if (before < TCP_MAX_PAYLOAD && s->state != ST_CLOSED)
                send_ack(s);
            return (int)n;
        }
        if (s->rcv_fin)
            return 0;
        if (s->err || s->state == ST_CLOSED)
            return -1;
        if (timer_ms() >= end)
            return -2;
        net_poll();
    }
}

void tcp_close(int h)
{
    if (!valid(h))
        return;
    Sock *s = &SOCKS[h];
    unsigned long long end = timer_ms() + 2000;
    wait_acked(s, end); /* data first, then FIN */
    if (!s->rtx && (s->state == ST_ESTABLISHED || s->state == ST_CLOSE_WAIT))
    {
        s->state = s->state == ST_ESTABLISHED ? ST_FIN_WAIT_1 : ST_LAST_ACK;
        emit(s, TCP_FIN | TCP_ACK, 0, 0);
    }
    while (s->state != ST_CLOSED && timer_ms() < end)
        net_poll();
    if (s->state != ST_CLOSED && s->state != ST_SYN_SENT)
        send_raw(s, s->snd_nxt, TCP_RST | TCP_ACK, 0, 0); /* we give up waiting: tell the peer the connection is gone */
    s->used = 0;
    s->state = ST_CLOSED;
}
