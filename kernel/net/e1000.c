/* Intel e1000 (82540EM, the NIC QEMU emulates with -device e1000) driver. See include/net.h.
 *
 * Polled, one adapter. The card has registers at the address in PCI BAR0. Frames travel through two rings of
 * 16-byte descriptors in RAM: each descriptor points at a 2 KiB buffer. For receiving, the card fills the
 * buffer at the head (RDH) and sets the DD ("descriptor done") bit; we hand the descriptor back by moving the
 * tail (RDT) forward. For sending, we fill the descriptor at the tail (TDT), advance the tail, and the card
 * sets DD when the frame has left. Both rings are physically contiguous because the card uses physical addresses. */
#include "idle.h"
#include "io.h"
#include "ioapic.h"
#include "net.h"
#include "nic.h"
#include "pci.h"
#include "pmm.h"
#include "smp.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define R_CTRL 0x0000
#define R_STATUS 0x0008
#define R_EERD 0x0014
#define R_ICS 0x00C8 /* interrupt cause set: software can raise a cause (a test of the wiring) */
#define R_ICR 0x00C0
#define R_IMS 0x00D0 /* interrupt mask set: which causes may interrupt */
#define R_IMC 0x00D8
#define R_RCTL 0x0100
#define R_TCTL 0x0400
#define R_TIPG 0x0410
#define R_RDBAL 0x2800
#define R_RDBAH 0x2804
#define R_RDLEN 0x2808
#define R_RDH 0x2810
#define R_RDT 0x2818
#define R_TDBAL 0x3800
#define R_TDBAH 0x3804
#define R_TDLEN 0x3808
#define R_TDH 0x3810
#define R_TDT 0x3818
#define R_MTA 0x5200 /* multicast table: 128 dwords */
#define R_RAL0 0x5400
#define R_RAH0 0x5404

#define CTRL_SLU (1u << 6)  /* set link up */
#define CTRL_RST (1u << 26)
#define STATUS_LU (1u << 1) /* link is up */
#define INT_RXDMT0 (1u << 4) /* interrupt causes: the receive ring is running empty, */
#define INT_RXO (1u << 6)    /* ... a frame was lost because it was full, */
#define INT_RXT0 (1u << 7)   /* ... a frame arrived */
#define RCTL_EN (1u << 1)
#define RCTL_BAM (1u << 15)    /* accept broadcast */
#define RCTL_SECRC (1u << 26)  /* the card strips the CRC */
#define TCTL_EN (1u << 1)
#define TCTL_PSP (1u << 3)     /* pad short packets */

#define RX_COUNT 16
#define TX_COUNT 8
#define BUF_SIZE 2048

#define DESC_DD 0x01u  /* status: done */
#define DESC_EOP 0x02u /* status: last descriptor of the frame */
#define TXCMD_EOP 0x01u
#define TXCMD_IFCS 0x02u /* the card appends the CRC */
#define TXCMD_RS 0x08u   /* report status: set DD when sent */

typedef struct
{
    u64 addr;
    u16 length;
    u16 checksum;
    u8 status;
    u8 errors;
    u16 special;
} __attribute__((packed)) RxDesc;

typedef struct
{
    u64 addr;
    u16 length;
    u8 cso;
    u8 cmd;
    u8 status;
    u8 css;
    u16 special;
} __attribute__((packed)) TxDesc;

typedef struct
{
    volatile u8 *regs;
    volatile RxDesc *rx;
    volatile TxDesc *tx;
    u8 *rx_buf, *tx_buf;
    unsigned rx_next; /* next descriptor the card will fill */
    unsigned tx_next; /* next descriptor we will fill */
    u8 mac[6];
    int ok;
} E1000;

static E1000 NIC;

static inline u32 rd(u32 off) { return *(volatile u32 *)(NIC.regs + off); }
static inline void wr(u32 off, u32 v) { *(volatile u32 *)(NIC.regs + off) = v; }
static inline void fence(void) { __asm__ volatile("mfence" ::: "memory"); }

