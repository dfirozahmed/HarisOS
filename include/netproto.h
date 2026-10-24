/* Network protocols, the parts that need no hardware and no state: checksums and building / parsing of
 * Ethernet + ARP and Ethernet + IPv4 + ICMP echo frames. Host-tested (tests/test_netproto.c).
 *
 * All multi-byte fields on the wire are big endian ("network order"). The functions read and write single
 * bytes, so they work on any buffer alignment and any CPU byte order. */
#ifndef HARIS_NETPROTO_H
#define HARIS_NETPROTO_H

#define ETH_HDR_LEN 14
#define ARP_FRAME_LEN 42      /* 14 Ethernet + 28 ARP */
#define IP_HDR_LEN 20         /* IPv4 header without options (we never send options) */
#define ICMP_HDR_LEN 8
#define ICMP_MAX_PAYLOAD 1000 /* what net_ping / the echo builder accept; keeps frames well under 1500 */

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP 0x0806
#define IP_PROTO_ICMP 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2
#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8

/* The Internet checksum (RFC 1071): ones' complement of the ones' complement sum of 16-bit words.
 * Over data that already contains a correct checksum the result is 0. */
unsigned short net_checksum(const void *data, unsigned len);

typedef struct
{
    unsigned op; /* ARP_OP_REQUEST or ARP_OP_REPLY */
    unsigned char sender_mac[6], sender_ip[4];
    unsigned char target_mac[6], target_ip[4];
} ArpPacket;

/* Writes a whole Ethernet frame (broadcast for a request, unicast to target_mac for a reply).
 * Returns the length (ARP_FRAME_LEN). */
unsigned arp_build(unsigned char *frame, unsigned op, const unsigned char sender_mac[6],
                   const unsigned char sender_ip[4], const unsigned char target_mac[6],
                   const unsigned char target_ip[4]);
/* 1 and fills *out if the frame is an ARP packet for Ethernet + IPv4, else 0. */
int arp_parse(const unsigned char *frame, unsigned len, ArpPacket *out);

typedef struct
{
    unsigned type; /* ICMP_ECHO_REQUEST or ICMP_ECHO_REPLY */
    unsigned id, seq;
    unsigned char src_mac[6], dst_mac[6];
    unsigned char src_ip[4], dst_ip[4];
    const unsigned char *payload; /* points into the parsed frame */
    unsigned payload_len;
} IcmpEcho;

/* Writes an Ethernet + IPv4 + ICMP echo frame with correct checksums. Returns the frame length, or 0 if the
 * payload is longer than ICMP_MAX_PAYLOAD. `ip_id` goes into the IPv4 identification field. */
unsigned icmp_echo_build(unsigned char *frame, const unsigned char src_mac[6], const unsigned char dst_mac[6],
                         const unsigned char src_ip[4], const unsigned char dst_ip[4], unsigned type,
                         unsigned id, unsigned seq, unsigned ip_id, const unsigned char *payload,
                         unsigned payload_len);
/* 1 and fills *out if the frame is a well-formed, unfragmented IPv4 ICMP echo request / reply whose IP and
 * ICMP checksums are right. Everything else gives 0. */
int icmp_echo_parse(const unsigned char *frame, unsigned len, IcmpEcho *out);

#endif
