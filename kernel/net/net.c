/* Network layer: ARP cache, answering ARP / ping, sending ping, boot self test. See include/net.h.
 *
 * ARP in short: to send an IP packet on Ethernet we need the MAC address of the next hop, and ARP asks for it:
 * a broadcast frame "who has 10.0.2.2? tell 10.0.2.15" and the owner answers with a unicast frame "10.0.2.2 is at
 * <mac>". Hosts remember answers in a small cache. A destination outside our subnet is sent to the gateway's MAC.
 *
 * Ping: an ICMP echo request inside an IPv4 packet; the target answers with an echo reply carrying the same id,
 * sequence number and data. The round trip time is the time between the two.
 *
 * Nothing runs in the background: net_poll() reads the frames that arrived and reacts. net_ping() and
 * net_self_test() call it while they wait. QEMU's user-mode network (-nic user) gives the guest 10.0.2.15,
 * and the gateway 10.0.2.2 answers ARP and ping. */
#include "net.h"
#include "net_internal.h"
#include "netproto.h"
#include "serial.h"
#include "tcp.h"
#include "timer.h"

#define ARP_CACHE_SIZE 8

typedef struct
{
    unsigned char ip[4], mac[6];
    int valid;
} ArpEntry;

static unsigned char OUR_IP[4]; /* 0.0.0.0 until DHCP (or the fallback) sets it */
static unsigned char NETMASK[4] = {255, 255, 255, 0};
static unsigned char GATEWAY[4];
static unsigned char DNS[4];
static const unsigned char BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static ArpEntry CACHE[ARP_CACHE_SIZE];
static unsigned CACHE_NEXT; /* round robin: the slot to overwrite when the cache is full */
static unsigned IP_ID;      /* IPv4 identification of the next packet we send */

/* the echo reply net_ping() is waiting for */
static int PING_WAIT;
static unsigned PING_ID, PING_SEQ;
static int PING_GOT;

/* the DHCP answer net_dhcp() is waiting for: parsed in place, only when its transaction id is ours */
static int DHCP_WAIT;
static unsigned DHCP_XID;
static int DHCP_HAVE;
static DhcpMsg DHCP_RX;

/* the DNS answer net_resolve() is waiting for */
static int DNS_WAIT;
static unsigned DNS_ID, DNS_SPORT;
static int DNS_STATE; /* 0 = waiting, 1 = got an address in DNS_IP, -1 = the server says there is none */
static unsigned char DNS_IP[4];

static int IP_SOURCE; /* 0 = none yet, 1 = DHCP, 2 = the fixed fallback of the self test */
int net_ip_source(void) { return IP_SOURCE; }
const unsigned char *net_ip(void) { return OUR_IP; }
const unsigned char *net_gateway(void) { return GATEWAY; }
const unsigned char *net_dns(void) { return DNS; }

static int same(const unsigned char *a, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void copy(unsigned char *d, const unsigned char *s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = s[i];
}

static void put_mac(const unsigned char *m)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++)
    {
        serial_putc(hex[m[i] >> 4]);
        serial_putc(hex[m[i] & 15]);
        if (i < 5)
            serial_putc(':');
    }
}

static void put_ip(const unsigned char *ip)
{
    for (int i = 0; i < 4; i++)
    {
        serial_putd(ip[i]);
        if (i < 3)
            serial_putc('.');
    }
}

static const ArpEntry *cache_find(const unsigned char *ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (CACHE[i].valid && same(CACHE[i].ip, ip, 4))
            return &CACHE[i];
    return 0;
}

static void cache_put(const unsigned char *ip, const unsigned char *mac)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (CACHE[i].valid && same(CACHE[i].ip, ip, 4))
        {
            copy(CACHE[i].mac, mac, 6); /* the owner may have changed its card */
            return;
        }
    ArpEntry *e = &CACHE[CACHE_NEXT];
    CACHE_NEXT = (CACHE_NEXT + 1) % ARP_CACHE_SIZE;
    copy(e->ip, ip, 4);
    copy(e->mac, mac, 6);
    e->valid = 1;
}

