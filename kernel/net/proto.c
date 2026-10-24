/* Stateless network protocol code: checksum, ARP and ICMP echo frames. See include/netproto.h. */
#include "netproto.h"

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static unsigned get16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }

static void copy(unsigned char *d, const unsigned char *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        d[i] = s[i];
}

unsigned short net_checksum(const void *data, unsigned len)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned long sum = 0; /* 64 bits: a 64 KiB packet cannot overflow it */
    for (; len >= 2; len -= 2, p += 2)
        sum += get16(p);
    if (len)
        sum += (unsigned long)p[0] << 8; /* an odd last byte is padded with a zero byte */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

unsigned arp_build(unsigned char *f, unsigned op, const unsigned char smac[6], const unsigned char sip[4],
                   const unsigned char tmac[6], const unsigned char tip[4])
{
    for (int i = 0; i < 6; i++)
        f[i] = op == ARP_OP_REQUEST ? 0xFF : tmac[i]; /* a request is a broadcast; a reply goes to the asker */
    copy(f + 6, smac, 6);
    put16(f + 12, ETH_TYPE_ARP);
    put16(f + 14, 1);             /* hardware type: Ethernet */
    put16(f + 16, ETH_TYPE_IPV4); /* protocol type */
    f[18] = 6;                    /* hardware address size */
    f[19] = 4;                    /* protocol address size */
    put16(f + 20, op);
    copy(f + 22, smac, 6);
    copy(f + 28, sip, 4);
    if (op == ARP_OP_REQUEST)
        for (int i = 0; i < 6; i++)
            f[32 + i] = 0; /* the target MAC is what we ask for */
    else
        copy(f + 32, tmac, 6);
    copy(f + 38, tip, 4);
    return ARP_FRAME_LEN;
}

int arp_parse(const unsigned char *f, unsigned len, ArpPacket *out)
{
    if (len < ARP_FRAME_LEN || get16(f + 12) != ETH_TYPE_ARP)
        return 0;
    if (get16(f + 14) != 1 || get16(f + 16) != ETH_TYPE_IPV4 || f[18] != 6 || f[19] != 4)
        return 0;
    unsigned op = get16(f + 20);
    if (op != ARP_OP_REQUEST && op != ARP_OP_REPLY)
        return 0;
    out->op = op;
    copy(out->sender_mac, f + 22, 6);
    copy(out->sender_ip, f + 28, 4);
    copy(out->target_mac, f + 32, 6);
    copy(out->target_ip, f + 38, 4);
    return 1;
}

unsigned icmp_echo_build(unsigned char *f, const unsigned char smac[6], const unsigned char dmac[6],
                         const unsigned char sip[4], const unsigned char dip[4], unsigned type, unsigned id,
                         unsigned seq, unsigned ip_id, const unsigned char *payload, unsigned plen)
{
    if (plen > ICMP_MAX_PAYLOAD)
        return 0;
    unsigned icmp_len = ICMP_HDR_LEN + plen;
    unsigned ip_len = IP_HDR_LEN + icmp_len;

    copy(f, dmac, 6);
    copy(f + 6, smac, 6);
    put16(f + 12, ETH_TYPE_IPV4);

    unsigned char *ip = f + ETH_HDR_LEN;
    ip[0] = 0x45; /* version 4, header of 5 dwords */
    ip[1] = 0;    /* type of service */
    put16(ip + 2, ip_len);
    put16(ip + 4, ip_id);
    put16(ip + 6, 0); /* no flags, no fragment offset */
    ip[8] = 64;       /* time to live */
    ip[9] = IP_PROTO_ICMP;
    put16(ip + 10, 0); /* checksum field is zero while summing */
    copy(ip + 12, sip, 4);
    copy(ip + 16, dip, 4);
    put16(ip + 10, net_checksum(ip, IP_HDR_LEN));

    unsigned char *ic = ip + IP_HDR_LEN;
    ic[0] = (unsigned char)type;
    ic[1] = 0; /* code */
    put16(ic + 2, 0);
    put16(ic + 4, id);
    put16(ic + 6, seq);
    for (unsigned i = 0; i < plen; i++)
        ic[ICMP_HDR_LEN + i] = payload[i];
    put16(ic + 2, net_checksum(ic, icmp_len));
    return ETH_HDR_LEN + ip_len;
}

int icmp_echo_parse(const unsigned char *f, unsigned len, IcmpEcho *out)
{
    if (len < ETH_HDR_LEN + IP_HDR_LEN + ICMP_HDR_LEN || get16(f + 12) != ETH_TYPE_IPV4)
        return 0;
    const unsigned char *ip = f + ETH_HDR_LEN;
    if ((ip[0] >> 4) != 4)
        return 0;
    unsigned ihl = (unsigned)(ip[0] & 15) * 4; /* header length; options are skipped but must fit */
    unsigned ip_len = get16(ip + 2);
    if (ihl < IP_HDR_LEN || ip_len < ihl + ICMP_HDR_LEN || ETH_HDR_LEN + ip_len > len) /* the frame may be padded */
        return 0;
    if (ip[9] != IP_PROTO_ICMP || (get16(ip + 6) & 0x3FFF)) /* fragments (MF flag or offset) are not handled */
        return 0;
    if (net_checksum(ip, ihl) != 0)
        return 0;
    const unsigned char *ic = ip + ihl;
    unsigned icmp_len = ip_len - ihl;
    if ((ic[0] != ICMP_ECHO_REQUEST && ic[0] != ICMP_ECHO_REPLY) || ic[1] != 0 || net_checksum(ic, icmp_len) != 0)
        return 0;
    out->type = ic[0];
    out->id = get16(ic + 4);
    out->seq = get16(ic + 6);
    copy(out->dst_mac, f, 6);
    copy(out->src_mac, f + 6, 6);
    copy(out->src_ip, ip + 12, 4);
    copy(out->dst_ip, ip + 16, 4);
    out->payload = ic + ICMP_HDR_LEN;
    out->payload_len = icmp_len - ICMP_HDR_LEN;
    return 1;
}
