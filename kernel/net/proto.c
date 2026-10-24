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

/* ---- UDP ---- */

/* Adds `len` bytes as 16-bit words to a running ones' complement sum (an odd last byte is padded with zero). */
static unsigned long sum_add(unsigned long sum, const unsigned char *p, unsigned len)
{
    for (; len >= 2; len -= 2, p += 2)
        sum += get16(p);
    if (len)
        sum += (unsigned long)p[0] << 8;
    return sum;
}

static unsigned short sum_finish(unsigned long sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

/* UDP checksum: the sum over the pseudo header (source ip, destination ip, zero, protocol, UDP length) and the UDP
 * header + data. With the checksum field in place, a correct datagram sums to 0. */
static unsigned short udp_checksum(const unsigned char sip[4], const unsigned char dip[4], const unsigned char *udp,
                                   unsigned udp_len)
{
    unsigned long sum = sum_add(0, sip, 4);
    sum = sum_add(sum, dip, 4);
    sum += IP_PROTO_UDP;
    sum += udp_len;
    return sum_finish(sum_add(sum, udp, udp_len));
}

unsigned udp_build(unsigned char *f, const unsigned char smac[6], const unsigned char dmac[6],
                   const unsigned char sip[4], const unsigned char dip[4], unsigned sport, unsigned dport,
                   unsigned ip_id, const unsigned char *payload, unsigned plen)
{
    if (plen > UDP_MAX_PAYLOAD)
        return 0;
    unsigned udp_len = UDP_HDR_LEN + plen;
    unsigned ip_len = IP_HDR_LEN + udp_len;

    copy(f, dmac, 6);
    copy(f + 6, smac, 6);
    put16(f + 12, ETH_TYPE_IPV4);

    unsigned char *ip = f + ETH_HDR_LEN;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, ip_len);
    put16(ip + 4, ip_id);
    put16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = IP_PROTO_UDP;
    put16(ip + 10, 0);
    copy(ip + 12, sip, 4);
    copy(ip + 16, dip, 4);
    put16(ip + 10, net_checksum(ip, IP_HDR_LEN));

    unsigned char *u = ip + IP_HDR_LEN;
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, udp_len);
    put16(u + 6, 0);
    for (unsigned i = 0; i < plen; i++)
        u[UDP_HDR_LEN + i] = payload[i];
    unsigned short c = udp_checksum(sip, dip, u, udp_len);
    put16(u + 6, c ? c : 0xFFFF); /* a computed 0 is sent as all ones, because 0 means "no checksum" */
    return ETH_HDR_LEN + ip_len;
}

int udp_parse(const unsigned char *f, unsigned len, UdpPacket *out)
{
    if (len < ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN || get16(f + 12) != ETH_TYPE_IPV4)
        return 0;
    const unsigned char *ip = f + ETH_HDR_LEN;
    if ((ip[0] >> 4) != 4)
        return 0;
    unsigned ihl = (unsigned)(ip[0] & 15) * 4;
    unsigned ip_len = get16(ip + 2);
    if (ihl < IP_HDR_LEN || ip_len < ihl + UDP_HDR_LEN || ETH_HDR_LEN + ip_len > len)
        return 0;
    if (ip[9] != IP_PROTO_UDP || (get16(ip + 6) & 0x3FFF) || net_checksum(ip, ihl) != 0)
        return 0;
    const unsigned char *u = ip + ihl;
    unsigned udp_len = get16(u + 4);
    if (udp_len < UDP_HDR_LEN || udp_len > ip_len - ihl)
        return 0;
    if (get16(u + 6) != 0 && udp_checksum(ip + 12, ip + 16, u, udp_len) != 0)
        return 0;
    out->src_port = get16(u);
    out->dst_port = get16(u + 2);
    copy(out->dst_mac, f, 6);
    copy(out->src_mac, f + 6, 6);
    copy(out->src_ip, ip + 12, 4);
    copy(out->dst_ip, ip + 16, 4);
    out->payload = u + UDP_HDR_LEN;
    out->payload_len = udp_len - UDP_HDR_LEN;
    return 1;
}