static void handle_arp(const ArpPacket *a)
{
    /* never learn a broadcast / zero address or our own: a forged entry could redirect our traffic */
    if (!same(a->sender_ip, OUR_IP, 4) && !same(a->sender_mac, BROADCAST_MAC, 6))
        cache_put(a->sender_ip, a->sender_mac);
    if (a->op == ARP_OP_REQUEST && same(a->target_ip, OUR_IP, 4))
    {
        unsigned char f[ARP_FRAME_LEN];
        unsigned n = arp_build(f, ARP_OP_REPLY, net_mac(), OUR_IP, a->sender_mac, a->sender_ip);
        net_send(f, n);
    }
}

static void handle_echo(const IcmpEcho *e)
{
    if (!same(e->dst_ip, OUR_IP, 4))
        return;
    if (e->type == ICMP_ECHO_REQUEST)
    {
        unsigned char f[ETH_HDR_LEN + IP_HDR_LEN + ICMP_HDR_LEN + ICMP_MAX_PAYLOAD];
        unsigned n = icmp_echo_build(f, net_mac(), e->src_mac, OUR_IP, e->src_ip, ICMP_ECHO_REPLY, e->id, e->seq,
                                     IP_ID++, e->payload, e->payload_len > ICMP_MAX_PAYLOAD ? ICMP_MAX_PAYLOAD : e->payload_len);
        if (n)
            net_send(f, n);
    }
    else if (PING_WAIT && e->id == PING_ID && e->seq == PING_SEQ)
        PING_GOT = 1;
}

static void handle_udp(const UdpPacket *u)
{
    /* DHCP answers come from port 67 to port 68, before we own an address, so the destination ip is not checked */
    if (DHCP_WAIT && !DHCP_HAVE && u->src_port == DHCP_SERVER_PORT && u->dst_port == DHCP_CLIENT_PORT)
    {
        if (dhcp_parse(u->payload, u->payload_len, &DHCP_RX) && DHCP_RX.xid == DHCP_XID)
            DHCP_HAVE = 1;
    }
    /* a DNS answer: from our DNS server's port 53 to the port we asked from, addressed to us. Anyone else could
     * guess an id, but not easily our source port and server address too. */
    else if (DNS_WAIT && DNS_STATE == 0 && u->src_port == DNS_PORT && u->dst_port == DNS_SPORT &&
             same(u->src_ip, DNS, 4) && same(u->dst_ip, OUR_IP, 4))
    {
        unsigned char ip[4];
        int r = dns_parse_a(u->payload, u->payload_len, DNS_ID, ip);
        if (r == 1)
            copy(DNS_IP, ip, 4);
        if (r != 0)
            DNS_STATE = r;
    }
}

int net_poll(void)
{
    unsigned char r[NET_MAX_FRAME];
    int handled = 0;
    int n;
    /* bounded: a flood cannot keep us here forever (the ring holds 16 frames anyway) */
    for (int i = 0; i < 16 && (n = net_recv(r, sizeof r)) > 0; i++)
    {
        ArpPacket a;
        IcmpEcho e;
        UdpPacket u;
        TcpSeg t;
        if (arp_parse(r, (unsigned)n, &a))
            handle_arp(&a);
        else if (icmp_echo_parse(r, (unsigned)n, &e))
            handle_echo(&e);
        else if (udp_parse(r, (unsigned)n, &u))
            handle_udp(&u);
        else if (tcp_parse(r, (unsigned)n, &t) && same(t.dst_ip, OUR_IP, 4))
            tcp_input(&t);
        handled++;
    }
    tcp_tick(); /* retransmission timers run whenever somebody waits for the network */
    return handled;
}

/* The MAC to put in the Ethernet header for a packet to `ip`: ask ARP (up to 3 tries). 1 and *mac, or 0. */
static int resolve(const unsigned char *ip, unsigned char *mac, unsigned timeout_ms)
{
    const unsigned char *hop = ip;
    int on_link = 1;
    for (int i = 0; i < 4; i++)
        if ((ip[i] & NETMASK[i]) != (OUR_IP[i] & NETMASK[i]))
            on_link = 0;
    if (!on_link)
        hop = GATEWAY;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        const ArpEntry *e = cache_find(hop);
        if (e)
        {
            copy(mac, e->mac, 6);
            return 1;
        }
        unsigned char f[ARP_FRAME_LEN];
        unsigned n = arp_build(f, ARP_OP_REQUEST, net_mac(), OUR_IP, BROADCAST_MAC, hop);
        if (net_send(f, n) != 0)
            return 0;
        unsigned long long end = timer_ms() + timeout_ms;
        while (timer_ms() < end && !cache_find(hop))
        {
            net_poll();
            net_yield();
        }
    }
    return 0;
}

