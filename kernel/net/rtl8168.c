/* Realtek RTL8111 / RTL8168 PCIe gigabit Ethernet (PCI 10EC:8168). Polled, one adapter. See kernel/net/nic.h.
 *
 * STATUS: written from the register description in Linux' r8169 driver (drivers/net/ethernet/realtek/r8169_main.c: register
 * offsets, bit values, descriptor layout, start order), without a data sheet and WITHOUT having run on the real chip: QEMU has
 * no RTL8168. It does the generic part that every revision shares. Linux does more for each revision (tuning values written
 * through the ERI and EPHY side doors, PHY patches, FIFO sizes); that is NOT done here, so on some revisions the card may
 * stay silent. The Device Manager line (rtl_status) says what happened: the chip revision, the link, counters or the reason
 * for a failure, so that a test on real hardware tells what is missing.
 *
 * How the card talks: its registers are in memory at a PCI BAR. Two rings of 16-byte descriptors in RAM describe the buffers:
 *   word 0  opts1: bit 31 DescOwn (the card owns it), bit 30 RingEnd (last descriptor, the card wraps to the first),
 *                  bit 29 FirstFrag, bit 28 LastFrag, low bits = length (receive: buffer size, then the frame length with CRC)
 *   word 1  opts2: VLAN info, unused here
 *   word 2,3  the 64-bit address of the buffer.
 * Receive: we give the card an empty buffer (DescOwn set); when a frame has arrived it clears DescOwn and writes the length.
 * Send: we fill a buffer, set DescOwn | FirstFrag | LastFrag | length, and poke TxPoll; the card clears DescOwn when sent. */
#include "io.h"
#include "net.h"
#include "nic.h"
#include "pci.h"
#include "pmm.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define R_MAC0 0x00        /* 6 bytes: the address */
#define R_MAR0 0x08        /* 8 bytes: multicast hash filter */
#define R_TNPDS 0x20       /* transmit ring address, low then high dword (normal priority queue) */
#define R_CMD 0x37         /* byte: reset, receive enable, transmit enable */
#define R_TXPOLL 0x38      /* byte: write NPQ to tell the card there is something to send */
#define R_IMR 0x3C         /* 16 bit: interrupt mask */
#define R_ISR 0x3E         /* 16 bit: interrupt status, write 1 to clear */
#define R_TXCFG 0x40       /* also holds the chip revision (read only bits) */
#define R_RXCFG 0x44
#define R_9346 0x50        /* byte: 0xC0 unlocks the configuration registers, 0 locks */
#define R_PHYSTATUS 0x6C   /* byte: link, speed, duplex */
#define R_RMS 0xDA         /* 16 bit: biggest frame the card accepts */
#define R_CPLUS 0xE0       /* 16 bit: "C+" command: VLAN stripping, checksum offload, ... */
#define R_INTMIT 0xE2      /* 16 bit: interrupt mitigation */
#define R_RDSAR 0xE4       /* receive ring address, low then high dword */
#define R_MTPS 0xEC        /* byte, 8168: biggest frame to send in units of 128 bytes */
#define R_MISC 0xF0        /* dword (8168e and later): bit 19 holds the receiver's data valid line shut */

#define CMD_RESET 0x10
#define CMD_RXEN 0x08
#define CMD_TXEN 0x04
#define TXPOLL_NPQ 0x40
#define CFG_UNLOCK 0xC0
#define CFG_LOCK 0x00

#define ACCEPT_BROADCAST 0x08
#define ACCEPT_MYPHYS 0x02
#define RXCFG_ACCEPT_MASK 0x3F

#define RXCFG_FIFO_THRESH (7u << 13) /* no threshold before the first PCI transfer */
#define RXCFG_128_INT (1u << 15)
#define RXCFG_MULTI_EN (1u << 14)
#define RXCFG_EARLY_OFF (1u << 11)
#define RXCFG_DMA_BURST (7u << 8)    /* unlimited burst */
#define TXCFG_DMA_BURST (7u << 8)
#define TXCFG_IFG (3u << 24)         /* the usual inter frame gap */
#define TXCFG_AUTO_FIFO (1u << 7)    /* 8168evl and later */
#define MISC_RXDV_GATED (1u << 19)

