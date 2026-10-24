/* Network: the first NIC on PCI and raw Ethernet frames. Polled, one adapter, no protocols yet.
 *
 *   net_init();                       once, after pci_scan(); finds an Intel e1000 and brings it up
 *   net_present();                    1 if an adapter works
 *   net_mac();                        its 6-byte MAC address
 *   net_send(frame, len);             send a whole Ethernet frame (without the CRC); 0 on success
 *   net_recv(buf, max);               length of the next received frame (without CRC), or 0 if none waits
 *   net_self_test();                  boot check: ARP "who has 10.0.2.2" and wait for the answer (QEMU user net) */
#ifndef HARIS_NET_H
#define HARIS_NET_H

#define NET_MAX_FRAME 1518 /* 14 byte header + 1500 payload + 4 byte CRC */

int net_init(void);
int net_present(void);
const unsigned char *net_mac(void);
int net_link_up(void);
int net_send(const void *frame, unsigned len);
int net_recv(void *buf, unsigned max);
int net_self_test(void);

#endif
