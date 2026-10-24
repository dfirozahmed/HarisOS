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
/* The result of the last Wi-Fi scan as text: line 0 is a summary, lines 1 .. n one network each (channel, security, BSSID, name), then a line
 * with the frames heard per channel. Returns the length of the line, or -1 (no Wi-Fi scan has been done, or no such line). */
int net_wifi_scan_line(unsigned index, char *buf, unsigned cap);

/* ---- choosing a Wi-Fi network at run time (the Wi-Fi app) ----
 * Work only while the Wi-Fi card is the network adapter (net_wifi_status().present). The card stays an adapter without a connection when none was
 * made at boot (no WIFI.CFG, or the network was not there): then the window of the desktop can pick one. A passphrase given here lives in the
 * driver's memory only; nothing is written to a disk. */
typedef struct
{
    char ssid[33];
    unsigned char bssid[6];
    unsigned char channel; /* the primary channel */
    unsigned char sec;     /* 0 open, 1 WEP, 2 WPA, 3 WPA2 (only WPA2-Personal with CCMP can be joined) */
    unsigned char ht, vht; /* what the access point offers: HT (802.11n), VHT (802.11ac) */
    unsigned beacons;      /* beacons heard during the scan: more = nearer / stronger, roughly */
} NetWifiAp;

typedef struct
{
    int present;           /* a Wi-Fi card that can scan and connect */
    int state;             /* 0 not connected, 1 connected, 2 connection lost, connecting again */
    char ssid[33];         /* the network of state 1 and 2 */
    unsigned char channel; /* ... its primary channel */
    unsigned width;        /* ... the channel width in MHz */
} NetWifiStatus;

#define NET_WIFI_OK 0
#define NET_WIFI_E_NO_ADAPTER -1   /* no Wi-Fi card in use */
#define NET_WIFI_E_ARGS -2         /* name or passphrase of the wrong length (name 1-32, passphrase 8-63 characters) */
#define NET_WIFI_E_NOT_FOUND -3    /* the network is not in the scan list */
#define NET_WIFI_E_UNSUPPORTED -4  /* open, WEP, WPA1 or another security than WPA2-Personal with CCMP */
#define NET_WIFI_E_NO_ANSWER -5    /* the access point did not answer (authentication / association) */
#define NET_WIFI_E_HANDSHAKE -6    /* it answered, but the key exchange failed: most likely a wrong passphrase */
#define NET_WIFI_E_BUSY -7         /* a scan is going on */

void net_wifi_status(NetWifiStatus *st);
int net_wifi_ap_count(void);                       /* networks in the last scan, -1 = no scan has been done */
int net_wifi_ap(unsigned index, NetWifiAp *out);   /* 0 = ok, -1 = no such entry */
/* Scan all channels again (about 4 seconds; the connection, if there is one, pauses meanwhile). Returns the number of networks, or a NET_WIFI_E_ code. */
int net_wifi_rescan(void);
/* The same in steps that do not hold anything up: _scan_start() returns at once, the scan goes on from every poll of the network stack (net_poll); _scan_progress() says
 * how far it is in percent, -1 when none is going on (then net_wifi_ap_count() / net_wifi_ap() show the new list). */
int net_wifi_scan_start(void);
int net_wifi_scan_progress(void);
/* Join a network from the scan list (several seconds, then DHCP). The network the card was connected to before stays the one to reconnect to when
 * this fails. Returns NET_WIFI_OK or a NET_WIFI_E_ code. */
int net_wifi_connect(const char *ssid, const char *passphrase);
int net_irq_init(void);
int net_irq_active(void);
int net_irq_take(void);
unsigned net_irq_count(void);

#endif