/* a read of port 0x80 takes about 1 us: a delay that needs no timer */
static void udelay(unsigned us)
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}

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

/* One 16-bit word of the card's EEPROM (the MAC address lives in words 0..2), or -1 on a timeout. */
static int eeprom_word(unsigned addr)
{
    wr(R_EERD, (addr << 8) | 1u);
    for (unsigned i = 0; i < 1000; i++)
    {
        u32 v = rd(R_EERD);
        if (v & (1u << 4)) /* done */
            return (int)(v >> 16);
        udelay(10);
    }
    return -1;
}

static int read_mac(void)
{
    u32 ral = rd(R_RAL0), rah = rd(R_RAH0);
    if (rah & (1u << 31)) /* address valid: the firmware or the emulator already loaded it */
    {
        NIC.mac[0] = (u8)ral;
        NIC.mac[1] = (u8)(ral >> 8);
        NIC.mac[2] = (u8)(ral >> 16);
        NIC.mac[3] = (u8)(ral >> 24);
        NIC.mac[4] = (u8)rah;
        NIC.mac[5] = (u8)(rah >> 8);
        return 1;
    }
    for (int i = 0; i < 3; i++)
    {
        int w = eeprom_word((unsigned)i);
        if (w < 0)
            return 0;
        NIC.mac[2 * i] = (u8)w;
        NIC.mac[2 * i + 1] = (u8)(w >> 8);
    }
    wr(R_RAL0, (u32)NIC.mac[0] | (u32)NIC.mac[1] << 8 | (u32)NIC.mac[2] << 16 | (u32)NIC.mac[3] << 24);
    wr(R_RAH0, (u32)NIC.mac[4] | (u32)NIC.mac[5] << 8 | (1u << 31));
    return 1;
}

static int bring_up(const PciDevice *pd)
{
    u32 cmd = pci_cfg_read(pd->bus, pd->dev, pd->fn, 4);
    pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (cmd & 0xFFFFu) | 0x6u); /* memory space + bus master */
    u32 bar0 = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10);
    if (bar0 & 1u) /* an I/O port BAR: not the register window */
        return 0;
    u64 base = bar0 & ~0xFULL;
    if (((bar0 >> 1) & 3u) == 2u)
        base |= (u64)pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x14) << 32;
    if (!base)
        return 0;

    zero(&NIC, sizeof NIC);
    NIC.regs = (volatile u8 *)base;

    /* reset, then no interrupts (polled) */
    wr(R_CTRL, rd(R_CTRL) | CTRL_RST);
    udelay(2000);
    wr(R_IMC, 0xFFFFFFFFu);
    (void)rd(R_ICR);

    if (!read_mac())
        return 0;

    /* DMA memory: page 0 = both descriptor rings, then the receive buffers, then the send buffers */
    unsigned rx_pages = RX_COUNT * BUF_SIZE / PMM_PAGE_SIZE;
    unsigned tx_pages = TX_COUNT * BUF_SIZE / PMM_PAGE_SIZE;
    unsigned pages = 1 + rx_pages + tx_pages;
    u64 mem = pmm_alloc_contiguous_tag(pages, PMM_TAG_OTHER);
    if (!mem)
        return 0;
    zero((void *)mem, (unsigned long)pages * PMM_PAGE_SIZE);
    NIC.rx = (volatile RxDesc *)mem;
    NIC.tx = (volatile TxDesc *)(mem + RX_COUNT * sizeof(RxDesc));
    NIC.rx_buf = (u8 *)(mem + PMM_PAGE_SIZE);
    NIC.tx_buf = NIC.rx_buf + (u64)rx_pages * PMM_PAGE_SIZE;
    for (unsigned i = 0; i < RX_COUNT; i++)
        NIC.rx[i].addr = (u64)(NIC.rx_buf + (u64)i * BUF_SIZE);

    for (unsigned i = 0; i < 128; i++) /* no multicast filtering */
        wr(R_MTA + 4 * i, 0);

    /* receive ring: the card owns every descriptor, so the tail is the last one */
    wr(R_RDBAL, (u32)(u64)NIC.rx);
    wr(R_RDBAH, (u32)((u64)NIC.rx >> 32));
    wr(R_RDLEN, RX_COUNT * sizeof(RxDesc));
    wr(R_RDH, 0);
    wr(R_RDT, RX_COUNT - 1);
    wr(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC); /* 2048 byte buffers = BSIZE 0 */

    /* send ring: empty, head = tail */
    wr(R_TDBAL, (u32)(u64)NIC.tx);
    wr(R_TDBAH, (u32)((u64)NIC.tx >> 32));
    wr(R_TDLEN, TX_COUNT * sizeof(TxDesc));
    wr(R_TDH, 0);
    wr(R_TDT, 0);
    wr(R_TIPG, 0x0060200Au);                                  /* the values the datasheet gives for copper */
    wr(R_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12)); /* collision threshold + distance */

    wr(R_CTRL, rd(R_CTRL) | CTRL_SLU);
    NIC.ok = 1;
    return 1;
}

