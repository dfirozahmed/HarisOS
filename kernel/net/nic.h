/* What a network card driver offers to the network code (nic.c picks one). Not part of the public API (include/net.h). */
#ifndef HARIS_NIC_H
#define HARIS_NIC_H

#include "net.h"
#include "pci.h"

typedef struct
{
    const char *name; /* "Intel e1000", "Realtek RTL8168" */
    /* Is this card one the driver is meant for (PCI ids only; no side effects)? */
    int (*claims)(const PciDevice *pd);
    /* Reset and start the card. 1 = it works now. 0 = it does not: status() then says why. */
    int (*probe)(const PciDevice *pd);
    const unsigned char *(*mac)(void);
    int (*link_up)(void);
    int (*send)(const void *frame, unsigned len); /* a whole Ethernet frame without CRC; 0 or -1 */
    int (*recv)(void *buf, unsigned max);          /* the next frame without CRC: its length, 0 = none (or a bad one dropped) */
    /* One line of text for the Device Manager (at most cap-1 characters): chip, link, counters, or why the card does not work. */
    void (*status)(char *buf, unsigned cap);
    /* Receive interrupts, all optional (NULL = polling only). irq_init returns the I/O APIC line or -1. */
    int (*irq_init)(void);
    int (*irq_active)(void);
    int (*irq_take)(void);
    unsigned (*irq_count)(void);
} NicDriver;

extern const NicDriver NIC_E1000;
extern const NicDriver NIC_RTL8168;
extern const NicDriver NIC_RTL8821CE; /* Wi-Fi: identification only for now, probe always fails (rtl8821ce.c) */

/* The last Wi-Fi scan as text lines (rtl8821ce.c); see net_wifi_scan_line() in net.h. */
int rtl8821ce_scan_line(unsigned index, char *buf, unsigned cap);

/* Choosing a network at run time (rtl8821ce.c); see net.h: net_wifi_*. */
void rtl8821ce_wifi_status(NetWifiStatus *st);
int rtl8821ce_ap_count(void);
int rtl8821ce_ap(unsigned index, NetWifiAp *out);
int rtl8821ce_wifi_rescan(void);
int rtl8821ce_wifi_scan_start(void);
int rtl8821ce_wifi_scan_progress(void);
int rtl8821ce_wifi_connect(const char *ssid, const char *psk);

/* Tiny text builder for status lines (no libc here): appends to buf[cap], always NUL-terminated. */
typedef struct
{
    char *buf;
    unsigned cap, n;
} NicText;

static inline void nic_text_init(NicText *t, char *buf, unsigned cap)
{
    t->buf = buf;
    t->cap = cap;
    t->n = 0;
    if (cap)
        buf[0] = 0;
}

static inline void nic_putc(NicText *t, char c)
{
    if (t->n + 1 < t->cap)
    {
        t->buf[t->n++] = c;
        t->buf[t->n] = 0;
    }
}

static inline void nic_puts(NicText *t, const char *s)
{
    while (*s)
        nic_putc(t, *s++);
}

static inline void nic_putd(NicText *t, unsigned v)
{
    char tmp[10];
    int n = 0;
    do
    {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        nic_putc(t, tmp[--n]);
}

static inline void nic_puth(NicText *t, unsigned v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        nic_putc(t, "0123456789abcdef"[(v >> (4 * i)) & 15]);
}

#endif