/* ---- DHCP ---- */

/* BOOTP layout: op htype hlen hops | xid | secs flags | ciaddr yiaddr siaddr giaddr | chaddr[16] | sname[64] |
 * file[128] | magic cookie 63 82 53 63 | options (code, length, data ... 255). */
#define BOOTP_YIADDR 16
#define BOOTP_CHADDR 28
#define BOOTP_COOKIE 236
#define BOOTP_OPTIONS 240

unsigned dhcp_build(unsigned char *o, unsigned type, const unsigned char mac[6], unsigned xid,
                    const unsigned char *requested_ip, const unsigned char *server_id)
{
    for (unsigned i = 0; i < DHCP_MIN_LEN; i++)
        o[i] = 0;
    o[0] = 1; /* BOOTREQUEST */
    o[1] = 1; /* Ethernet */
    o[2] = 6;
    o[4] = (unsigned char)(xid >> 24);
    o[5] = (unsigned char)(xid >> 16);
    o[6] = (unsigned char)(xid >> 8);
    o[7] = (unsigned char)xid;
    put16(o + 10, 0x8000); /* flags: please answer by broadcast */
    copy(o + BOOTP_CHADDR, mac, 6);
    o[BOOTP_COOKIE] = 0x63;
    o[BOOTP_COOKIE + 1] = 0x82;
    o[BOOTP_COOKIE + 2] = 0x53;
    o[BOOTP_COOKIE + 3] = 0x63;

    unsigned char *p = o + BOOTP_OPTIONS;
    *p++ = 53; *p++ = 1; *p++ = (unsigned char)type;
    if (requested_ip)
    {
        *p++ = 50; *p++ = 4;
        copy(p, requested_ip, 4);
        p += 4;
    }
    if (server_id)
    {
        *p++ = 54; *p++ = 4;
        copy(p, server_id, 4);
        p += 4;
    }
    *p++ = 55; *p++ = 4;       /* parameter request list: what we want to know */
    *p++ = 1; *p++ = 3; *p++ = 6; *p++ = 51; /* subnet mask, router, DNS server, lease time */
    *p++ = 255;                /* end; the rest of the message is already zero padding */
    return DHCP_MIN_LEN;
}

int dhcp_parse(const unsigned char *d, unsigned len, DhcpMsg *out)
{
    if (len < BOOTP_OPTIONS || d[0] != 2 || d[1] != 1 || d[2] != 6) /* BOOTREPLY over Ethernet */
        return 0;
    if (d[BOOTP_COOKIE] != 0x63 || d[BOOTP_COOKIE + 1] != 0x82 || d[BOOTP_COOKIE + 2] != 0x53 ||
        d[BOOTP_COOKIE + 3] != 0x63)
        return 0;
    DhcpMsg *m = out; /* filled in place: a struct copy could become a memcpy call, and the kernel has none */
    volatile unsigned char *z = (volatile unsigned char *)m; /* no memset in the kernel */
    for (unsigned i = 0; i < sizeof *m; i++)
        z[i] = 0;
    m->xid = (unsigned)d[4] << 24 | (unsigned)d[5] << 16 | (unsigned)d[6] << 8 | d[7];
    copy(m->yiaddr, d + BOOTP_YIADDR, 4);

    unsigned i = BOOTP_OPTIONS;
    while (i < len)
    {
        unsigned code = d[i++];
        if (code == 255)
            break;
        if (code == 0) /* padding */
            continue;
        if (i >= len)
            return 0;
        unsigned n = d[i++];
        if (n > len - i) /* the option runs past the end */
            return 0;
        const unsigned char *v = d + i;
        if (code == 53 && n == 1)
            m->type = v[0];
        else if (code == 54 && n == 4)
        {
            copy(m->server_id, v, 4);
            m->has_server = 1;
        }
        else if (code == 1 && n == 4)
        {
            copy(m->mask, v, 4);
            m->has_mask = 1;
        }
        else if (code == 3 && n >= 4)
        {
            copy(m->router, v, 4);
            m->has_router = 1;
        }
        else if (code == 6 && n >= 4)
        {
            copy(m->dns, v, 4);
            m->has_dns = 1;
        }
        else if (code == 51 && n == 4)
            m->lease_s = (unsigned)v[0] << 24 | (unsigned)v[1] << 16 | (unsigned)v[2] << 8 | v[3];
        i += n;
    }
    if (m->type == 0)
        return 0;
    return 1;
}