/* 82540EM (QEMU "e1000") and 82545EM. Others are not handled: QEMU's "e1000e" (82574L) differs, real laptops have I217/I219. */
static int e1000_claims(const PciDevice *pd)
{
    return pd->vendor_id == 0x8086 && (pd->device_id == 0x100E || pd->device_id == 0x100F);
}


static const unsigned char *e1000_mac(void) { return NIC.mac; }
static int e1000_link_up(void) { return NIC.ok && (rd(R_STATUS) & STATUS_LU) != 0; }

static int e1000_send(const void *frame, unsigned len)
{
    if (!NIC.ok || !frame || len < 14 || len > NET_MAX_FRAME - 4)
        return -1;
    volatile TxDesc *d = &NIC.tx[NIC.tx_next];
    /* a descriptor that was used before must be done; a never used one has status 0 and length 0 */
    if (d->length != 0 && !(d->status & DESC_DD))
        return -1;
    u8 *buf = NIC.tx_buf + (u64)NIC.tx_next * BUF_SIZE;
    copy(buf, frame, len);
    d->addr = (u64)buf;
    d->length = (u16)len;
    d->cmd = TXCMD_EOP | TXCMD_IFCS | TXCMD_RS;
    d->status = 0;
    fence();
    NIC.tx_next = (NIC.tx_next + 1) % TX_COUNT;
    wr(R_TDT, NIC.tx_next);
    return 0;
}

static int e1000_recv(void *buf, unsigned max)
{
    if (!NIC.ok)
        return 0;
    volatile RxDesc *d = &NIC.rx[NIC.rx_next];
    if (!(d->status & DESC_DD))
        return 0;
    fence();
    unsigned len = d->length;
    int got = 0;
    if ((d->status & DESC_EOP) && !d->errors && len <= BUF_SIZE) /* 2 KiB buffers: one frame per descriptor */
    {
        got = (int)(len < max ? len : max);
        copy(buf, NIC.rx_buf + (u64)NIC.rx_next * BUF_SIZE, (unsigned long)got);
    }
    /* give the descriptor back to the card; a bad frame is dropped (got stays 0) */
    d->status = 0;
    wr(R_RDT, NIC.rx_next);
    NIC.rx_next = (NIC.rx_next + 1) % RX_COUNT;
    return got;
}

/* ---- interrupts ---------------------------------------------------------------------------------------------------------------
 * The card can interrupt when a frame has arrived. The handler does as little as possible: it reads ICR (which also clears the
 * causes and lets go of the wire) and leaves a note; net_poll() later reads the frames. A lost or missing interrupt therefore
 * costs only time, never data: the ring keeps the frames, and net_poll() is still called every few milliseconds. */
#define IRQ_CAUSES (INT_RXT0 | INT_RXO | INT_RXDMT0)
#define IRQ_SPURIOUS_MAX 1000 /* interrupts without a cause of ours before the line is given up (something else holds it) */