int net_ping(const unsigned char ip[4], unsigned seq, unsigned timeout_ms)
{
    if (!net_present())
        return -1;
    unsigned char mac[6];
    if (!resolve(ip, mac, timeout_ms))
        return -1;

    static const unsigned char data[] = "HarisOS ping 0123456789abcdef"; /* 29 bytes with the final 0 */
    unsigned char f[ETH_HDR_LEN + IP_HDR_LEN + ICMP_HDR_LEN + sizeof data];
    PING_ID = 0x4849; /* "HI" */
    PING_SEQ = seq & 0xFFFF;
    unsigned n = icmp_echo_build(f, net_mac(), mac, OUR_IP, ip, ICMP_ECHO_REQUEST, PING_ID, PING_SEQ, IP_ID++, data,
                                 sizeof data);
    PING_GOT = 0;
    PING_WAIT = 1;
    unsigned long long start = timer_us();
    int rtt = -1;
    if (net_send(f, n) == 0)
    {
        unsigned long long end = timer_ms() + timeout_ms;
        while (timer_ms() < end && !PING_GOT)
        {
            net_poll();
            net_yield();
        }
        if (PING_GOT)
        {
            unsigned long long us = timer_us() - start;
            rtt = (int)((us + 999) / 1000); /* whole milliseconds, rounded up: 0 would look like "no answer" */
        }
    }
    PING_WAIT = 0;
    return rtt;
}

/* Sends a UDP datagram to `ip` (ARP first when the next hop's MAC is unknown). 0 on success. */
static int udp_send(const unsigned char *ip, unsigned sport, unsigned dport, const unsigned char *data, unsigned len,
                    unsigned arp_timeout_ms)
{
    unsigned char mac[6];
    unsigned char f[ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + DNS_MAX_QUERY];
    if (len > DNS_MAX_QUERY || !resolve(ip, mac, arp_timeout_ms))
        return -1;
    unsigned n = udp_build(f, net_mac(), mac, OUR_IP, ip, sport, dport, IP_ID++, data, len);
    return n ? net_send(f, n) : -1;
}

/* DNS in two messages: we ask the server "what is the A record of example.com?" in a UDP datagram from a port we pick,
 * and it answers from port 53 to that port with the address (a recursive server like QEMU's 10.0.2.3 asks the
 * internet for us). Names written as dotted numbers are taken as they are. Returns 1 and the address, else 0. */
static int resolve_name(const char *name, unsigned char ip[4], unsigned timeout_ms)
{
    if (ip_parse(name, ip))
        return 1;
    if (!net_present() || (DNS[0] | DNS[1] | DNS[2] | DNS[3]) == 0)
        return 0;
    unsigned char q[DNS_MAX_QUERY];
    for (int attempt = 0; attempt < 2; attempt++)
    {
        /* a new id and source port per try: an old, late answer then no longer matches */
        unsigned r = (unsigned)timer_us() ^ ((unsigned)attempt * 0x9E37u);
        DNS_ID = r & 0xFFFF;
        DNS_SPORT = 49152 + (r >> 8) % 16000;
        unsigned n = dns_build_query(q, DNS_ID, name);
        if (!n)
            return 0;
        DNS_STATE = 0;
        DNS_WAIT = 1;
        if (udp_send(DNS, DNS_SPORT, DNS_PORT, q, n, timeout_ms) == 0)
        {
            unsigned long long end = timer_ms() + timeout_ms;
            while (timer_ms() < end && DNS_STATE == 0)
            {
                net_poll();
                net_yield();
            }
        }
        DNS_WAIT = 0;
        if (DNS_STATE == 1)
        {
            copy(ip, DNS_IP, 4);
            return 1;
        }
        if (DNS_STATE == -1) /* a definite "no": asking again would get the same answer */
            return 0;
    }
    return 0;
}

/* One DNS question at a time: the answer state above is shared, so a second process asking while the first waits
 * (they run while the first yields) must wait its turn. */
