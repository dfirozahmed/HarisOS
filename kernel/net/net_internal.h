/* What net.c and tcp.c share. Not part of the public network API (include/net.h). */
#ifndef HARIS_NET_INTERNAL_H
#define HARIS_NET_INTERNAL_H

#include "netproto.h"

/* MAC address for a packet to `ip`: the host itself when it is on our subnet, otherwise the gateway (ARP, up to
 * 3 tries of timeout_ms each). 1 and mac[6], or 0. */
int net_arp_mac(const unsigned char ip[4], unsigned char mac[6], unsigned timeout_ms);
/* IPv4 identification for the next packet we send. */
unsigned net_next_ip_id(void);

/* tcp.c: a TCP segment addressed to us arrived / time passed (retransmit timers). Both are called from net_poll(). */
void tcp_input(const TcpSeg *seg);
void tcp_tick(void);

#endif
