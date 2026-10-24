/* Host test for rtl8168.c against a pretend card: the register file is plain memory (so what the driver writes can be read back),
 * the delay hook plays the chip finishing its reset, and the test plays the DMA engine by flipping the descriptors' ownership
 * bit. This proves the driver's own logic (register values and order of use, ring layout, ownership, padding, error frames,
 * wrap around, failure texts). It cannot prove that the real chip behaves like the model: that needs real hardware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmm.h"
#include "../kernel/net/rtl8168.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- the pretend card ---- */
static _Alignas(4096) unsigned char REGS[0x1000];
static int RESET_FINISHES = 1; /* 0: the card never clears the reset bit */
static unsigned UDELAYS;

void rtl_host_udelay(unsigned us)
{
    (void)us;
    UDELAYS++;
    if (RESET_FINISHES && (REGS[R_CMD] & CMD_RESET))
        REGS[R_CMD] &= (unsigned char)~CMD_RESET; /* the chip clears the bit when its reset is done */
}

static void card_init(unsigned txcfg, int link)
{
    memset(REGS, 0, sizeof REGS);
    static const unsigned char mac[6] = {0x00, 0xE0, 0x4C, 0x68, 0x01, 0x02};
    memcpy(REGS + R_MAC0, mac, 6);
    memcpy(REGS + R_TXCFG, &txcfg, 4);
    REGS[R_PHYSTATUS] = (unsigned char)(link ? (PHY_LINK | PHY_FULL | PHY_1000) : 0);
    unsigned ones = 0xFFFFFFFFu;
    memcpy(REGS + R_MISC, &ones, 4);   /* gated, as after a reset */
    memcpy(REGS + R_CPLUS, &ones, 2);  /* every C+ bit set: the driver must clear VLAN stripping and checksum offload */
}

static u32 reg32(unsigned off) { u32 v; memcpy(&v, REGS + off, 4); return v; }
static u16 reg16(unsigned off) { u16 v; memcpy(&v, REGS + off, 2); return v; }

/* ---- PCI and memory stubs ---- */
static u32 CFG_CMD;
static int BAR_MODE; /* 0 = BAR 2 is a 64 bit memory BAR at REGS; 1 = only an I/O BAR */
u32 pci_cfg_read(unsigned bus, unsigned dev, unsigned fn, unsigned off)
{
    (void)bus; (void)dev; (void)fn;
    if (off == 4) return CFG_CMD;
    if (off == 0x10) return 1u; /* BAR 0: I/O ports */
    if (BAR_MODE == 1) return 0;
    if (off == 0x18) return (u32)(unsigned long long)(size_t)REGS | 4u; /* BAR 2: memory, 64 bit */
    if (off == 0x1C) return (u32)((unsigned long long)(size_t)REGS >> 32);
    return 0;
}
void pci_cfg_write(unsigned bus, unsigned dev, unsigned fn, unsigned off, u32 v)
{
    (void)bus; (void)dev; (void)fn;
    if (off == 4) CFG_CMD = v;
}
unsigned long long pmm_alloc_contiguous_tag(unsigned long long count, PmmTag tag)
{
    (void)tag;
    unsigned char *raw = malloc((size_t)count * 4096 + 4096); /* (MinGW has no aligned_alloc: align by hand; the test never frees) */
    void *p = (void *)(((size_t)raw + 4095) & ~(size_t)4095);
    return (unsigned long long)(size_t)p;
}
int io_dummy;

static const PciDevice PD = {0, 0x19, 0, 0x10EC, 0x8168, 0x02, 0x00, 0x00, 0x0C, 0};

/* what the card would do with a send descriptor: send it and give it back */
static void card_sent(Desc *d) { d->opts1 &= ~DESC_OWN; }

/* what the card would do with a receive descriptor: store a frame of n bytes (with a CRC) and give the descriptor back */
static void card_received(Desc *d, const unsigned char *frame, unsigned n, u32 flags)
{
    u8 *buf = (u8 *)(size_t)d->addr;
    memcpy(buf, frame, n);
    memset(buf + n, 0xCC, 4); /* the CRC */
    d->opts1 = (d->opts1 & DESC_END) | flags | (n + 4);
}