static int DNS_BUSY;

int net_resolve(const char *name, unsigned char ip[4], unsigned timeout_ms)
{
    if (ip_parse(name, ip)) /* no lock needed for a dotted number */
        return 1;
    while (DNS_BUSY)
        net_yield();
    DNS_BUSY = 1;
    int r = resolve_name(name, ip, timeout_ms);
    DNS_BUSY = 0;
    return r;
}

/* Sends one DHCP client message: from 0.0.0.0 to 255.255.255.255, Ethernet broadcast, port 68 to port 67. */
static int dhcp_send(unsigned type, const unsigned char *requested_ip, const unsigned char *server_id)
{
    static const unsigned char any[4] = {0, 0, 0, 0}, all[4] = {255, 255, 255, 255};
    unsigned char payload[DHCP_MIN_LEN];
    unsigned char f[ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + DHCP_MIN_LEN];
    unsigned pn = dhcp_build(payload, type, net_mac(), DHCP_XID, requested_ip, server_id);
    unsigned n = udp_build(f, net_mac(), BROADCAST_MAC, any, all, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, IP_ID++, payload, pn);
    return n ? net_send(f, n) : -1;
}

/* Waits for a DHCP message of type `want` (or a NAK); 1 when it arrived, 0 on timeout. */
static int dhcp_wait(unsigned want, unsigned timeout_ms)
{
    unsigned long long end = timer_ms() + timeout_ms;
    while (timer_ms() < end)
    {
        net_poll();
        net_yield();
        if (DHCP_HAVE)
        {
            if (DHCP_RX.type == want || DHCP_RX.type == DHCP_NAK)
                return 1;
            DHCP_HAVE = 0; /* some other message with our id (a second offer, say): keep waiting */
        }
    }
    return 0;
}

/* DHCP in four messages: we broadcast DISCOVER ("any server there?"), a server answers OFFER ("you could have
 * 10.0.2.15"), we broadcast REQUEST ("I take 10.0.2.15 from server X") and the server confirms with ACK, which also
 * carries the subnet mask, the router (gateway), the DNS server and the lease time. */
int net_dhcp(unsigned timeout_ms)
{
    if (!net_present())
        return 0;
    for (int attempt = 0; attempt < 3; attempt++)
    {
        /* a fresh transaction id each try, so a late answer to an older try is ignored */
        DHCP_XID = (unsigned)timer_us() ^ ((unsigned)net_mac()[5] << 8 | net_mac()[4]) ^ (unsigned)attempt;
        DHCP_HAVE = 0;
        DHCP_WAIT = 1;

        int got = dhcp_send(DHCP_DISCOVER, 0, 0) == 0 && dhcp_wait(DHCP_OFFER, timeout_ms) && DHCP_RX.type == DHCP_OFFER &&
                  DHCP_RX.has_server;
        if (got)
        {
            unsigned char offered[4], server[4];
            copy(offered, DHCP_RX.yiaddr, 4);
            copy(server, DHCP_RX.server_id, 4);
            DHCP_HAVE = 0;
            got = dhcp_send(DHCP_REQUEST, offered, server) == 0 && dhcp_wait(DHCP_ACK, timeout_ms) &&
                  DHCP_RX.type == DHCP_ACK;
        }
        if (got)
        {
            copy(OUR_IP, DHCP_RX.yiaddr, 4);
            if (DHCP_RX.has_mask)
                copy(NETMASK, DHCP_RX.mask, 4);
            if (DHCP_RX.has_router)
                copy(GATEWAY, DHCP_RX.router, 4);
            if (DHCP_RX.has_dns)
                copy(DNS, DHCP_RX.dns, 4);
            for (int i = 0; i < ARP_CACHE_SIZE; i++)
                CACHE[i].valid = 0; /* entries from an older network are wrong now */
            DHCP_WAIT = 0;
            IP_SOURCE = 1;
            return 1;
        }
    }
    DHCP_WAIT = 0;
    return 0;
}

