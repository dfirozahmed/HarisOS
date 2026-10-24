/* Network: the first NIC on PCI and raw Ethernet frames. Polled, one adapter, no protocols yet.
 *
 *   net_init();                       once, after pci_scan(); finds an Intel e1000 and brings it up
 *   net_present();                    1 if an adapter works
 *   net_mac();                        its 6-byte MAC address
 *   net_send(frame, len);             send a whole Ethernet frame (without the CRC); 0 on success
 *   net_recv(buf, max);               length of the next received frame (without CRC), or 0 if none waits
 *   net_ip();                         our IPv4 address (fixed 10.0.2.15 for now, the QEMU user network; DHCP later)
 *   net_poll();                       handles the frames that arrived: learns ARP answers, answers ARP and ping
 *                                     requests for us; returns how many it read. Call it while waiting.
 *   net_ping(ip, seq, timeout_ms);    echo request to ip (ARP first when needed); round trip in ms, or -1
 *   net_self_test();                  boot check: three pings to the QEMU gateway; returns how many were answered */
#ifndef HARIS_NET_H
#define HARIS_NET_H

#define NET_MAX_FRAME 1518 /* 14 byte header + 1500 payload + 4 byte CRC */

int net_init(void);
int net_present(void);
const unsigned char *net_mac(void);
int net_link_up(void);
int net_send(const void *frame, unsigned len);
int net_recv(void *buf, unsigned max);
const unsigned char *net_ip(void);
int net_poll(void);
int net_ping(const unsigned char ip[4], unsigned seq, unsigned timeout_ms);
int net_self_test(void);

#endif