int main(void)
{
    /* ---- the claim ---- */
    CHECK(NIC_RTL8168.claims(&PD));
    PciDevice other = PD;
    other.device_id = 0x8136; /* RTL8101: a different family */
    CHECK(!NIC_RTL8168.claims(&other));
    other = PD;
    other.vendor_id = 0x8086;
    CHECK(!NIC_RTL8168.claims(&other));

    /* ---- a successful start of an 8168g (XID 0x4C0) with the link up ---- */
    card_init(0x4C0u << 20, 1);
    CHECK(NIC_RTL8168.probe(&PD) == 1);
    CHECK((CFG_CMD & 6) == 6);                       /* memory space and bus master turned on */
    CHECK(R.ver_index >= 0 && strcmp(CHIPS[R.ver_index].name, "RTL8168g/8111g") == 0 && CHIPS[R.ver_index].ver == 40);
    CHECK(memcmp(NIC_RTL8168.mac(), "\x00\xE0\x4C\x68\x01\x02", 6) == 0);
    CHECK(NIC_RTL8168.link_up());
    CHECK(REGS[R_CMD] == (CMD_TXEN | CMD_RXEN));    /* reset finished, both units on */
    CHECK(REGS[R_9346] == CFG_LOCK);                 /* the configuration registers were locked again */
    CHECK(reg16(R_CPLUS) == 0x2003);                 /* VLAN stripping and RX checksum offload cleared */
    CHECK(reg16(R_RMS) == MAX_FRAME && reg16(R_INTMIT) == 0 && reg16(R_IMR) == 0);
    CHECK(REGS[R_MTPS] == 0x27);                     /* 8168evl and later */
    CHECK(reg32(R_RXCFG) == (RXCFG_128_INT | RXCFG_MULTI_EN | RXCFG_DMA_BURST | RXCFG_EARLY_OFF | ACCEPT_BROADCAST | ACCEPT_MYPHYS));
    CHECK(reg32(R_TXCFG) == (TXCFG_DMA_BURST | TXCFG_IFG | TXCFG_AUTO_FIFO));
    CHECK((reg32(R_MISC) & MISC_RXDV_GATED) == 0 && (reg32(R_MISC) | MISC_RXDV_GATED) == 0xFFFFFFFFu); /* only that bit */
    CHECK(reg32(R_MAR0) == 0 && reg32(R_MAR0 + 4) == 0);

    /* the rings the card was told about */
    u64 tx_addr = (u64)reg32(R_TNPDS) | (u64)reg32(R_TNPDS + 4) << 32;
    u64 rx_addr = (u64)reg32(R_RDSAR) | (u64)reg32(R_RDSAR + 4) << 32;
    CHECK(tx_addr == (u64)(size_t)R.tx && rx_addr == (u64)(size_t)R.rx);
    CHECK((tx_addr & 255) == 0 && (rx_addr & 255) == 0); /* the card wants 256 byte alignment */
    CHECK((u64)(size_t)R.tx >= (u64)(size_t)R.rx + RX_COUNT * sizeof(Desc)); /* the rings do not overlap */
    CHECK(sizeof(Desc) == 16);
    for (unsigned i = 0; i < RX_COUNT; i++)
    {
        CHECK(R.rx[i].opts1 == (DESC_OWN | (i == RX_COUNT - 1 ? DESC_END : 0) | BUF_SIZE)); /* the card owns every buffer, the last ends the ring */
        CHECK(R.rx[i].addr == (u64)(size_t)(R.rx_buf + (size_t)i * BUF_SIZE));
    }
    for (unsigned i = 0; i < TX_COUNT; i++)
        CHECK(R.tx[i].opts1 == (i == TX_COUNT - 1 ? DESC_END : 0)); /* nothing to send, the last ends the ring */

    /* ---- status text ---- */
    char st[96];
    NIC_RTL8168.status(st, sizeof st);
    CHECK(strstr(st, "RTL8168g/8111g") && strstr(st, "1000M full") && strstr(st, "rx 0 tx 0"));
    NIC_RTL8168.status(st, 12); /* a short buffer is cut and terminated */
    CHECK(strlen(st) == 11);

    /* ---- sending: padded to 60, owned by the card, the card is poked ---- */
    unsigned char arp[42];
    for (int i = 0; i < 42; i++)
        arp[i] = (unsigned char)(i + 1);
    CHECK(NIC_RTL8168.send(arp, sizeof arp) == 0);
    CHECK(R.tx[0].opts1 == (DESC_OWN | DESC_FIRST | DESC_LAST | 60));
    CHECK(R.tx[0].addr == (u64)(size_t)R.tx_buf);
    CHECK(memcmp(R.tx_buf, arp, 42) == 0);
    int pad_ok = 1;
    for (int i = 42; i < 60; i++)
        pad_ok &= R.tx_buf[i] == 0;
    CHECK(pad_ok);
    CHECK(REGS[R_TXPOLL] == TXPOLL_NPQ);
    unsigned char big[1500];
    memset(big, 0x5A, sizeof big);
    CHECK(NIC_RTL8168.send(big, sizeof big) == 0);
    CHECK(R.tx[1].opts1 == (DESC_OWN | DESC_FIRST | DESC_LAST | 1500)); /* long frames are not padded */
    CHECK(NIC_RTL8168.send(big, 10) == -1 && NIC_RTL8168.send(0, 60) == -1); /* shorter than an Ethernet header / no data */
    CHECK(NIC_RTL8168.send(big, MAX_FRAME) == -1);                         /* longer than the card takes */
    card_sent(&R.tx[0]);
    card_sent(&R.tx[1]);
    /* the ring wraps; the last descriptor keeps the end mark */
    for (int i = 0; i < 6; i++)
    {
        CHECK(NIC_RTL8168.send(arp, sizeof arp) == 0);
        card_sent(&R.tx[2 + i]);
    }
    CHECK((R.tx[TX_COUNT - 1].opts1 & DESC_END) == DESC_END || R.tx[TX_COUNT - 1].opts1 == (DESC_END | 60)); /* sent by the card: END stays */
    CHECK(R.tx_next == 0);
    /* when the card has not finished the descriptor that is next, the send is refused (and counted), not overwritten */
    CHECK(NIC_RTL8168.send(arp, sizeof arp) == 0 && R.tx_next == 1);
    for (int i = 1; i < TX_COUNT; i++)
        CHECK(NIC_RTL8168.send(arp, sizeof arp) == 0); /* 7 more: descriptor 0 .. all owned by the card now */
    CHECK(NIC_RTL8168.send(arp, sizeof arp) == -1 && R.tx_busy == 1);
    for (int i = 0; i < TX_COUNT; i++)
        card_sent(&R.tx[i]);
    CHECK(NIC_RTL8168.send(arp, sizeof arp) == 0);

    /* ---- receiving ---- */
    unsigned char rbuf[2000];
    CHECK(NIC_RTL8168.recv(rbuf, sizeof rbuf) == 0); /* the card still owns every descriptor: nothing arrived */
    unsigned char frame[100];
    for (int i = 0; i < 100; i++)
        frame[i] = (unsigned char)(0xA0 + i);
    card_received(&R.rx[0], frame, 100, DESC_FIRST | DESC_LAST);
    int n = NIC_RTL8168.recv(rbuf, sizeof rbuf);
    CHECK(n == 100 && memcmp(rbuf, frame, 100) == 0);                 /* without the CRC */
    CHECK(R.rx[0].opts1 == (DESC_OWN | BUF_SIZE) && R.rx_next == 1);   /* given back to the card */
    CHECK(NIC_RTL8168.recv(rbuf, sizeof rbuf) == 0);                   /* next one is still the card's */
    /* a frame with the error flag, a runt, a frame split over two descriptors: dropped, descriptor still given back */
    card_received(&R.rx[1], frame, 100, DESC_FIRST | DESC_LAST | RX_RES);
    CHECK(NIC_RTL8168.recv(rbuf, sizeof rbuf) == 0 && (R.rx[1].opts1 & DESC_OWN) && R.rx_dropped == 1);
    card_received(&R.rx[2], frame, 10, DESC_FIRST | DESC_LAST);
    CHECK(NIC_RTL8168.recv(rbuf, sizeof rbuf) == 0 && (R.rx[2].opts1 & DESC_OWN) && R.rx_dropped == 2);
    card_received(&R.rx[3], frame, 100, DESC_FIRST);
    CHECK(NIC_RTL8168.recv(rbuf, sizeof rbuf) == 0 && (R.rx[3].opts1 & DESC_OWN) && R.rx_dropped == 3);
    /* a buffer smaller than the frame gets the start of it */
    card_received(&R.rx[4], frame, 100, DESC_FIRST | DESC_LAST);
    CHECK(NIC_RTL8168.recv(rbuf, 30) == 30 && memcmp(rbuf, frame, 30) == 0);
    /* wrap around the ring: the last descriptor keeps its end mark when it is given back */
    R.rx_next = RX_COUNT - 1;
    card_received(&R.rx[RX_COUNT - 1], frame, 60, DESC_FIRST | DESC_LAST);
    CHECK(NIC_RTL8168.recv(rbuf, sizeof rbuf) == 60);
    CHECK(R.rx[RX_COUNT - 1].opts1 == (DESC_OWN | DESC_END | BUF_SIZE) && R.rx_next == 0);
    NIC_RTL8168.status(st, sizeof st);
    CHECK(strstr(st, "rx 3") && strstr(st, "bad 3")); /* 3 good frames (100, 30 of 100, 60) and 3 dropped */

    /* ---- other revisions: the receive configuration and the send FIFO follow Linux' table ---- */
    CHECK(rxcfg_for(17) == (RXCFG_FIFO_THRESH | RXCFG_DMA_BURST));
    CHECK(rxcfg_for(20) == (RXCFG_128_INT | RXCFG_MULTI_EN | RXCFG_DMA_BURST));
    CHECK(rxcfg_for(33) == (RXCFG_128_INT | RXCFG_DMA_BURST));
    CHECK(rxcfg_for(35) == (RXCFG_128_INT | RXCFG_MULTI_EN | RXCFG_DMA_BURST));
    CHECK(rxcfg_for(51) == (RXCFG_128_INT | RXCFG_MULTI_EN | RXCFG_DMA_BURST | RXCFG_EARLY_OFF));
    CHECK(!evl_up(33) && evl_up(34) && evl_up(35) && !evl_up(36) && !evl_up(38) && evl_up(40) && evl_up(52));
    card_init(0x2C8u << 20, 0); /* 8168evl, no cable */
    CHECK(NIC_RTL8168.probe(&PD) == 1 && CHIPS[R.ver_index].ver == 34 && !NIC_RTL8168.link_up());
    NIC_RTL8168.status(st, sizeof st);
    CHECK(strstr(st, "RTL8168evl/8111evl") && strstr(st, "link down"));
    CHECK((reg32(R_MISC) & MISC_RXDV_GATED) != 0); /* before the 8168g family the gate bit is not touched */
    card_init(0x3D0u << 20, 1); /* an XID bit outside the revision mask (0x10) is ignored: still the 8168c of 0x3C0 */
    CHECK(NIC_RTL8168.probe(&PD) == 1 && CHIPS[R.ver_index].ver == 19);
    CHECK(REGS[R_MTPS] == 63 && reg32(R_TXCFG) == (TXCFG_DMA_BURST | TXCFG_IFG));
    card_init(0x7F0u << 20, 1); /* a revision that is not in the table: it still starts, with generic settings, and says so */
    CHECK(NIC_RTL8168.probe(&PD) == 1 && R.ver_index == -1);
    NIC_RTL8168.status(st, sizeof st);
    CHECK(strstr(st, "RTL8168 rev "));

    /* ---- failures: each says why, and the card is not used ---- */
    memset(REGS, 0xFF, sizeof REGS);
    CHECK(NIC_RTL8168.probe(&PD) == 0);
    NIC_RTL8168.status(st, sizeof st);
    CHECK(strcmp(st, "RTL8168 failed: registers read as all ones") == 0);
    CHECK(NIC_RTL8168.recv(rbuf, sizeof rbuf) == 0 && NIC_RTL8168.send(arp, 42) == -1 && !NIC_RTL8168.link_up());
    card_init(0x4C0u << 20, 1);
    RESET_FINISHES = 0;
    CHECK(NIC_RTL8168.probe(&PD) == 0);
    NIC_RTL8168.status(st, sizeof st);
    CHECK(strcmp(st, "RTL8168 failed: reset did not finish") == 0);
    RESET_FINISHES = 1;
    card_init(0x4C0u << 20, 1);
    memset(REGS + R_MAC0, 0, 6);
    CHECK(NIC_RTL8168.probe(&PD) == 0);
    NIC_RTL8168.status(st, sizeof st);
    CHECK(strcmp(st, "RTL8168 failed: no valid MAC address") == 0);
    card_init(0x4C0u << 20, 1);
    REGS[R_MAC0] = 0x01; /* a multicast bit */
    CHECK(NIC_RTL8168.probe(&PD) == 0);
    card_init(0x4C0u << 20, 1);
    BAR_MODE = 1;
    CHECK(NIC_RTL8168.probe(&PD) == 0);
    NIC_RTL8168.status(st, sizeof st);
    CHECK(strcmp(st, "RTL8168 failed: no memory BAR") == 0);
    BAR_MODE = 0;
    card_init(0x4C0u << 20, 1);
    CHECK(NIC_RTL8168.probe(&PD) == 1); /* and a good card still starts afterwards */

    if (fails == 0)
        printf("rtl8168 tests passed\n");
    return fails ? 1 : 0;
}
