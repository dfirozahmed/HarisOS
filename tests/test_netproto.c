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

    if (fails == 0)
        printf("netproto tests passed\n");
    return fails ? 1 : 0;
}
