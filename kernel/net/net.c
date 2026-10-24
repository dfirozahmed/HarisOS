/* Network boot self test. See include/net.h.
 *
 * ARP in short: to send an IP packet on Ethernet we need the MAC address of the next hop, and ARP asks for it:
 * a broadcast frame "who has 10.0.2.2? tell 10.0.2.15" and the owner answers with a unicast frame "10.0.2.2 is at
 * <mac>". QEMU's user-mode network (-nic user) gives the guest 10.0.2.15 and answers for the gateway 10.0.2.2,
 * so getting the answer proves that the card both sends and receives. */
#include "net.h"
#include "serial.h"
#include "timer.h"

#define ETH_ARP 0x0806

static const unsigned char GUEST_IP[4] = {10, 0, 2, 15};
static const unsigned char GATEWAY_IP[4] = {10, 0, 2, 2};

static void put_mac(const unsigned char *m)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++)
    {
        serial_putc(hex[m[i] >> 4]);
        serial_putc(hex[m[i] & 15]);
        if (i < 5)
            serial_putc(':');
    }
}

static int same(const unsigned char *a, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int net_self_test(void)
{
    if (!net_present())
    {
        serial_puts("HARISOS: network: no supported adapter\n");
        return 0;
    }
    const unsigned char *mac = net_mac();
    serial_puts("HARISOS: network: e1000 mac ");
    put_mac(mac);
    serial_puts(net_link_up() ? ", link up\n" : ", link down\n");

    unsigned char f[42];
    for (int i = 0; i < 6; i++)
    {
        f[i] = 0xFF; /* broadcast */
        f[6 + i] = mac[i];
    }
    f[12] = ETH_ARP >> 8;
    f[13] = ETH_ARP & 0xFF;
    f[14] = 0; f[15] = 1;      /* hardware type: Ethernet */
    f[16] = 0x08; f[17] = 0x00; /* protocol type: IPv4 */
    f[18] = 6; f[19] = 4;      /* address sizes */
    f[20] = 0; f[21] = 1;      /* operation: request */
    for (int i = 0; i < 6; i++)
    {
        f[22 + i] = mac[i];    /* sender MAC */
        f[32 + i] = 0;         /* target MAC: unknown */
    }
    for (int i = 0; i < 4; i++)
    {
        f[28 + i] = GUEST_IP[i];
        f[38 + i] = GATEWAY_IP[i];
    }
    if (net_send(f, sizeof f) != 0)
    {
        serial_puts("HARISOS: network: ARP send failed\n");
        return 0;
    }

    unsigned char r[NET_MAX_FRAME];
    /* generous: QEMU's TCG main loop can answer a few hundred ms late while the guest only polls memory */
    unsigned long long start = timer_ms(), end = start + 5000;
    while (timer_ms() < end)
    {
        int n = net_recv(r, sizeof r);
        /* an ARP reply (operation 2) from the gateway */
        if (n >= 42 && r[12] == (ETH_ARP >> 8) && r[13] == (ETH_ARP & 0xFF) && r[21] == 2 &&
            same(r + 28, GATEWAY_IP, 4))
        {
            serial_puts("HARISOS: network: ARP reply, 10.0.2.2 is at ");
            put_mac(r + 22);
            serial_puts(" (");
            serial_putd((long long)(timer_ms() - start));
            serial_puts(" ms)\n");
            return 1;
        }
    }
    serial_puts("HARISOS: network: no ARP reply\n");
    return 0;
}