#define CPLUS_KEEP 0x2003u           /* Normal_mode and the interrupt timer bits; VLAN stripping and RX checksum offload off */
#define PHY_LINK 0x02
#define PHY_FULL 0x01
#define PHY_1000 0x10
#define PHY_100 0x08
#define PHY_10 0x04

#define DESC_OWN 0x80000000u
#define DESC_END 0x40000000u
#define DESC_FIRST 0x20000000u
#define DESC_LAST 0x10000000u
#define RX_RES 0x00200000u           /* receive status: some error (CRC, runt, ...) */
#define LEN_MASK 0x3FFFu

#define RX_COUNT 16
#define TX_COUNT 8
#define BUF_SIZE 2048
#define MAX_FRAME 1536               /* RMS: biggest frame accepted (1518 + a VLAN tag fits) */
#define MIN_FRAME 60                 /* shorter frames are padded; the card does not */

typedef struct
{
    volatile u32 opts1, opts2;
    volatile u64 addr;
} Desc;

typedef struct
{
    volatile u8 *regs;
    Desc *rx, *tx;
    u8 *rx_buf, *tx_buf;
    unsigned rx_next, tx_next;
    u8 mac[6];
    unsigned xid;                    /* chip revision bits of TxConfig, (TxConfig >> 20) & 0xFCF */
    int ver_index;                   /* into CHIPS, -1 = not in the table */
    int ok;
    unsigned rx_frames, rx_dropped, tx_frames, tx_busy;
    const char *fail;                /* why probe failed (static text) */
} Rtl;

static Rtl R;

/* Revisions by their TxConfig bits: Linux' table, 8168 family. `ver` is Linux' MAC_VER number: it decides the receive
 * configuration and whether the send FIFO is automatic. Entries are tried in order (specific masks first). */
typedef struct
{
    unsigned mask, val;
    int ver;
    const char *name;
} Chip;

static const Chip CHIPS[] = {
    {0x7cf, 0x54b, 52, "RTL8168fp/RTL8117"}, {0x7cf, 0x54a, 52, "RTL8168fp/RTL8117"},
    {0x7cf, 0x502, 51, "RTL8168ep/8111ep"},  {0x7cf, 0x541, 46, "RTL8168h/8111h"},
    {0x7cf, 0x6c0, 46, "RTL8168M"},          {0x7cf, 0x5c8, 44, "RTL8411b"},
    {0x7cf, 0x509, 42, "RTL8168gu/8111gu"},  {0x7cf, 0x4c0, 40, "RTL8168g/8111g"},
    {0x7c8, 0x488, 38, "RTL8411"},           {0x7cf, 0x481, 36, "RTL8168f/8111f"},
    {0x7cf, 0x480, 35, "RTL8168f/8111f"},    {0x7c8, 0x2c8, 34, "RTL8168evl/8111evl"},
    {0x7cf, 0x2c1, 32, "RTL8168e/8111e"},    {0x7c8, 0x2c0, 33, "RTL8168e/8111e"},
    {0x7cf, 0x281, 25, "RTL8168d/8111d"},    {0x7c8, 0x280, 26, "RTL8168d/8111d"},
    {0x7cf, 0x28a, 28, "RTL8168dp/8111dp"},  {0x7cf, 0x28b, 31, "RTL8168dp/8111dp"},
    {0x7cf, 0x3c9, 23, "RTL8168cp/8111cp"},  {0x7cf, 0x3c8, 18, "RTL8168cp/8111cp"},
    {0x7c8, 0x3c8, 24, "RTL8168cp/8111cp"},  {0x7cf, 0x3c0, 19, "RTL8168c/8111c"},
    {0x7cf, 0x3c2, 20, "RTL8168c/8111c"},    {0x7cf, 0x3c3, 21, "RTL8168c/8111c"},
    {0x7c8, 0x3c0, 22, "RTL8168c/8111c"},    {0x7c8, 0x380, 17, "RTL8168b/8111b"},
};
#define NCHIPS ((int)(sizeof CHIPS / sizeof CHIPS[0]))

