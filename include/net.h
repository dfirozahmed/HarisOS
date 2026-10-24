/* Network: the first supported NIC on PCI (Intel e1000, Realtek RTL8168) and the protocols above it. One adapter.
 *
 *   net_init();                       once, after pci_scan(); finds a supported card and starts it
 *   net_present();                    1 if an adapter works
 *   net_mac();                        its 6-byte MAC address
 *   net_send(frame, len);             send a whole Ethernet frame (without the CRC); 0 on success
 *   net_recv(buf, max);               length of the next received frame (without CRC), or 0 if none waits
 *   net_ip(); net_gateway(); net_dns();  our IPv4 address and the servers; 0.0.0.0 until net_dhcp() or the self
 *                                     test's fallback sets them
 *   net_resolve(name, ip, timeout_ms);  name -> IPv4 through the DNS server (2 tries, timeout per try); a dotted
 *                                     number is returned as it is; 1 and ip[4] on success
 *   net_dhcp(timeout_ms);            asks a DHCP server for an address (3 tries, timeout per answer); 1 on success
 *   net_poll();                       handles the frames that arrived: learns ARP answers, answers ARP and ping
 *                                     requests for us; returns how many it read. Call it while waiting.
 *   net_ping(ip, seq, timeout_ms);    echo request to ip (ARP first when needed); round trip in ms, or -1
 *   net_adapter(loc, &working, text, cap);  for the Device Manager: 1 if a card was found that a driver is meant for. loc = its PCI bus,
 *                                     device, function; working = 0 if it did not start; text = one line (chip, link, counters, or why not)
 *   net_irq_init();                   after net_init(): find the I/O APIC line of the card and let it interrupt on a received frame;
 *                                     its number, or -1 (then everything stays polled). Optional: without it nothing changes.
 *   net_irq_active();                 1 while interrupts are in use
 *   net_irq_take();                   1 if the card has interrupted since the last call (frames may be waiting), and forget it
 *   net_irq_count();                  how many receive interrupts there were (for diagnostics)
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
int net_ip_source(void); /* 0 = no address, 1 = from DHCP, 2 = the fixed QEMU address used when DHCP failed */
const char *net_driver_name(void); /* "Intel e1000", "Realtek RTL8168" ... ("none" without a working card) */
const unsigned char *net_gateway(void);
const unsigned char *net_dns(void);
int net_dhcp(unsigned timeout_ms);
int net_resolve(const char *name, unsigned char ip[4], unsigned timeout_ms);
void net_set_yield(void (*fn)(void)); /* what net_yield() calls while a caller waits for the network; the kernel passes task_yield */
int net_poll(void);
int net_ping(const unsigned char ip[4], unsigned seq, unsigned timeout_ms);
int net_self_test(void);
int net_adapter(unsigned char loc[3], int *working, char *status, unsigned cap);
int net_irq_init(void);
int net_irq_active(void);
int net_irq_take(void);
unsigned net_irq_count(void);

#endif