static volatile unsigned IRQ_COUNT, IRQ_SPURIOUS;
static volatile int IRQ_FLAG;   /* a frame may be waiting */
static volatile int IRQ_ACTIVE; /* interrupts are in use */
static unsigned IRQ_GSI;

/* Called by the interrupt entry code in idle.c (interrupts off, on whatever stack the CPU switched to). */
void net_irq_handler(void)
{
    if (!NIC.regs)
        return;
    unsigned icr = rd(R_ICR); /* reading clears it */
    if (icr & IRQ_CAUSES)
    {
        IRQ_COUNT++;
        IRQ_FLAG = 1;
        smp_wake_idle(); /* the poller that takes the flag (the main loop) may sleep on another processor: end its sleep now, not at the next tick */
    }
    else if (++IRQ_SPURIOUS >= IRQ_SPURIOUS_MAX) /* the wire is held by another device that nobody serves: stop listening, poll again */
    {
        wr(R_IMC, 0xFFFFFFFFu);
        ioapic_mask(IRQ_GSI);
        IRQ_ACTIVE = 0;
    }
}

/* Which I/O APIC line is the card's wire? Nothing here reads the ACPI routing tables, so find out by asking: for each PCI-style
 * line (GSI 16 and up) send it to our vector, make the card raise a cause by itself (ICS), and see whether the handler runs.
 * Returns the GSI, or -1 (no I/O APIC / local APIC, or no line answers: polling goes on as before). Call it with the card
 * working, once; it briefly enables interrupts (idle_wait). */
static int e1000_irq_init(void)
{
    if (!NIC.ok || !idle_ready() || !ioapic_present())
        return -1;
    unsigned apic = idle_lapic_id();
    unsigned lines = ioapic_gsi_count();
    for (unsigned gsi = 16; gsi < lines; gsi++)
    {
        IRQ_GSI = gsi;
        IRQ_SPURIOUS = 0;
        if (ioapic_route(gsi, IDLE_VEC_NET, apic) != 0)
            continue;
        unsigned before = IRQ_COUNT;
        IRQ_ACTIVE = 1;
        wr(R_IMS, IRQ_CAUSES);
        wr(R_ICS, INT_RXT0); /* "a frame arrived", though none did */
        for (int i = 0; i < 3 && IRQ_COUNT == before; i++)
            idle_wait(); /* sti; hlt: returns at the latest at the next 1 ms timer tick */
        wr(R_IMC, 0xFFFFFFFFu);
        (void)rd(R_ICR);
        ioapic_mask(gsi);
        IRQ_ACTIVE = 0;
        if (IRQ_COUNT != before)
        {
            IRQ_SPURIOUS = 0;
            IRQ_FLAG = 1; /* (a frame may have come while we looked) */
            ioapic_route(gsi, IDLE_VEC_NET, smp_irq_target()); /* found it on this processor; from now on an application processor takes the interrupts (smp.h) */
            IRQ_ACTIVE = 1;
            wr(R_IMS, IRQ_CAUSES);
            return (int)gsi;
        }
    }
    return -1;
}

static int e1000_irq_active(void) { return IRQ_ACTIVE; }
static unsigned e1000_irq_count(void) { return IRQ_COUNT; }

static int e1000_irq_take(void)
{
    int f = IRQ_FLAG;
    IRQ_FLAG = 0;
    return f;
}

static void e1000_status(char *buf, unsigned cap)
{
    NicText t;
    nic_text_init(&t, buf, cap);
    nic_puts(&t, "Intel e1000, ");
    nic_puts(&t, e1000_link_up() ? "link up" : "link down");
    nic_puts(&t, e1000_irq_active() ? ", interrupts" : ", polled");
}

const NicDriver NIC_E1000 = {
    "Intel e1000", e1000_claims, bring_up, e1000_mac, e1000_link_up, e1000_send, e1000_recv, e1000_status,
    e1000_irq_init, e1000_irq_active, e1000_irq_take, e1000_irq_count,
};