static inline u8 r8(u32 off) { return *(volatile u8 *)(R.regs + off); }
static inline u16 r16(u32 off) { return *(volatile u16 *)(R.regs + off); }
static inline u32 r32(u32 off) { return *(volatile u32 *)(R.regs + off); }
static inline void w8(u32 off, u8 v) { *(volatile u8 *)(R.regs + off) = v; }
static inline void w16(u32 off, u16 v) { *(volatile u16 *)(R.regs + off) = v; }
static inline void w32(u32 off, u32 v) { *(volatile u32 *)(R.regs + off) = v; }
static inline void fence(void) { __asm__ volatile("mfence" ::: "memory"); }

#ifdef HARIS_HOST
/* the host test (tests/test_rtl8168.c) plays the card: its delay hook is where the "card" finishes a reset */
void rtl_host_udelay(unsigned us);
static void udelay(unsigned us) { rtl_host_udelay(us); }
#else
/* a read of port 0x80 takes about 1 us: a delay that needs no timer */
static void udelay(unsigned us)
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}
#endif

static void zero(void *p, unsigned long n)
{
    volatile u8 *b = (volatile u8 *)p;
    for (unsigned long i = 0; i < n; i++)
        b[i] = 0;
}

static void copy(void *dst, const void *src, unsigned long n)
{
    volatile u8 *d = (volatile u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)src;
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
}

/* Linux' rtl_init_rxcfg: which receive configuration a revision wants. */
static u32 rxcfg_for(int ver)
{
    if (ver == 17)
        return RXCFG_FIFO_THRESH | RXCFG_DMA_BURST;
    if ((ver >= 18 && ver <= 24) || (ver >= 34 && ver <= 36) || ver == 38)
        return RXCFG_128_INT | RXCFG_MULTI_EN | RXCFG_DMA_BURST;
    if (ver >= 40)
        return RXCFG_128_INT | RXCFG_MULTI_EN | RXCFG_DMA_BURST | RXCFG_EARLY_OFF;
    return RXCFG_128_INT | RXCFG_DMA_BURST; /* 8168d, 8168e (before evl) */
}

/* 8168evl and later size the send FIFO themselves. */
static int evl_up(int ver) { return ver >= 34 && ver != 36 && ver != 38; }

static int rtl_claims(const PciDevice *pd) { return pd->vendor_id == 0x10EC && pd->device_id == 0x8168; }

/* The first memory BAR (BAR 2 on this chip; BAR 0 is the same registers in I/O ports). 0 if there is none. */
static u64 find_mmio_bar(const PciDevice *pd)
{
    for (unsigned bar = 0; bar < 6; bar++)
    {
        u32 v = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10 + 4 * bar);
        if (v & 1u)
            continue; /* I/O ports */
        u64 base = v & ~0xFULL;
        if (((v >> 1) & 3u) == 2u && bar < 5) /* 64 bit BAR: the next dword is its upper half */
        {
            base |= (u64)pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10 + 4 * (bar + 1)) << 32;
            bar++;
        }
        if (base)
            return base;
    }
    return 0;
}

static int fail(const char *why)
{
    R.fail = why;
    R.ok = 0;
    return 0;
}