/* ---- DNS ---- */

unsigned dns_build_query(unsigned char *o, unsigned id, const char *name)
{
    put16(o, id);
    put16(o + 2, 0x0100); /* standard query, recursion desired */
    put16(o + 4, 1);      /* one question */
    put16(o + 6, 0);
    put16(o + 8, 0);
    put16(o + 10, 0);

    if (*name == 0)
        return 0;
    unsigned pos = 12;
    const char *p = name;
    while (*p)
    {
        unsigned n = 0; /* length of this label */
        while (p[n] && p[n] != '.')
        {
            char c = p[n];
            int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok)
                return 0;
            n++;
        }
        if (n == 0 || n > 63 || pos - 12 + n + 1 + 1 > 255) /* empty label, too long label, name over 255 encoded bytes */
            return 0;
        o[pos++] = (unsigned char)n; /* a label is its length, then its characters */
        for (unsigned i = 0; i < n; i++)
            o[pos++] = (unsigned char)p[i];
        p += n;
        if (*p == '.')
        {
            p++;
            if (*p == 0) /* one trailing dot ("example.com.") is the same name */
                break;
        }
    }
    o[pos++] = 0;           /* the root label ends the name */
    put16(o + pos, 1);      /* type A */
    put16(o + pos + 2, 1);  /* class IN */
    return pos + 4;
}

/* Skips a name at *pos: labels, or a compression pointer (2 bytes, end of name). 1 if it stayed inside the message. */
static int dns_skip_name(const unsigned char *m, unsigned len, unsigned *pos)
{
    unsigned i = *pos;
    for (;;)
    {
        if (i >= len)
            return 0;
        unsigned c = m[i];
        if (c == 0)
        {
            *pos = i + 1;
            return 1;
        }
        if ((c & 0xC0) == 0xC0)
        {
            if (i + 2 > len)
                return 0;
            *pos = i + 2;
            return 1;
        }
        if (c & 0xC0) /* 0x40 / 0x80 label types are not defined */
            return 0;
        i += c + 1;
    }
}

int dns_parse_a(const unsigned char *m, unsigned len, unsigned id, unsigned char ip[4])
{
    if (len < 12 || get16(m) != id)
        return 0;
    unsigned flags = get16(m + 2);
    if (!(flags & 0x8000) || ((flags >> 11) & 15) != 0) /* must be a response to a standard query */
        return 0;
    unsigned qd = get16(m + 4), an = get16(m + 6), pos = 12;
    if (qd != 1)
        return 0;
    if ((flags & 15) != 0) /* RCODE: 3 = name does not exist, 2 = server failure ... */
        return -1;
    for (unsigned i = 0; i < qd; i++)
    {
        if (!dns_skip_name(m, len, &pos) || pos + 4 > len)
            return 0;
        pos += 4;
    }
    for (unsigned i = 0; i < an; i++)
    {
        if (!dns_skip_name(m, len, &pos) || pos + 10 > len)
            return 0;
        unsigned type = get16(m + pos), cls = get16(m + pos + 2), rdlen = get16(m + pos + 8);
        pos += 10;
        if (rdlen > len - pos)
            return 0;
        if (type == 1 && cls == 1 && rdlen == 4)
        {
            copy(ip, m + pos, 4);
            return 1;
        }
        pos += rdlen; /* CNAME and others: the A record of the target usually follows in the same answer */
    }
    return -1;
}

int ip_parse(const char *s, unsigned char ip[4])
{
    for (int part = 0; part < 4; part++)
    {
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9')
        {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || v > 255)
                return 0;
        }
        if (digits == 0)
            return 0;
        ip[part] = (unsigned char)v;
        if (part < 3 && *s++ != '.')
            return 0;
    }
    return *s == 0;
}