/* TCP check: ask a web server for its front page and report the status line and how many bytes came back. */
static void http_test(const unsigned char *ip)
{
    static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    int s = tcp_connect(ip, 80, 3000);
    if (s < 0)
    {
        serial_puts("HARISOS: network: tcp connect failed\n");
        return;
    }
    if (tcp_send(s, req, sizeof req - 1, 3000) < 0)
    {
        serial_puts("HARISOS: network: tcp send failed\n");
        tcp_close(s);
        return;
    }
    unsigned char buf[512];
    unsigned total = 0;
    int n;
    serial_puts("HARISOS: network: http ");
    while ((n = tcp_recv(s, buf, sizeof buf, 3000)) > 0)
    {
        if (total == 0) /* the status line, e.g. "HTTP/1.0 200 OK" */
            for (int i = 0; i < n && i < 60 && buf[i] != '\r' && buf[i] != '\n'; i++)
                serial_putc((char)buf[i]);
        total += (unsigned)n;
    }
    serial_puts(", ");
    serial_putd(total);
    serial_puts(n == 0 ? " bytes, closed by the server\n" : " bytes, then an error or timeout\n");
    tcp_close(s);
}

/* The address of the machine: from DHCP, else the one QEMU's user network would have given. Not a test: without it there is no IP. */
void net_configure(void)
{
    serial_puts("HARISOS: network: ");
    serial_puts(net_driver_name());
    serial_puts(" mac ");
    put_mac(net_mac());
    serial_puts(net_link_up() ? ", link up\n" : ", link down\n");

    if (net_dhcp(6000)) /* (a Wi-Fi link can need a few seconds for the first answer) */
        serial_puts("HARISOS: network: dhcp ok, ip ");
    else
    {
        /* no DHCP server: use the address QEMU's user network would have given */
        static const unsigned char ip[4] = {10, 0, 2, 15}, gw[4] = {10, 0, 2, 2}, dns[4] = {10, 0, 2, 3};
        copy(OUR_IP, ip, 4);
        IP_SOURCE = 2;
        copy(GATEWAY, gw, 4);
        copy(DNS, dns, 4);
        serial_puts("HARISOS: network: dhcp failed, static ip ");
    }
    put_ip(OUR_IP);
    serial_puts(", gateway ");
    put_ip(GATEWAY);
    serial_puts(", dns ");
    put_ip(DNS);
    serial_puts(", mask ");
    put_ip(NETMASK);
    serial_puts("\n");
}

int net_self_test(void)
{
    if (!net_present())
    {
        serial_puts("HARISOS: network: no supported adapter\n");
        return 0;
    }
    net_configure();

    /* the first ARP answer can take a second in QEMU (its TCG main loop is slow to deliver), so wait generously */
    int ok = 0;
    for (unsigned seq = 1; seq <= 3; seq++)
    {
        int rtt = net_ping(GATEWAY, seq, 3000);
        serial_puts("HARISOS: network: ping ");
        put_ip(GATEWAY);
        if (rtt < 0)
            serial_puts(" no reply\n");
        else
        {
            serial_puts(" reply, ");
            serial_putd(rtt);
            serial_puts(" ms\n");
            ok++;
        }
    }
    /* DNS: a dotted number must come back unchanged, a name needs the internet (the result is only reported) */
    unsigned char addr[4];
    if (net_resolve("10.1.2.3", addr, 100) && addr[3] == 3)
        serial_puts("HARISOS: network: dns literal ok\n");
    serial_puts("HARISOS: network: dns example.com ");
    if (net_resolve("example.com", addr, 1500))
    {
        put_ip(addr);
        serial_puts("\n");
        http_test(addr);
    }
    else
        serial_puts("not resolved (no internet, or no DNS server)\n");

    const ArpEntry *g = cache_find(GATEWAY);
    if (g)
    {
        serial_puts("HARISOS: network: gateway is at ");
        put_mac(g->mac);
        serial_puts("\n");
    }
    return ok;
}

/* for tcp.c (see net_internal.h) */
int net_arp_mac(const unsigned char ip[4], unsigned char mac[6], unsigned timeout_ms)
{
    return resolve(ip, mac, timeout_ms);
}

unsigned net_next_ip_id(void) { return IP_ID++; }

static void (*YIELD_FN)(void);
void net_set_yield(void (*fn)(void)) { YIELD_FN = fn; }
void net_yield(void)
{
    if (YIELD_FN)
        YIELD_FN();
}
