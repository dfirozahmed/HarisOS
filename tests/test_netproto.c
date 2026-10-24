/* Host test for proto.c: checksum, ARP and ICMP echo frames. */
#include <stdio.h>
#include <string.h>
#include "netproto.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const unsigned char MAC_A[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const unsigned char MAC_B[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
static const unsigned char IP_A[4] = {10, 0, 2, 15};
static const unsigned char IP_B[4] = {10, 0, 2, 2};

int main(void)
{
    /* RFC 1071 example: words 0001 f203 f4f5 f6f7 sum to ddf2, so the checksum is 220d */
    const unsigned char rfc[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK(net_checksum(rfc, sizeof rfc) == 0x220d);
    /* a well known IPv4 header (checksum field zeroed): the checksum is b861 */
    const unsigned char hdr[] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                                 0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7};
    CHECK(net_checksum(hdr, sizeof hdr) == 0xb861);
    /* odd length: the last byte counts as the high byte of a word */
    const unsigned char odd[] = {0x12, 0x34, 0x56};
    CHECK(net_checksum(odd, 3) == (unsigned short)~(0x1234 + 0x5600));
    CHECK(net_checksum(odd, 0) == 0xFFFF);

    /* ARP request round trip */
    unsigned char f[1600];
    ArpPacket a;
    CHECK(arp_build(f, ARP_OP_REQUEST, MAC_A, IP_A, MAC_A, IP_B) == ARP_FRAME_LEN);
    CHECK(f[0] == 0xFF && f[5] == 0xFF && memcmp(f + 6, MAC_A, 6) == 0);
    CHECK(f[12] == 0x08 && f[13] == 0x06);
    CHECK(arp_parse(f, ARP_FRAME_LEN, &a));
    CHECK(a.op == ARP_OP_REQUEST && memcmp(a.sender_mac, MAC_A, 6) == 0 && memcmp(a.sender_ip, IP_A, 4) == 0);
    CHECK(memcmp(a.target_ip, IP_B, 4) == 0);
    CHECK(!arp_parse(f, ARP_FRAME_LEN - 1, &a)); /* too short */
    f[21] = 7;                                   /* unknown operation */
    CHECK(!arp_parse(f, ARP_FRAME_LEN, &a));

    /* ARP reply goes to the asker's MAC */
    CHECK(arp_build(f, ARP_OP_REPLY, MAC_B, IP_B, MAC_A, IP_A) == ARP_FRAME_LEN);
    CHECK(memcmp(f, MAC_A, 6) == 0);
    CHECK(arp_parse(f, ARP_FRAME_LEN, &a) && a.op == ARP_OP_REPLY && memcmp(a.target_mac, MAC_A, 6) == 0);

    /* ICMP echo request round trip, odd payload length */
    const unsigned char data[] = "HarisOS ping!";
    unsigned n = icmp_echo_build(f, MAC_A, MAC_B, IP_A, IP_B, ICMP_ECHO_REQUEST, 0x1234, 7, 99, data, sizeof data);
    CHECK(n == ETH_HDR_LEN + IP_HDR_LEN + ICMP_HDR_LEN + sizeof data);
    CHECK(net_checksum(f + ETH_HDR_LEN, IP_HDR_LEN) == 0); /* the IP header sums to zero */
    IcmpEcho e;
    CHECK(icmp_echo_parse(f, n, &e));
    CHECK(e.type == ICMP_ECHO_REQUEST && e.id == 0x1234 && e.seq == 7);
    CHECK(memcmp(e.src_ip, IP_A, 4) == 0 && memcmp(e.dst_ip, IP_B, 4) == 0);
    CHECK(memcmp(e.src_mac, MAC_A, 6) == 0 && memcmp(e.dst_mac, MAC_B, 6) == 0);
    CHECK(e.payload_len == sizeof data && memcmp(e.payload, data, sizeof data) == 0);

    /* a frame padded to the Ethernet minimum (trailing bytes after the IP length) still parses */
    unsigned char padded[200];
    memcpy(padded, f, n);
    memset(padded + n, 0, 20);
    CHECK(icmp_echo_parse(padded, n + 20, &e) && e.payload_len == sizeof data);

    /* corruption is caught: payload byte, IP header byte, truncation, not-IPv4, fragment */
    unsigned char bad[200];
    memcpy(bad, f, n);
    bad[n - 1] ^= 1;
    CHECK(!icmp_echo_parse(bad, n, &e));
    memcpy(bad, f, n);
    bad[ETH_HDR_LEN + 8] = 1; /* TTL changed, checksum now wrong */
    CHECK(!icmp_echo_parse(bad, n, &e));
    CHECK(!icmp_echo_parse(f, n - 1, &e)); /* IP length says more than the frame has */
    memcpy(bad, f, n);
    bad[12] = 0x86; bad[13] = 0xDD; /* IPv6 */
    CHECK(!icmp_echo_parse(bad, n, &e));
    memcpy(bad, f, n);
    bad[ETH_HDR_LEN + 6] = 0x20; /* more-fragments flag; fix the checksum so only the flag is wrong */
    bad[ETH_HDR_LEN + 10] = bad[ETH_HDR_LEN + 11] = 0;
    unsigned short c = net_checksum(bad + ETH_HDR_LEN, IP_HDR_LEN);
    bad[ETH_HDR_LEN + 10] = (unsigned char)(c >> 8);
    bad[ETH_HDR_LEN + 11] = (unsigned char)c;
    CHECK(!icmp_echo_parse(bad, n, &e));

    /* reply type and the size limit */
    n = icmp_echo_build(f, MAC_B, MAC_A, IP_B, IP_A, ICMP_ECHO_REPLY, 1, 2, 3, data, 0);
    CHECK(n == ETH_HDR_LEN + IP_HDR_LEN + ICMP_HDR_LEN);
    CHECK(icmp_echo_parse(f, n, &e) && e.type == ICMP_ECHO_REPLY && e.payload_len == 0);
    CHECK(icmp_echo_build(f, MAC_B, MAC_A, IP_B, IP_A, ICMP_ECHO_REQUEST, 1, 2, 3, data, ICMP_MAX_PAYLOAD + 1) == 0);


    /* UDP round trip, odd and even payload lengths, with the pseudo header checksum */
    UdpPacket u;
    const unsigned char blob[] = "12345";
    n = udp_build(f, MAC_A, MAC_B, IP_A, IP_B, 5353, 53, 5, blob, 5);
    CHECK(n == ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + 5);
    CHECK(udp_parse(f, n, &u) && u.src_port == 5353 && u.dst_port == 53 && u.payload_len == 5);
    CHECK(memcmp(u.payload, blob, 5) == 0 && memcmp(u.src_ip, IP_A, 4) == 0 && memcmp(u.dst_mac, MAC_B, 6) == 0);
    n = udp_build(f, MAC_A, MAC_B, IP_A, IP_B, 1, 2, 5, blob, 4);
    CHECK(udp_parse(f, n, &u) && u.payload_len == 4);
    CHECK(net_checksum(f + ETH_HDR_LEN, IP_HDR_LEN) == 0);
    memcpy(bad, f, n);
    bad[n - 1] ^= 0x10; /* flipped payload bit: the UDP checksum catches it */
    CHECK(!udp_parse(bad, n, &u));
    memcpy(bad, f, n);
    bad[ETH_HDR_LEN + IP_HDR_LEN + 6] = bad[ETH_HDR_LEN + IP_HDR_LEN + 7] = 0; /* checksum 0 = none: accepted */
    bad[n - 1] ^= 0x10;
    CHECK(udp_parse(bad, n, &u));
    memcpy(bad, f, n);
    bad[ETH_HDR_LEN + IP_HDR_LEN + 4] = 0xFF; /* UDP length bigger than the IP packet */
    CHECK(!udp_parse(bad, n, &u));
    CHECK(!udp_parse(f, ETH_HDR_LEN + IP_HDR_LEN + 3, &u));
    CHECK(udp_build(f, MAC_A, MAC_B, IP_A, IP_B, 1, 2, 5, blob, UDP_MAX_PAYLOAD + 1) == 0);

    /* DHCP: DISCOVER layout, then parse a hand made OFFER */
    unsigned char d[DHCP_MIN_LEN];
    CHECK(dhcp_build(d, DHCP_DISCOVER, MAC_A, 0xCAFEBABEu, NULL, NULL) == DHCP_MIN_LEN);
    CHECK(d[0] == 1 && d[4] == 0xCA && d[7] == 0xBE && d[10] == 0x80 && memcmp(d + 28, MAC_A, 6) == 0);
    CHECK(d[236] == 0x63 && d[239] == 0x63 && d[240] == 53 && d[241] == 1 && d[242] == DHCP_DISCOVER);
    DhcpMsg m;
    CHECK(!dhcp_parse(d, DHCP_MIN_LEN, &m)); /* a request is not a reply */
    dhcp_build(d, DHCP_REQUEST, MAC_A, 1, IP_A, IP_B);
    CHECK(d[243] == 50 && memcmp(d + 245, IP_A, 4) == 0 && d[249] == 54 && memcmp(d + 251, IP_B, 4) == 0);

    unsigned char o[DHCP_MIN_LEN] = {0};
    o[0] = 2; o[1] = 1; o[2] = 6;
    o[4] = 0xCA; o[5] = 0xFE; o[6] = 0xBA; o[7] = 0xBE;
    memcpy(o + 16, IP_A, 4);
    o[236] = 0x63; o[237] = 0x82; o[238] = 0x53; o[239] = 0x63;
    const unsigned char opts[] = {53, 1, DHCP_OFFER, 0, 54, 4, 10, 0, 2, 2, 1, 4, 255, 255, 255, 0,
                                  3, 8, 10, 0, 2, 2, 10, 0, 2, 9, 6, 4, 10, 0, 2, 3,
                                  51, 4, 0, 1, 0x51, 0x80, 255};
    memcpy(o + 240, opts, sizeof opts);
    CHECK(dhcp_parse(o, sizeof o, &m));
    CHECK(m.type == DHCP_OFFER && m.xid == 0xCAFEBABEu && memcmp(m.yiaddr, IP_A, 4) == 0);
    CHECK(m.has_server && memcmp(m.server_id, IP_B, 4) == 0 && m.has_mask && m.mask[0] == 255 && m.mask[3] == 0);
    CHECK(m.has_router && memcmp(m.router, IP_B, 4) == 0); /* the first of two routers */
    CHECK(m.has_dns && m.dns[3] == 3 && m.lease_s == 86400);
    unsigned char bad_opt[DHCP_MIN_LEN];
    memcpy(bad_opt, o, sizeof o);
    bad_opt[240 + 1] = 200; /* option length past the end of the message */
    CHECK(!dhcp_parse(bad_opt, sizeof bad_opt, &m));
    memcpy(bad_opt, o, sizeof o);
    bad_opt[236] = 0; /* no magic cookie */
    CHECK(!dhcp_parse(bad_opt, sizeof bad_opt, &m));
    CHECK(!dhcp_parse(o, 100, &m));

    /* DNS query layout: header, 7 'example' 3 'com' 0, type A, class IN */
    unsigned char q[DNS_MAX_QUERY];
    unsigned ql = dns_build_query(q, 0xBEEF, "example.com");
    CHECK(ql == 12 + 1 + 7 + 1 + 3 + 1 + 4);
    CHECK(q[0] == 0xBE && q[1] == 0xEF && q[2] == 0x01 && q[3] == 0x00 && q[5] == 1 && q[7] == 0);
    CHECK(q[12] == 7 && memcmp(q + 13, "example", 7) == 0 && q[20] == 3 && memcmp(q + 21, "com", 3) == 0 && q[24] == 0);
    CHECK(q[25] == 0 && q[26] == 1 && q[27] == 0 && q[28] == 1);
    CHECK(dns_build_query(q, 1, "example.com.") == ql); /* trailing dot is the same name */
    CHECK(dns_build_query(q, 1, "localhost") == 12 + 1 + 9 + 1 + 4); /* a single label works */
    CHECK(dns_build_query(q, 1, "") == 0 && dns_build_query(q, 1, ".") == 0);
    CHECK(dns_build_query(q, 1, "a..b") == 0 && dns_build_query(q, 1, ".a.b") == 0);
    CHECK(dns_build_query(q, 1, "bad name.com") == 0 && dns_build_query(q, 1, "a/b.com") == 0);
    char big[300];
    memset(big, 'a', 64); big[64] = 0;
    CHECK(dns_build_query(q, 1, big) == 0); /* label of 64 */
    memset(big, 'a', 63); big[63] = 0;
    CHECK(dns_build_query(q, 1, big) == 12 + 1 + 63 + 1 + 4);
    /* whole name: 4 labels of 63 = 4*64+1 = 257 encoded bytes -> too long; 3 labels of 63 + a short one fits */
    char longname[300];
    int ln = 0;
    for (int k = 0; k < 4; k++)
    {
        for (int i = 0; i < 63; i++)
            longname[ln++] = 'a';
        longname[ln++] = '.';
    }
    longname[ln - 1] = 0;
    CHECK(dns_build_query(q, 1, longname) == 0);
    longname[63 * 3 + 3] = 0; /* three labels: 3*64+1 = 193 encoded bytes */
    CHECK(dns_build_query(q, 1, longname) == 12 + 193 + 4);

    /* DNS answers: build one by hand (query copy + CNAME + A with a compression pointer) */
    unsigned char r[512];
    unsigned rl = dns_build_query(r, 0x1234, "www.example.com");
    r[2] = 0x81; r[3] = 0x80; /* response, recursion desired + available, RCODE 0 */
    r[7] = 2;                 /* two answers */
    const unsigned char cname[] = {0xC0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 2, 0xC0, 12}; /* CNAME -> pointer */
    const unsigned char arec[] = {0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 93, 184, 216, 34};
    memcpy(r + rl, cname, sizeof cname);
    memcpy(r + rl + sizeof cname, arec, sizeof arec);
    unsigned rn = rl + sizeof cname + sizeof arec;
    unsigned char got[4];
    CHECK(dns_parse_a(r, rn, 0x1234, got) == 1 && got[0] == 93 && got[1] == 184 && got[2] == 216 && got[3] == 34);
    CHECK(dns_parse_a(r, rn, 0x1235, got) == 0);     /* another query's id */
    CHECK(dns_parse_a(r, rn - 1, 0x1234, got) == 0); /* cut inside the last record */
    CHECK(dns_parse_a(r, 11, 0x1234, got) == 0);
    unsigned char t[512];
    memcpy(t, r, rn);
    t[2] = 0x01; /* QR bit clear: that is a query, not an answer */
    CHECK(dns_parse_a(t, rn, 0x1234, got) == 0);
    memcpy(t, r, rn);
    t[3] = 0x83; /* RCODE 3: name does not exist */
    CHECK(dns_parse_a(t, rn, 0x1234, got) == -1);
    memcpy(t, r, rn);
    t[7] = 1; /* only the CNAME is counted: no address */
    CHECK(dns_parse_a(t, rn, 0x1234, got) == -1);
    memcpy(t, r, rn);
    t[rl + 11] = 0xFF; /* CNAME data length far beyond the message */
    CHECK(dns_parse_a(t, rn, 0x1234, got) == 0);
    memcpy(t, r, rn);
    t[12] = 0x80; /* a reserved label type in the question */
    CHECK(dns_parse_a(t, rn, 0x1234, got) == 0);

    /* IPv4 text */
    unsigned char pip[4];
    CHECK(ip_parse("10.0.2.15", pip) && pip[0] == 10 && pip[1] == 0 && pip[2] == 2 && pip[3] == 15);
    CHECK(ip_parse("255.255.255.255", pip) && pip[3] == 255);
    CHECK(!ip_parse("256.1.1.1", pip) && !ip_parse("1.2.3", pip) && !ip_parse("1.2.3.4.5", pip));
    CHECK(!ip_parse("1.2.3.", pip) && !ip_parse("a.b.c.d", pip) && !ip_parse("", pip) && !ip_parse("1.2.3.4 ", pip));
    CHECK(!ip_parse("1..2.3", pip) && !ip_parse("example.com", pip) && !ip_parse("0001.2.3.4", pip));

    /* TCP segment: SYN with MSS option, then a data segment; checksum with pseudo header; corruption caught */
    TcpSeg ts;
    n = tcp_build(f, MAC_A, MAC_B, IP_A, IP_B, 50000, 80, 0xFFFFFFF0u, 0, TCP_SYN, 8192, 1460, 11, NULL, 0);
    CHECK(n == ETH_HDR_LEN + IP_HDR_LEN + 24);
    CHECK(net_checksum(f + ETH_HDR_LEN, IP_HDR_LEN) == 0);
    CHECK(tcp_parse(f, n, &ts) && ts.src_port == 50000 && ts.dst_port == 80 && ts.seq == 0xFFFFFFF0u && ts.ack == 0);
    CHECK(ts.flags == TCP_SYN && ts.window == 8192 && ts.mss == 1460 && ts.payload_len == 0);
    n = tcp_build(f, MAC_B, MAC_A, IP_B, IP_A, 80, 50000, 1000, 0xFFFFFFF1u, TCP_ACK | TCP_PSH, 65535, 0, 12, blob, 5);
    CHECK(n == ETH_HDR_LEN + IP_HDR_LEN + 20 + 5);
    CHECK(tcp_parse(f, n, &ts) && ts.flags == (TCP_ACK | TCP_PSH) && ts.ack == 0xFFFFFFF1u && ts.mss == 0);
    CHECK(ts.payload_len == 5 && memcmp(ts.payload, blob, 5) == 0 && ts.window == 65535);
    CHECK(memcmp(ts.src_ip, IP_B, 4) == 0 && memcmp(ts.dst_mac, MAC_A, 6) == 0);
    memcpy(bad, f, n);
    bad[n - 1] ^= 1;
    CHECK(!tcp_parse(bad, n, &ts)); /* payload bit flipped */
    memcpy(bad, f, n);
    bad[ETH_HDR_LEN + IP_HDR_LEN + 5] ^= 1; /* sequence number bit flipped */
    CHECK(!tcp_parse(bad, n, &ts));
    CHECK(!tcp_parse(f, n - 1, &ts));
    CHECK(tcp_build(f, MAC_B, MAC_A, IP_B, IP_A, 80, 50000, 1, 1, TCP_ACK, 1, 0, 1, blob, TCP_MAX_PAYLOAD + 1) == 0);
    /* a window over 65535 is clamped (no window scaling) */
    n = tcp_build(f, MAC_B, MAC_A, IP_B, IP_A, 80, 1, 1, 1, TCP_ACK, 100000, 0, 1, NULL, 0);
    CHECK(tcp_parse(f, n, &ts) && ts.window == 65535);
    /* an option whose length runs past the header is rejected even with a correct checksum */
    n = tcp_build(f, MAC_A, MAC_B, IP_A, IP_B, 1, 2, 1, 0, TCP_SYN, 100, 1460, 1, NULL, 0);
    {
        unsigned char *t = f + ETH_HDR_LEN + IP_HDR_LEN;
        unsigned char tmp[12 + 24];
        t[21] = 9;
        t[16] = t[17] = 0;
        memcpy(tmp, IP_A, 4);
        memcpy(tmp + 4, IP_B, 4);
        tmp[8] = 0; tmp[9] = 6; tmp[10] = 0; tmp[11] = 24; /* pseudo header: zero, protocol 6, TCP length 24 */
        memcpy(tmp + 12, t, 24);
        unsigned short s = net_checksum(tmp, sizeof tmp);
        t[16] = (unsigned char)(s >> 8);
        t[17] = (unsigned char)s;
        CHECK(!tcp_parse(f, n, &ts));
    }
    if (fails == 0)
        printf("netproto tests passed\n");
    return fails ? 1 : 0;
}