static int rtl_probe(const PciDevice *pd)
{
    zero(&R, sizeof R);
    R.ver_index = -1;
    u32 cmd = pci_cfg_read(pd->bus, pd->dev, pd->fn, 4);
    pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (cmd & 0xFFFFu) | 0x6u); /* memory space + bus master */
    u64 base = find_mmio_bar(pd);
    if (!base)
        return fail("no memory BAR");
    R.regs = (volatile u8 *)base;
    if (r32(R_TXCFG) == 0xFFFFFFFFu) /* all ones: nothing answers there (device off, or the BAR is not mapped) */
        return fail("registers read as all ones");

    /* reset; the card clears the bit itself when it is done */
    w8(R_CMD, CMD_RESET);
    int done = 0;
    for (int i = 0; i < 1000 && !done; i++)
    {
        udelay(100);
        done = !(r8(R_CMD) & CMD_RESET);
    }
    if (!done)
        return fail("reset did not finish");

    R.xid = (r32(R_TXCFG) >> 20) & 0xFCF;
    for (int i = 0; i < NCHIPS; i++)
        if ((R.xid & CHIPS[i].mask) == CHIPS[i].val)
        {
            R.ver_index = i;
            break;
        }
    int ver = R.ver_index >= 0 ? CHIPS[R.ver_index].ver : 0;

    for (int i = 0; i < 6; i++)
        R.mac[i] = r8(R_MAC0 + i);
    int all0 = 1, all1 = 1;
    for (int i = 0; i < 6; i++)
    {
        all0 &= R.mac[i] == 0;
        all1 &= R.mac[i] == 0xFF;
    }
    if (all0 || all1 || (R.mac[0] & 1)) /* a multicast address cannot be a card's own */
        return fail("no valid MAC address");

    /* DMA memory: page 0 = both descriptor rings (256 byte aligned), then the receive buffers, then the send buffers */
    unsigned rx_pages = RX_COUNT * BUF_SIZE / PMM_PAGE_SIZE;
    unsigned tx_pages = TX_COUNT * BUF_SIZE / PMM_PAGE_SIZE;
    unsigned pages = 1 + rx_pages + tx_pages;
    u64 mem = pmm_alloc_contiguous_tag(pages, PMM_TAG_OTHER);
    if (!mem)
        return fail("no memory for the rings");
    zero((void *)mem, (unsigned long)pages * PMM_PAGE_SIZE);
    R.rx = (Desc *)mem;
    R.tx = (Desc *)(mem + 256);
    R.rx_buf = (u8 *)(mem + PMM_PAGE_SIZE);
    R.tx_buf = R.rx_buf + (u64)rx_pages * PMM_PAGE_SIZE;
    for (unsigned i = 0; i < RX_COUNT; i++)
    {
        R.rx[i].addr = (u64)(R.rx_buf + (u64)i * BUF_SIZE);
        R.rx[i].opts1 = DESC_OWN | (i == RX_COUNT - 1 ? DESC_END : 0) | BUF_SIZE;
    }
    for (unsigned i = 0; i < TX_COUNT; i++)
        R.tx[i].opts1 = i == TX_COUNT - 1 ? DESC_END : 0;

    /* configuration registers are read only until unlocked */
    w8(R_9346, CFG_UNLOCK);
    w16(R_CPLUS, (u16)(r16(R_CPLUS) & CPLUS_KEEP)); /* no VLAN stripping, no checksum offload: frames arrive as they are */
    w8(R_MTPS, evl_up(ver) ? 0x27 : 63);            /* biggest frame to send (8168: units of 128 bytes) */
    w16(R_INTMIT, 0);
    w16(R_RMS, MAX_FRAME);
    w32(R_TNPDS + 4, (u32)((u64)R.tx >> 32)); /* the high half first, as Linux does */
    w32(R_TNPDS, (u32)(u64)R.tx);
    w32(R_RDSAR + 4, (u32)((u64)R.rx >> 32));
    w32(R_RDSAR, (u32)(u64)R.rx);
    w8(R_9346, CFG_LOCK);
    if (ver >= 40) /* 8168g and later: the reset leaves the receiver's data line gated */
        w32(R_MISC, r32(R_MISC) & ~MISC_RXDV_GATED);
    (void)r8(R_CMD); /* make the writes reach the card */

    w8(R_CMD, CMD_TXEN | CMD_RXEN); /* the transmit and receive configuration registers want the units enabled first */
    w32(R_RXCFG, rxcfg_for(ver) | ACCEPT_BROADCAST | ACCEPT_MYPHYS);
    w32(R_TXCFG, TXCFG_DMA_BURST | TXCFG_IFG | (evl_up(ver) ? TXCFG_AUTO_FIFO : 0));
    w32(R_MAR0, 0); /* no multicast */
    w32(R_MAR0 + 4, 0);
    w16(R_IMR, 0); /* polled */
    w16(R_ISR, 0xFFFF);

    R.ok = 1;
    return 1;
}

