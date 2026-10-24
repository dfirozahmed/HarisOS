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


#define IP_PROTO_UDP 17
#define UDP_HDR_LEN 8
#define UDP_MAX_PAYLOAD 1400

typedef struct
{
    unsigned src_port, dst_port;
    unsigned char src_mac[6], dst_mac[6];
    unsigned char src_ip[4], dst_ip[4];
    const unsigned char *payload; /* points into the parsed frame */
    unsigned payload_len;
} UdpPacket;

/* Writes an Ethernet + IPv4 + UDP frame; the UDP checksum covers the pseudo header (addresses + protocol + length)
 * as RFC 768 asks. Returns the frame length, or 0 if the payload is longer than UDP_MAX_PAYLOAD. */
unsigned udp_build(unsigned char *frame, const unsigned char src_mac[6], const unsigned char dst_mac[6],
                   const unsigned char src_ip[4], const unsigned char dst_ip[4], unsigned src_port,
                   unsigned dst_port, unsigned ip_id, const unsigned char *payload, unsigned payload_len);
/* 1 and fills *out for a well-formed, unfragmented IPv4 UDP datagram with right IP checksum and (when the sender
 * used one) right UDP checksum. A UDP checksum field of 0 means "none" and is accepted. */
int udp_parse(const unsigned char *frame, unsigned len, UdpPacket *out);

/* DHCP (RFC 2131): how a host without an address asks for one. Ports: client 68, server 67. */
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_NAK 6
#define DHCP_MIN_LEN 300 /* old servers drop shorter messages (BOOTP minimum) */

typedef struct
{
    unsigned type; /* DHCP_OFFER, DHCP_ACK, ... (option 53) */
    unsigned xid;
    unsigned char yiaddr[4];    /* the address offered / granted to us */
    unsigned char server_id[4]; /* option 54 */
    unsigned char mask[4];      /* option 1 */
    unsigned char router[4];    /* option 3, the first one */
    unsigned char dns[4];       /* option 6, the first one */
    unsigned lease_s;     /* option 51 */
    int has_server, has_mask, has_router, has_dns;
} DhcpMsg;

/* Writes a DHCP client message (DISCOVER or REQUEST) into `out` (room for DHCP_MIN_LEN bytes). Broadcast flag set, so
 * the server may answer before we own an address. `requested_ip` / `server_id` are added when not NULL (REQUEST).
 * Returns the length (DHCP_MIN_LEN). */
unsigned dhcp_build(unsigned char *out, unsigned type, const unsigned char mac[6], unsigned xid,
                    const unsigned char *requested_ip, const unsigned char *server_id);
/* 1 and fills *out if `data` is a well-formed BOOTREPLY with the DHCP magic cookie and a message type; options that
 * run past the end make it fail. *out is overwritten even on failure. */
int dhcp_parse(const unsigned char *data, unsigned len, DhcpMsg *out);


/* DNS (RFC 1035): ask a server for the IPv4 address (A record) of a name, over UDP port 53. */
#define DNS_PORT 53
#define DNS_MAX_QUERY 272 /* 12 header + up to 255 name bytes + 4 (type, class) */

/* Writes a standard recursive query for the A record of `name` ("example.com", a trailing dot is fine). Returns the
 * length, or 0 if the name is not valid: empty, a label empty or over 63 bytes, whole name over 253 characters, or a
 * character other than letters, digits, '-' and '_'. */
unsigned dns_build_query(unsigned char *out, unsigned id, const char *name);
/* Reads a reply to the query with this id. Gives 1 and the first A record in ip[4]; 0 when the message is not a usable
 * reply to this query (wrong id, not a response, truncated or malformed) so the caller keeps waiting; -1 when the server
 * answered but there is no address (name does not exist, server failure, no A record). Compressed names are skipped, never
 * followed, so a hostile reply cannot loop us. */
int dns_parse_a(const unsigned char *msg, unsigned len, unsigned id, unsigned char ip[4]);

/* "10.0.2.15" -> 4 bytes. 1 on success; 0 for anything else (a name, a value over 255, a missing part). */
int ip_parse(const char *s, unsigned char ip[4]);
#endif
