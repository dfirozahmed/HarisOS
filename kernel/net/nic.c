/* Picks the network card driver and forwards the public network calls (include/net.h) to it. See kernel/net/nic.h. */
#include "net.h"
#include "nic.h"
#include "pci.h"

static const NicDriver *const DRIVERS[] = {&NIC_E1000, &NIC_RTL8168, &NIC_RTL8821CE};
#define NDRIVERS ((int)(sizeof DRIVERS / sizeof DRIVERS[0]))

static const NicDriver *ACTIVE;  /* the driver of the card in use */
static const NicDriver *CLAIMED; /* the driver of a card it was meant for but that did not start (kept for the Device Manager) */
static unsigned char LOC[3];     /* bus, device, function of the ACTIVE or else the CLAIMED card */
static int HAVE_LOC;
static const unsigned char NO_MAC[6];

int net_init(void)
{
    ACTIVE = CLAIMED = 0;
    HAVE_LOC = 0;
    for (int i = 0; i < pci_count(); i++)
    {
        const PciDevice *pd = pci_get(i);
        if (pd->class_code != 0x02) /* network controllers only */
            continue;
        for (int k = 0; k < NDRIVERS; k++)
        {
            const NicDriver *d = DRIVERS[k];
            if (!d->claims(pd))
                continue;
            if (d->probe(pd))
            {
                ACTIVE = d;
                LOC[0] = pd->bus;
                LOC[1] = pd->dev;
                LOC[2] = pd->fn;
                HAVE_LOC = 1;
                return 1;
            }
            if (!CLAIMED) /* remember the first card that failed, in case no other one works */
            {
                CLAIMED = d;
                LOC[0] = pd->bus;
                LOC[1] = pd->dev;
                LOC[2] = pd->fn;
                HAVE_LOC = 1;
            }
            break;
        }
    }
    return 0;
}

int net_present(void) { return ACTIVE != 0; }
const unsigned char *net_mac(void) { return ACTIVE ? ACTIVE->mac() : NO_MAC; }
int net_link_up(void) { return ACTIVE && ACTIVE->link_up(); }
int net_send(const void *frame, unsigned len) { return ACTIVE ? ACTIVE->send(frame, len) : -1; }
int net_recv(void *buf, unsigned max) { return ACTIVE ? ACTIVE->recv(buf, max) : 0; }

int net_irq_init(void) { return ACTIVE && ACTIVE->irq_init ? ACTIVE->irq_init() : -1; }
int net_irq_active(void) { return ACTIVE && ACTIVE->irq_active && ACTIVE->irq_active(); }
int net_irq_take(void) { return ACTIVE && ACTIVE->irq_take ? ACTIVE->irq_take() : 0; }
unsigned net_irq_count(void) { return ACTIVE && ACTIVE->irq_count ? ACTIVE->irq_count() : 0; }

int net_adapter(unsigned char loc[3], int *working, char *status, unsigned cap)
{
    const NicDriver *d = ACTIVE ? ACTIVE : CLAIMED;
    if (!d || !HAVE_LOC)
        return 0;
    loc[0] = LOC[0];
    loc[1] = LOC[1];
    loc[2] = LOC[2];
    *working = ACTIVE != 0;
    d->status(status, cap);
    if (ACTIVE) /* append the address, so that the Device Manager shows whether DHCP worked */
    {
        NicText t;
        unsigned n = 0;
        while (n < cap && status[n])
            n++;
        t.buf = status;
        t.cap = cap;
        t.n = n;
        const unsigned char *ip = net_ip();
        if (net_ip_source() == 0)
            nic_puts(&t, ", no IP address");
        else
        {
            nic_puts(&t, ", ");
            for (int i = 0; i < 4; i++)
            {
                nic_putd(&t, ip[i]);
                if (i < 3)
                    nic_putc(&t, '.');
            }
            nic_puts(&t, net_ip_source() == 1 ? " (DHCP)" : " (fixed)");
        }
    }
    return 1;
}

const char *net_driver_name(void) { return ACTIVE ? ACTIVE->name : "no adapter"; }

int net_wifi_scan_line(unsigned index, char *buf, unsigned cap) { return rtl8821ce_scan_line(index, buf, cap); }

/* ---- choosing a Wi-Fi network (include/net.h): only the Wi-Fi card does it ---- */
static int wifi_active(void) { return ACTIVE == &NIC_RTL8821CE; }

void net_wifi_status(NetWifiStatus *st)
{
    if (wifi_active())
        rtl8821ce_wifi_status(st);
    else
    {
        st->present = 0;
        st->state = 0;
        st->ssid[0] = 0;
        st->channel = 0;
        st->width = 20;
    }
}

int net_wifi_ap_count(void) { return wifi_active() ? rtl8821ce_ap_count() : -1; }
int net_wifi_ap(unsigned index, NetWifiAp *out) { return wifi_active() ? rtl8821ce_ap(index, out) : -1; }
int net_wifi_rescan(void) { return wifi_active() ? rtl8821ce_wifi_rescan() : NET_WIFI_E_NO_ADAPTER; }
int net_wifi_scan_start(void) { return wifi_active() ? rtl8821ce_wifi_scan_start() : NET_WIFI_E_NO_ADAPTER; }
int net_wifi_scan_progress(void) { return wifi_active() ? rtl8821ce_wifi_scan_progress() : -1; }

int net_wifi_boot_begin(void) { return wifi_active() ? rtl8821ce_boot_begin() : 0; }
void net_wifi_boot_run(void)
{
    if (wifi_active())
        rtl8821ce_boot_run();
}
void net_wifi_boot_abort(void)
{
    if (wifi_active())
        rtl8821ce_boot_abort();
}

int net_wifi_connect(const char *ssid, const char *passphrase)
{
    if (!wifi_active())
        return NET_WIFI_E_NO_ADAPTER;
    int r = rtl8821ce_wifi_connect(ssid, passphrase);
    if (r == NET_WIFI_OK)
        net_dhcp(6000); /* another network: another address (if it fails, net_ip_source() says so) */
    return r;
}