static const unsigned char *rtl_mac(void) { return R.mac; }
static int rtl_link_up(void) { return R.ok && (r8(R_PHYSTATUS) & PHY_LINK) != 0; }

static int rtl_send(const void *frame, unsigned len)
{
    if (!R.ok || !frame || len < 14 || len > MAX_FRAME - 4)
        return -1;
    Desc *d = &R.tx[R.tx_next];
    if (d->opts1 & DESC_OWN) /* the card has not sent the one that was here last time round */
    {
        R.tx_busy++;
        return -1;
    }
    u8 *buf = R.tx_buf + (u64)R.tx_next * BUF_SIZE;
    copy(buf, frame, len);
    unsigned total = len;
    while (total < MIN_FRAME) /* the card sends what it is given: pad short frames (ARP is 42 bytes) with zeros */
        buf[total++] = 0;
    d->addr = (u64)buf;
    d->opts2 = 0;
    fence(); /* address and length before the ownership bit */
    d->opts1 = DESC_OWN | DESC_FIRST | DESC_LAST | (R.tx_next == TX_COUNT - 1 ? DESC_END : 0) | total;
    fence();
    R.tx_next = (R.tx_next + 1) % TX_COUNT;
    R.tx_frames++;
    w8(R_TXPOLL, TXPOLL_NPQ);
    return 0;
}

static int rtl_recv(void *buf, unsigned max)
{
    if (!R.ok)
        return 0;
    Desc *d = &R.rx[R.rx_next];
    u32 st = d->opts1;
    if (st & DESC_OWN) /* still the card's: nothing has arrived */
        return 0;
    fence();
    unsigned len = st & LEN_MASK; /* includes the 4 byte CRC */
    int got = 0;
    if ((st & (DESC_FIRST | DESC_LAST)) == (DESC_FIRST | DESC_LAST) && !(st & RX_RES) && len >= 18 && len <= BUF_SIZE)
    {
        unsigned n = len - 4; /* without the CRC */
        got = (int)(n < max ? n : max);
        copy(buf, R.rx_buf + (u64)R.rx_next * BUF_SIZE, (unsigned long)got);
        R.rx_frames++;
    }
    else
        R.rx_dropped++;
    /* hand the buffer back to the card */
    d->opts2 = 0;
    fence();
    d->opts1 = DESC_OWN | (R.rx_next == RX_COUNT - 1 ? DESC_END : 0) | BUF_SIZE;
    fence();
    R.rx_next = (R.rx_next + 1) % RX_COUNT;
    return got;
}

static void rtl_status(char *buf, unsigned cap)
{
    NicText t;
    nic_text_init(&t, buf, cap);
    if (!R.ok)
    {
        nic_puts(&t, "RTL8168 failed: ");
        nic_puts(&t, R.fail ? R.fail : "not started");
        return;
    }
    if (R.ver_index >= 0)
        nic_puts(&t, CHIPS[R.ver_index].name);
    else
    {
        nic_puts(&t, "RTL8168 rev ");
        nic_puth(&t, R.xid, 3);
    }
    u8 ph = r8(R_PHYSTATUS);
    if (!(ph & PHY_LINK))
        nic_puts(&t, ", link down");
    else
    {
        nic_puts(&t, ", ");
        nic_puts(&t, (ph & PHY_1000) ? "1000M" : (ph & PHY_100) ? "100M" : (ph & PHY_10) ? "10M" : "link");
        nic_puts(&t, (ph & PHY_FULL) ? " full" : " half");
    }
    nic_puts(&t, ", rx ");
    nic_putd(&t, R.rx_frames);
    nic_puts(&t, " tx ");
    nic_putd(&t, R.tx_frames);
    if (R.rx_dropped)
    {
        nic_puts(&t, " bad ");
        nic_putd(&t, R.rx_dropped);
    }
}

const NicDriver NIC_RTL8168 = {
    "Realtek RTL8168", rtl_claims, rtl_probe, rtl_mac, rtl_link_up, rtl_send, rtl_recv, rtl_status, 0, 0, 0, 0,
};
