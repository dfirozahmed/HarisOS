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
#include "netproto.h"
#include "serial.h"
#include "timer.h"

#define ARP_CACHE_SIZE 8

typedef struct
{
    unsigned char ip[4], mac[6];
    int valid;
} ArpEntry;

static const unsigned char OUR_IP[4] = {10, 0, 2, 15};
static const unsigned char NETMASK[4] = {255, 255, 255, 0};
static const unsigned char GATEWAY[4] = {10, 0, 2, 2};
static const unsigned char BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static ArpEntry CACHE[ARP_CACHE_SIZE];
static unsigned CACHE_NEXT; /* round robin: the slot to overwrite when the cache is full */
static unsigned IP_ID;      /* IPv4 identification of the next packet we send */

/* the echo reply net_ping() is waiting for */
static int PING_WAIT;
static unsigned PING_ID, PING_SEQ;
static int PING_GOT;

const unsigned char *net_ip(void) { return OUR_IP; }

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
        if (arp_parse(r, (unsigned)n, &a))
            handle_arp(&a);
        else if (icmp_echo_parse(r, (unsigned)n, &e))
            handle_echo(&e);
        handled++;
    }
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
            net_poll();
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
            net_poll();
        if (PING_GOT)
        {
            unsigned long long us = timer_us() - start;
            rtt = (int)((us + 999) / 1000); /* whole milliseconds, rounded up: 0 would look like "no answer" */
        }
    }
    PING_WAIT = 0;
    return rtt;
}

int net_self_test(void)
{
    if (!net_present())
    {
        serial_puts("HARISOS: network: no supported adapter\n");
        return 0;
    }
    serial_puts("HARISOS: network: e1000 mac ");
    put_mac(net_mac());
    serial_puts(net_link_up() ? ", link up, ip " : ", link down, ip ");
    put_ip(OUR_IP);
    serial_puts("\n");

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
    const ArpEntry *g = cache_find(GATEWAY);
    if (g)
    {
        serial_puts("HARISOS: network: gateway is at ");
        put_mac(g->mac);
        serial_puts("\n");
    }
    return ok;
}
