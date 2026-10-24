/* Realtek RTL8821CE (PCI 10EC:B821 / 10EC:C821): 802.11ac Wi-Fi + Bluetooth on PCIe. See kernel/net/nic.h.
 *
 * STAGES 1 TO 3 OF A LONG ROAD: this is NOT a working network driver. probe() always answers 0 ("no working adapter") and the Device
 * Manager line reports what happened, so that the real state of the card in a real PC is known before anything is built on it.
 *   stage 1   identify: enable PCI memory space and read a few chip registers (only reads).
 *   stage 2   power on, the way Linux does (rtw_mac_power_on): a MAC that is off is powered on (rtw_mac_pre_system_cfg + the "card
 *             enable" flow + the system configuration); one that is already on is powered off and on again. Seen to work on a real card.
 *   stage 3a  find the firmware file (RTW8821C.BIN on any drive) and check its header and size.
 *   stage 3b  set up the PCIe DMA rings and load the firmware into the chip (reserved-page "beacon" DMA + DDMA), start its CPU and wait
 *             for "firmware ready"; then power the chip off again. FIRST USE OF DMA: only tested against a model on the host.
 * All of it was written from the Linux driver (drivers/net/wireless/realtek/rtw88: reg.h, pci.h, mac.c, fw.c, pci.c, tx.c, rtw8821c.c)
 * without a data sheet. The tables and every register number were compared with Linux' own definitions by compiling both. Deliberate
 * differences from Linux: the "firmware still resident" handshake (a toggle of the RPWM register, whose address was not confirmed) is
 * skipped (such a card is left alone); the PCIe PHY / ASPM configuration (rtw_pci_phy_cfg, rtw_pci_link_cfg) is skipped; the Bluetooth
 * coexistence register that Linux saves around the download is left alone; the receive ring is small (the download receives nothing).
 *
 * The road ahead, in the order it can be built and tested on the real card:
 *   4. keep the firmware running; firmware commands (H2C) and events (C2H), then the MAC / baseband / RF tables of rtw8821c.c;
 *   5. 802.11 itself: scan (probe requests, beacons), authenticate, associate, data frames in and out;
 *   6. WPA2-PSK: PBKDF2-HMAC-SHA1 for the key, the 4-way handshake, AES-CCMP for every frame.
 *
 * Switches (compile time): -DRTL8821CE_POWER_ON=0 = read only again; -DRTL8821CE_FW_LOAD=0 = power on but no DMA and no firmware;
 * -DRTL8821CE_OFF_AFTER=0 = leave the chip powered on after a successful firmware start. */
#include "exec.h"
#include "io.h"
#include "net.h"
#include "nic.h"
#include "pci.h"
#include "pmm.h"
#include "vfs.h"

#ifndef RTL8821CE_POWER_ON
#define RTL8821CE_POWER_ON 1
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define R_SYS_FUNC_EN 0x0002 /* 16 bit: which blocks of the chip are enabled */
#define R_SYS_PW_CTRL 0x0004 /* 16 bit: power control / state machine (byte 5 holds the power switch bits) */
#define R_MCUFW_CTRL 0x0080  /* 16 bit: 0xC078 means a firmware from an earlier user of the card is still in the chip */
#define R_SYS_CFG1 0x00F0    /* 32 bit: bit 23 RTL id, bits 19..16 vendor, bits 15..12 chip cut (0 = A, 1 = B, 2 = C ...) */
#define R_SYS_CFG2 0x00FC    /* 32 bit: more chip identity */
#define R_CR 0x0100          /* byte: Linux treats 0xEA as "MAC powered off", anything else as "powered on" (rtw_mac_power_switch) */
#define PCI_BAR_MMIO 2       /* rtw88 maps BAR 2 */

/* for the power-on (rtw_mac_pre_system_cfg, PCIe, chip with the 3081 CPU) */
#define R_RSV_CTRL 0x001C
#define R_RF_CTRL 0x001F
#define R_GPIO_MUXCFG 0x0040
#define R_LED_CFG 0x004C
#define R_PAD_CTRL1 0x0064
#define R_HCI_OPT_CTRL 0x0074
#define R_WLRF1 0x00EC

#define POLL_TRIES 20000     /* RTW_PWR_POLLING_CNT: tries of 50 us, one second in all */
#define BIT(n) (1u << (n))

typedef struct
{
    volatile u8 *regs;
    int ok;           /* the registers were readable */
    u32 cfg1, cfg2;
    u16 func_en, pw_ctrl, mcufw;
    u8 cr;
    /* stage 2 */
    int pwr_state;    /* PWR_* */
    int pwr_step;     /* the step that failed (1-based), 0 = none */
    int pwr_phase;    /* PHASE_OFF / PHASE_ON: which flow that step belongs to */
    int was_on;       /* the MAC was on when we came (so it was powered off first) */
    u8 cr2;
    u16 func_en2, pw_ctrl2, mcufw2;
    const char *fail; /* static text */
} Wifi;

enum { PWR_NOT_TRIED, PWR_LEFT_ALONE, PWR_OK, PWR_FAILED, PWR_DISABLED };

static Wifi W;

#ifdef HARIS_HOST
/* the host test (tests/test_rtl8821ce.c) plays the chip: its delay hook is where the "chip" finishes what it was asked to do */
void rtl8821ce_host_udelay(unsigned us);
static void udelay(unsigned us) { rtl8821ce_host_udelay(us); }
#else
/* a read of port 0x80 takes about 1 us: a delay that needs no timer */
static void udelay(unsigned us)
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}
#endif

#ifdef HARIS_HOST
/* ... and every write to the chip is reported, so that the test can check the exact order of the sequence */
void rtl8821ce_host_log(u32 off, unsigned size, u32 value);
#define WRITE_LOG(off, size, v) rtl8821ce_host_log((off), (size), (u32)(v))
#else
#define WRITE_LOG(off, size, v) ((void)0)
#endif

static u8 rd8(u32 off) { return *(volatile u8 *)(W.regs + off); }
static u16 rd16(u32 off) { return *(volatile u16 *)(W.regs + off); }
static u32 rd32(u32 off) { return *(volatile u32 *)(W.regs + off); }
static void wr8(u32 off, u8 v)
{
    *(volatile u8 *)(W.regs + off) = v;
    WRITE_LOG(off, 1, v);
}
static void wr32(u32 off, u32 v)
{
    *(volatile u32 *)(W.regs + off) = v;
    WRITE_LOG(off, 4, v);
}

/* The power-on flow of the PCIe interface as Linux' tables give it: cmd 1 = read the byte, replace the bits of `mask` by those of
 * `value`, write it back; cmd 2 = poll until (byte & mask) == (value & mask). The steps of the USB and SDIO interfaces are left out. */
typedef struct
{
    u16 off;
    u8 cmd, mask, value;
} PwrCmd;

#define PWR_WRITE 1
#define PWR_POLL 2

static const PwrCmd POWER_ON[] = {
    /* trans_carddis_to_cardemu_8821c */
    {0x0005, PWR_WRITE, BIT(3) | BIT(4) | BIT(7), 0},
    {0x0300, PWR_WRITE, 0xFF, 0},
    {0x0301, PWR_WRITE, 0xFF, 0},
    /* trans_cardemu_to_act_8821c */
    {0x0005, PWR_WRITE, BIT(4) | BIT(3) | BIT(2), 0},
    {0x0075, PWR_WRITE, BIT(0), BIT(0)},
    {0x0006, PWR_POLL, BIT(1), BIT(1)},
    {0x0075, PWR_WRITE, BIT(0), 0},
    {0x0006, PWR_WRITE, BIT(0), BIT(0)},
    {0x0005, PWR_WRITE, BIT(7), 0},
    {0x0005, PWR_WRITE, BIT(4) | BIT(3), 0},
    {0x0005, PWR_WRITE, BIT(0), BIT(0)},
    {0x0005, PWR_POLL, BIT(0), 0},
    {0x0020, PWR_WRITE, BIT(3), BIT(3)},
    {0x0074, PWR_WRITE, BIT(5), BIT(5)},
    {0x0022, PWR_WRITE, BIT(1), 0},
    {0x0062, PWR_WRITE, BIT(7) | BIT(6) | BIT(5), BIT(7) | BIT(6) | BIT(5)},
    {0x0061, PWR_WRITE, BIT(7) | BIT(6) | BIT(5), 0},
    {0x007C, PWR_WRITE, BIT(1), 0},
};
#define NPOWER_ON ((int)(sizeof POWER_ON / sizeof POWER_ON[0]))

/* The power-off flow (card_disable_flow_8821c), PCIe steps only. Generated from Linux' tables by compiling them, not typed by hand. */
static const PwrCmd POWER_OFF[] = {
    /* trans_act_to_cardemu_8821c */
    {0x0093, PWR_WRITE, 0x08, 0x00},
    {0x001F, PWR_WRITE, 0xFF, 0x00},
    {0x0049, PWR_WRITE, 0x02, 0x00},
    {0x0006, PWR_WRITE, 0x01, 0x01},
    {0x0002, PWR_WRITE, 0x02, 0x00},
    {0x0005, PWR_WRITE, 0x02, 0x02},
    {0x0005, PWR_POLL, 0x02, 0x00},
    {0x0020, PWR_WRITE, 0x08, 0x00},
    /* trans_cardemu_to_carddis_8821c */
    {0x0067, PWR_WRITE, 0x20, 0x00},
    {0x0005, PWR_WRITE, 0x04, 0x04},
    {0x0081, PWR_WRITE, 0xC0, 0x00},
    {0x0090, PWR_WRITE, 0x02, 0x00},
};
#define NPOWER_OFF ((int)(sizeof POWER_OFF / sizeof POWER_OFF[0]))

/* Waits up to a second for the byte to match. Linux, on a PCIe card, toggles a power-control bit (BIT_PFM_WOWL) and tries once more. */
static int poll8(u32 off, u8 mask, u8 value)
{
    for (int round = 0; round < 2; round++)
    {
        for (int i = 0; i < POLL_TRIES; i++)
        {
            if ((rd8(off) & mask) == (value & mask))
                return 1;
            udelay(50);
        }
        if (round == 0)
        {
            u8 v = rd8(R_SYS_PW_CTRL);
            wr8(R_SYS_PW_CTRL, (u8)(v | BIT(3)));
            wr8(R_SYS_PW_CTRL, (u8)(v & ~BIT(3)));
        }
    }
    return 0;
}

/* rtw_mac_pre_system_cfg: 0 */
static void pre_system_cfg(void)
{
    wr8(R_RSV_CTRL, 0);
    wr32(R_HCI_OPT_CTRL, rd32(R_HCI_OPT_CTRL) | BIT(8));                       /* BIT_USB_SUS_DIS */
    wr32(R_PAD_CTRL1, rd32(R_PAD_CTRL1) | BIT(29) | BIT(28));                   /* pin mux: PAPE / LNAON go to WLAN */
    wr32(R_LED_CFG, rd32(R_LED_CFG) & ~(BIT(26) | BIT(25)));
    wr32(R_GPIO_MUXCFG, rd32(R_GPIO_MUXCFG) | BIT(2));                          /* BIT_WLRFE_4_5_EN */
    wr8(R_SYS_FUNC_EN, (u8)(rd8(R_SYS_FUNC_EN) & ~(BIT(0) | BIT(1))));          /* baseband off */
    wr8(R_RF_CTRL, (u8)(rd8(R_RF_CTRL) & ~(BIT(2) | BIT(1) | BIT(0))));         /* RF off */
    wr32(R_WLRF1, rd32(R_WLRF1) & ~(BIT(24) | BIT(25) | BIT(26)));
}

/* Runs a table of steps. 0 = all done, else the number (1-based) of the step that did not finish. */
static int run_steps(const PwrCmd *t, int n)
{
    for (int i = 0; i < n; i++)
    {
        const PwrCmd *c = &t[i];
        if (c->cmd == PWR_WRITE)
        {
            u8 v = rd8(c->off);
            v = (u8)((v & ~c->mask) | (c->value & c->mask));
            wr8(c->off, v);
        }
        else if (!poll8(c->off, c->mask, c->value))
            return i + 1;
    }
    return 0;
}

/* rtw_mac_power_on: with the MAC already on, Linux powers it off first and then on again, so that the chip starts from a known state.
 * Returns 0 = powered on; else the step that failed, with W.pwr_phase telling which flow (PHASE_OFF or PHASE_ON). */
enum { PHASE_OFF = 1, PHASE_ON = 2 };
static int power_cycle(int already_on)
{
    pre_system_cfg();
    if (already_on)
    {
        W.pwr_phase = PHASE_OFF;
        int step = run_steps(POWER_OFF, NPOWER_OFF);
        if (step)
            return step;
        pre_system_cfg();
    }
    W.pwr_phase = PHASE_ON;
    return run_steps(POWER_ON, NPOWER_ON);
}

static void read_state(u16 *func_en, u16 *pw_ctrl, u16 *mcufw, u8 *cr)
{
    *func_en = rd16(R_SYS_FUNC_EN);
    *pw_ctrl = rd16(R_SYS_PW_CTRL);
    *mcufw = rd16(R_MCUFW_CTRL);
    *cr = rd8(R_CR);
}

/* ---- the firmware file (stage 3a: found and checked, not loaded yet) ----
 * The chip's own CPU runs a program that Realtek ships as a file: linux-firmware rtw88/rtw8821c_fw.bin. It is not part of this
 * repository (Realtek's licence); put a copy named RTW8821C.BIN in the top folder of a drive (the build does it for
 * firmware/RTW8821C.BIN, see CMakeLists.txt). Layout (rtw88/fw.h, mac.c check_firmware_size): a 64 byte header, then three parts,
 * the data memory (DMEM), the instruction memory (IMEM) and, if header byte 0x18 bit 4 says so, the extra memory (EMEM); each part is
 * followed by 8 bytes of checksum. The sizes are in the header, so the file size must equal 64 + sum of (part + 8). */
#define FW_NAME "RTW8821C.BIN"
#define FW_HDR_SIZE 64
#define FW_CHKSUM_SIZE 8
#define FW_SIGNATURE 0x8821
#define FW_PART_MAX 0x100000u /* a part over 1 MiB is nonsense: also keeps the sums from overflowing */

enum { FW_NOT_CHECKED, FW_OK, FW_MISSING, FW_BAD };

static struct
{
    int state;
    const char *why;                   /* FW_BAD: static text */
    unsigned version, subversion;      /* 24.11 */
    unsigned year, month, day;
    unsigned size, dmem, imem, emem;   /* bytes of the three parts (without their checksums) */
    unsigned dmem_addr, imem_addr, emem_addr; /* where in the chip's memory they go */
    int node;                          /* the file in the VFS */
} FW;

static unsigned le16(const u8 *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned le32(const u8 *p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }

static void fw_check(void)
{
    FW.state = FW_MISSING;
    FW.why = 0;
    int node = exec_find(FW_NAME);
    if (node < 0)
        return;
    const VfsNode *n = vfs_node(node);
    if (!n || n->is_dir)
        return;
    FW.state = FW_BAD;
    FW.size = n->size;
    FW.node = node;
    u8 h[FW_HDR_SIZE];
    if (n->size < FW_HDR_SIZE || vfs_read(node, 0, h, FW_HDR_SIZE) != FW_HDR_SIZE)
    {
        FW.why = "too short";
        return;
    }
    if (le16(h) != FW_SIGNATURE)
    {
        FW.why = "signature";
        return;
    }
    FW.version = le16(h + 4);
    FW.subversion = h[6];
    FW.month = h[0x10];
    FW.day = h[0x11];
    FW.year = le16(h + 0x14);
    FW.dmem = le32(h + 0x24);
    FW.dmem_addr = le32(h + 0x20);
    FW.imem_addr = le32(h + 0x3C);
    FW.emem_addr = le32(h + 0x38);
    FW.imem = le32(h + 0x30);
    FW.emem = (h[0x18] & 0x10) ? le32(h + 0x34) : 0;
    if (FW.dmem > FW_PART_MAX || FW.imem > FW_PART_MAX || FW.emem > FW_PART_MAX)
    {
        FW.why = "part sizes";
        return;
    }
    unsigned real = FW_HDR_SIZE + FW.dmem + FW_CHKSUM_SIZE + FW.imem + FW_CHKSUM_SIZE + (FW.emem ? FW.emem + FW_CHKSUM_SIZE : 0);
    if (real != n->size)
    {
        FW.why = "file size";
        return;
    }
    FW.state = FW_OK;
}

/* ---- stage 3b: DMA rings and the firmware download ----
 * The firmware cannot be written to the chip's memory through the registers; it goes the long way, as in Linux (mac.c
 * __rtw_download_firmware): the host sends each piece of at most 4 KiB, as if it were a packet, through the chip's "beacon queue" (a
 * DMA ring in RAM that the chip reads) into the chip's transmit buffer, and then the chip's own DMA engine (DDMA) copies it from there to
 * its instruction or data memory, adding up a checksum on the way. When all parts are in, the on-chip CPU is switched on and reports
 * "firmware ready" in MCUFW_CTRL.
 *
 * Every register, bit and number below comes from Linux rtw88 (reg.h, pci.h, mac.c, fw.c, pci.c, tx.c, rtw8821c.c). The chip addresses RAM
 * with 32 bits here, so everything it reads must be below 4 GiB. */
#ifndef RTL8821CE_FW_LOAD
#define RTL8821CE_FW_LOAD 1 /* -DRTL8821CE_FW_LOAD=0: stop after the power-on */
#endif
#ifndef RTL8821CE_OFF_AFTER
#define RTL8821CE_OFF_AFTER 1 /* power the chip off again after the firmware test, so that a reboot finds it as Linux would */
#endif

#define R_SYS_CLK_CTRL 0x0008
#define R_TXDMA_PQ_MAP 0x010C
#define R_FIFOPAGE_CTRL_2 0x0204
#define R_TXDMA_STATUS 0x0210
#define R_RQPN_CTRL_2 0x022C
#define R_FIFOPAGE_INFO_1 0x0230
#define R_FWHW_TXQ_CTRL 0x0420
#define R_BCN_CTRL 0x0550
#define R_CPU_DMEM_CON 0x1080
#define R_FW_DBG7 0x10FC
#define R_CR_EXT 0x1100
#define R_DDMA_CH0SA 0x1200
#define R_DDMA_CH0DA 0x1204
#define R_DDMA_CH0CTRL 0x1208
#define R_H2CQ_CSR 0x1330

#define OCP_TXBUF 0x18780000u /* where the chip's DMA engine sees its transmit buffer */
#define OCP_DMEM 0x00200000u  /* addresses below this are instruction memory (IMEM / EMEM), from here on data memory */
#define TX_DESC_SIZE 48
#define CHUNK_MAX 0x1000u
#define SYS_FUNC_EN_8821C 0xD8 /* chip->sys_func_en: blocks switched on in the high byte of SYS_FUNC_EN */

/* the PCIe interface: DMA rings (pci.h). Per transmit queue: the register with the number of entries and the one with the ring's address. */
#define R_PCI_CTRL 0x0300
#define R_TXBD_RWPTR_CLR 0x039C
#define R_TXBD_BCN_WORK 0x0383
#define R_RXBD_NUM_MPDUQ 0x0382
#define R_RXBD_DESA_MPDUQ 0x0338
#define TXBD_OWN_BIT 15

typedef struct
{
    u16 num_reg, desa_reg;
    u16 len; /* entries in the ring */
} TxQueue;

enum { Q_BK, Q_BE, Q_VI, Q_VO, Q_BCN, Q_MGMT, Q_HI0, Q_H2C, Q_COUNT };

static const TxQueue QUEUES[Q_COUNT] = {
    [Q_BK] = {0x038A, 0x0330, 128},   [Q_BE] = {0x0388, 0x0328, 256},   [Q_VI] = {0x0386, 0x0320, 128},
    [Q_VO] = {0x0384, 0x0318, 128},   [Q_BCN] = {0, 0x0308, 1},         [Q_MGMT] = {0x0380, 0x0310, 128},
    [Q_HI0] = {0x038C, 0x0340, 128},  [Q_H2C] = {0x1328, 0x1320, 128},
};

#define RX_COUNT 32            /* Linux uses 512 buffers; the firmware download does not receive anything */
#define RX_BUF_SIZE (11454 + 24)
#define RX_BUF_SLOT 12288      /* a buffer and its slack, a multiple of 4096 */

/* DMA memory for the rings: one block, 4 KiB aligned pieces. */
#define PAGE 4096u
#define DMA_PAGES (Q_COUNT + 1 + RX_COUNT * (RX_BUF_SLOT / PAGE) + 2)

static struct
{
    int ready;
    u8 *mem;
    u32 dma;                /* the address the chip uses for mem */
    u8 *stage;              /* a transmit descriptor and up to CHUNK_MAX bytes of firmware */
    u32 stage_dma;
} D;

#ifdef HARIS_HOST
/* the host test hands out memory and the 32-bit address the pretend chip uses for it */
void *rtl8821ce_host_dma_alloc(unsigned pages, u32 *dma);
static void *dma_alloc(unsigned pages, u32 *dma) { return rtl8821ce_host_dma_alloc(pages, dma); }
#else
static void *dma_alloc(unsigned pages, u32 *dma)
{
    u64 p = pmm_alloc_contiguous_tag(pages, PMM_TAG_OTHER);
    if (!p)
        return 0;
    if (p + (u64)pages * PMM_PAGE_SIZE > 0x100000000ULL) /* the chip has 32-bit addresses here */
    {
        pmm_free_contiguous(p, pages);
        return 0;
    }
    volatile u8 *b = (volatile u8 *)p;
    for (u64 i = 0; i < (u64)pages * PMM_PAGE_SIZE; i++)
        b[i] = 0;
    *dma = (u32)p;
    return (void *)p;
}
#endif

static inline void fence(void) { __asm__ volatile("mfence" ::: "memory"); }
static void put16(u8 *p, unsigned v)
{
    volatile u8 *q = p;
    q[0] = (u8)v;
    q[1] = (u8)(v >> 8);
}
static void put32(u8 *p, u32 v)
{
    volatile u8 *q = p;
    q[0] = (u8)v;
    q[1] = (u8)(v >> 8);
    q[2] = (u8)(v >> 16);
    q[3] = (u8)(v >> 24);
}
static void zero_bytes(u8 *p, unsigned n)
{
    volatile u8 *q = p;
    for (unsigned i = 0; i < n; i++)
        q[i] = 0;
}

static void wr16(u32 off, u16 v)
{
    *(volatile u16 *)(W.regs + off) = v;
    WRITE_LOG(off, 2, v);
}
static void set8(u32 off, u8 bits) { wr8(off, (u8)(rd8(off) | bits)); }
static void clr8(u32 off, u8 bits) { wr8(off, (u8)(rd8(off) & ~bits)); }

/* Linux check_hw_ready: up to 10 ms for (register & mask) == value. */
static int ready32(u32 off, u32 mask, u32 value, int tries)
{
    for (int i = 0; i < tries; i++)
    {
        if ((rd32(off) & mask) == value)
            return 1;
        udelay(10);
    }
    return 0;
}

static int dma_setup(void)
{
    if (D.ready)
        return 1;
    D.mem = dma_alloc(DMA_PAGES, &D.dma);
    if (!D.mem)
        return 0;
    u8 *rx_ring = D.mem + Q_COUNT * PAGE;
    u8 *rx_bufs = rx_ring + PAGE;
    for (unsigned i = 0; i < RX_COUNT; i++) /* receive buffer descriptors: size, packet size (0), address */
    {
        u8 *bd = rx_ring + i * 8;
        put16(bd, RX_BUF_SIZE);
        put16(bd + 2, 0);
        put32(bd + 4, D.dma + (u32)(rx_bufs - D.mem) + i * RX_BUF_SLOT);
    }
    D.stage = rx_bufs + RX_COUNT * RX_BUF_SLOT;
    D.stage_dma = D.dma + (u32)(D.stage - D.mem);
    D.ready = 1;
    return 1;
}

/* rtw_pci_setup: tell the chip where the rings are and reset its read / write pointers (rtw_pci_reset_buf_desc, rtw_pci_dma_reset). */
static void hci_setup(void)
{
    wr8(R_PCI_CTRL + 3, (u8)(rd8(R_PCI_CTRL + 3) | 0xF7));
    wr32(QUEUES[Q_BCN].desa_reg, D.dma + Q_BCN * PAGE);
    static const int ORDER[] = {Q_H2C, Q_BK, Q_BE, Q_VO, Q_VI, Q_MGMT, Q_HI0}; /* the order Linux writes them in */
    for (unsigned i = 0; i < sizeof ORDER / sizeof ORDER[0]; i++)
    {
        const TxQueue *q = &QUEUES[ORDER[i]];
        wr16(q->num_reg, (u16)(q->len & 0xFFF));
        wr32(q->desa_reg, D.dma + (u32)ORDER[i] * PAGE);
    }
    wr16(R_RXBD_NUM_MPDUQ, RX_COUNT & 0xFFF);
    wr32(R_RXBD_DESA_MPDUQ, D.dma + Q_COUNT * PAGE);
    wr32(R_TXBD_RWPTR_CLR, 0xFFFFFFFFu);
    wr32(R_H2CQ_CSR, rd32(R_H2CQ_CSR) | BIT(16) | BIT(8)); /* BIT_CLR_H2CQ_HOST_IDX | BIT_CLR_H2CQ_HW_IDX */
    wr32(R_PCI_CTRL, rd32(R_PCI_CTRL) | BIT(20) | BIT(15)); /* BIT_RST_TRXDMA_INTF | BIT_RX_TAG_EN */
}

/* __rtw_mac_init_system_cfg (the chip with the 3081 CPU): the last part of rtw_mac_power_on */
static void init_system_cfg(void)
{
    wr32(R_CPU_DMEM_CON, rd32(R_CPU_DMEM_CON) | BIT(16) | BIT(8)); /* BIT_WL_PLATFORM_RST | BIT_DDMA_EN */
    set8(R_SYS_FUNC_EN + 1, SYS_FUNC_EN_8821C);
    wr8(R_CR_EXT + 3, (u8)((rd8(R_CR_EXT + 3) & 0xF0) | 0x0C));
    u32 tmp = rd32(R_MCUFW_CTRL); /* no boot from flash: the driver loads the firmware */
    if (tmp & BIT(20))
    {
        wr32(R_MCUFW_CTRL, tmp & ~BIT(20));
        wr32(R_GPIO_MUXCFG, rd32(R_GPIO_MUXCFG) & ~BIT(19));
    }
}

/* the outcome of the firmware download, for the Device Manager line */
enum { DL_NOT_TRIED, DL_OK, DL_FAILED, DL_SKIPPED };
static struct
{
    int state;
    const char *why;     /* DL_FAILED, DL_SKIPPED: static text */
    int part;            /* 0 DMEM, 1 IMEM, 2 EMEM */
    int piece;           /* the failure happened while a piece of that part was being sent (DL.part, DL.off say which) */
    unsigned off;        /* byte offset of the piece in its part */
    u16 mcufw;           /* MCUFW_CTRL at the end */
    int off_after;       /* 1 = powered off again; -1 = the power-off failed */
} DL;

/* Sends one piece, already in D.stage + TX_DESC_SIZE, to the transmit buffer at page 0 through the beacon queue
 * (rtw_fw_write_data_rsvd_page, rtw_pci_write_data_rsvd_page, rtw_tx_fill_tx_desc). 0 = done. */
static int send_piece(unsigned size)
{
    u8 bcn_ctrl = rd8(R_BCN_CTRL);
    wr16(R_FIFOPAGE_CTRL_2, (u16)(0 | BIT(15)));       /* the "beacon" starts at page 0; BIT_BCN_VALID_V1 is cleared by writing 1 */
    u8 cr1 = rd8(R_CR + 1);
    wr8(R_CR + 1, (u8)(cr1 | 1));                      /* BIT_ENSWBCN >> 8 */
    wr8(R_BCN_CTRL, (u8)((bcn_ctrl & ~BIT(3)) | BIT(4))); /* beacon function off, no TSF update */
    u8 txq2 = rd8(R_FWHW_TXQ_CTRL + 2);
    wr8(R_FWHW_TXQ_CTRL + 2, (u8)(txq2 & ~BIT(6)));    /* BIT_EN_BCNQ_DL >> 16 */

    /* the transmit descriptor (48 bytes, 10 words used) */
    u8 *d = D.stage;
    zero_bytes(d, TX_DESC_SIZE);
    unsigned bmc = size > 4 ? (D.stage[TX_DESC_SIZE + 4] & 1u) : 0; /* Linux reads the "destination address" of the piece: a multicast bit */
    put32(d + 0, size | (TX_DESC_SIZE << 16) | (bmc << 24) | BIT(26) | BIT(31)); /* size, offset, bmc, last segment, no queue/sequence select */
    put32(d + 4, (16u << 8) | (7u << 16));              /* queue select 16 = beacon, rate id 7 (RTW_RATEID_G) */
    put32(d + 12, BIT(8) | BIT(10));                    /* use the rate in the descriptor, no rate fallback */
    put32(d + 16, 4);                                   /* DESC_RATE6M */
    put32(d + 32, BIT(15));                             /* hardware sequence number */

    /* the buffer descriptor of the beacon queue: entry 0 = the transmit descriptor, entry 1 = the data; OWN bit in the length field */
    u8 *bd = D.mem + Q_BCN * PAGE;
    zero_bytes(bd, 16);
    unsigned psb = ((TX_DESC_SIZE + size - 1) / 128 + 1) | BIT(TXBD_OWN_BIT);
    put16(bd + 0, TX_DESC_SIZE);
    put16(bd + 2, psb);
    put32(bd + 4, D.stage_dma);
    put16(bd + 8, size);
    put16(bd + 10, 0);
    put32(bd + 12, D.stage_dma + TX_DESC_SIZE);
    fence();
    set8(R_TXBD_BCN_WORK, BIT(4));                     /* BIT_PCI_BCNQ_FLAG: go */

    int ok = ready32(R_FIFOPAGE_CTRL_2, BIT(15), BIT(15), 1000); /* the chip says the "beacon" is in its buffer */
    wr16(R_FIFOPAGE_CTRL_2, (u16)(0 | BIT(15)));       /* rsvd_boundary is still 0 at this point of Linux' start-up */
    wr8(R_BCN_CTRL, bcn_ctrl);
    wr8(R_FWHW_TXQ_CTRL + 2, txq2);
    wr8(R_CR + 1, cr1);
    return ok ? 0 : -1;
}

/* iddma_download_firmware: copy `len` bytes from the transmit buffer (page 0, behind the descriptor) to `dst` in the chip's memory. */
static int ddma_piece(u32 dst, unsigned len, int first)
{
    if (!ready32(R_DDMA_CH0CTRL, BIT(31), 0, 1000))
        return -1;
    u32 ctrl = BIT(29) | BIT(31) | (len & 0x3FFFFu) | (first ? 0 : BIT(24)); /* checksum on, start, [continue the checksum] */
    wr32(R_DDMA_CH0SA, OCP_TXBUF + TX_DESC_SIZE);
    wr32(R_DDMA_CH0DA, dst);
    wr32(R_DDMA_CH0CTRL, ctrl);
    return ready32(R_DDMA_CH0CTRL, BIT(31), 0, 1000) ? 0 : -1;
}

/* check_fw_checksum: the DDMA engine adds up what it copied; the result goes into MCUFW_CTRL as "downloaded" / "checksum ok" bits. */
static int check_checksum(u32 dst)
{
    u8 fw_ctrl = rd8(R_MCUFW_CTRL);
    int imem = dst < OCP_DMEM;
    if (rd32(R_DDMA_CH0CTRL) & BIT(27)) /* BIT_DDMACH0_CHKSUM_STS: a mismatch */
    {
        fw_ctrl = (u8)(fw_ctrl | (imem ? BIT(3) : BIT(5)));
        fw_ctrl = (u8)(fw_ctrl & ~(imem ? BIT(4) : BIT(6)));
        wr8(R_MCUFW_CTRL, fw_ctrl);
        return 0;
    }
    fw_ctrl = (u8)(fw_ctrl | (imem ? (BIT(3) | BIT(4)) : (BIT(5) | BIT(6))));
    wr8(R_MCUFW_CTRL, fw_ctrl);
    return 1;
}

/* download_firmware_to_mem: one part of the file, in pieces */
static int download_part(int part, unsigned file_off, unsigned size, u32 dst)
{
    DL.part = part;
    DL.piece = 1;
    wr32(R_DDMA_CH0CTRL, rd32(R_DDMA_CH0CTRL) | BIT(25)); /* reset the checksum status */
    int first = 1;
    for (unsigned off = 0; off < size;)
    {
        unsigned n = size - off < CHUNK_MAX ? size - off : CHUNK_MAX;
        DL.off = off;
        if (vfs_read(FW.node, file_off + off, D.stage + TX_DESC_SIZE, n) != (int)n)
        {
            DL.why = "file read";
            return -1;
        }
        if (send_piece(n))
        {
            DL.why = "send timeout";
            return -1;
        }
        if (ddma_piece(dst + off, n, first))
        {
            DL.why = "ddma timeout";
            return -1;
        }
        first = 0;
        off += n;
    }
    if (!check_checksum(dst))
    {
        DL.why = "checksum";
        return -1;
    }
    return 0;
}

typedef struct
{
    unsigned len, reg;
    u32 val;
} Backup;

static void backup_restore(const Backup *b, int n)
{
    for (int i = 0; i < n; i++)
    {
        if (b[i].len == 1)
            wr8(b[i].reg, (u8)b[i].val);
        else if (b[i].len == 2)
            wr16(b[i].reg, (u16)b[i].val);
        else
            wr32(b[i].reg, b[i].val);
    }
}

static void wlan_cpu_enable(int enable)
{
    if (enable)
    {
        set8(R_RSV_CTRL + 1, BIT(0));       /* the CPU's I/O interface */
        set8(R_SYS_FUNC_EN + 1, BIT(2));    /* the CPU itself (BIT_FEN_CPUEN) */
    }
    else
    {
        clr8(R_SYS_FUNC_EN + 1, BIT(2));
        clr8(R_RSV_CTRL + 1, BIT(0));
    }
}

/* __rtw_download_firmware. 0 = the firmware runs; else DL.why says what went wrong. (The Bluetooth coexistence register that Linux saves
 * and restores around this is left alone.) */
static int fw_download(void)
{
    Backup bk[6];
    wlan_cpu_enable(0);

    /* download_firmware_reg_backup: use only the high priority queue, give it pages, switch the beacon functions off */
    bk[0] = (Backup){1, R_TXDMA_PQ_MAP + 1, rd8(R_TXDMA_PQ_MAP + 1)};
    wr8(R_TXDMA_PQ_MAP + 1, 3u << 6);                 /* RTW_DMA_MAPPING_HIGH << 6 */
    bk[1] = (Backup){1, R_CR, rd8(R_CR)};
    bk[2] = (Backup){4, R_H2CQ_CSR, BIT(31)};          /* BIT_H2CQ_FULL (not read: this is the value to leave behind) */
    wr8(R_CR, BIT(0) | BIT(2));                        /* BIT_HCI_TXDMA_EN | BIT_TXDMA_EN */
    wr32(R_H2CQ_CSR, BIT(31));
    bk[3] = (Backup){2, R_FIFOPAGE_INFO_1, rd16(R_FIFOPAGE_INFO_1)};
    bk[4] = (Backup){4, R_RQPN_CTRL_2, rd32(R_RQPN_CTRL_2) | BIT(31)}; /* BIT_LD_RQPN */
    wr16(R_FIFOPAGE_INFO_1, 0x200);
    wr32(R_RQPN_CTRL_2, bk[4].val);
    u8 bcn = rd8(R_BCN_CTRL);
    bk[5] = (Backup){1, R_BCN_CTRL, bcn};
    wr8(R_BCN_CTRL, (u8)((bcn & ~BIT(3)) | BIT(4)));

    /* download_firmware_reset_platform */
    clr8(R_CPU_DMEM_CON + 2, BIT(0));
    clr8(R_SYS_CLK_CTRL + 1, BIT(6));
    set8(R_CPU_DMEM_CON + 2, BIT(0));
    set8(R_SYS_CLK_CTRL + 1, BIT(6));

    /* start_download_firmware: DMEM, IMEM, then EMEM when there is one */
    wr16(R_MCUFW_CTRL, (u16)((rd16(R_MCUFW_CTRL) & 0x3800) | BIT(0))); /* BIT_MCUFWDL_EN */
    unsigned dmem = FW.dmem + FW_CHKSUM_SIZE, imem = FW.imem + FW_CHKSUM_SIZE, emem = FW.emem ? FW.emem + FW_CHKSUM_SIZE : 0;
    int bad = download_part(0, FW_HDR_SIZE, dmem, FW.dmem_addr & ~BIT(31));
    if (!bad)
        bad = download_part(1, FW_HDR_SIZE + dmem, imem, FW.imem_addr & ~BIT(31));
    if (!bad && emem)
        bad = download_part(2, FW_HDR_SIZE + dmem + imem, emem, FW.emem_addr & ~BIT(31));
    if (bad)
    {
        clr8(R_MCUFW_CTRL, BIT(0));
        set8(R_SYS_FUNC_EN + 1, BIT(2));
        backup_restore(bk, 6);
        return -1;
    }
    backup_restore(bk, 6);

    /* download_firmware_end_flow */
    wr32(R_TXDMA_STATUS, BIT(2));                      /* BTI_PAGE_OVF */
    u16 ctrl = rd16(R_MCUFW_CTRL);
    if ((ctrl & (BIT(4) | BIT(6))) == (BIT(4) | BIT(6)))
        wr16(R_MCUFW_CTRL, (u16)((ctrl | BIT(14)) & ~BIT(0))); /* BIT_FW_DW_RDY, download finished */
    wlan_cpu_enable(1);

    /* download_firmware_validate: the CPU runs the firmware, which says so (FW_READY) */
    DL.piece = 0;
    if (!ready32(R_MCUFW_CTRL, 0xCFFF, 0xC078, 3000))
    {
        DL.why = "firmware not ready";
        clr8(R_MCUFW_CTRL, BIT(0));
        set8(R_SYS_FUNC_EN + 1, BIT(2));
        return -1;
    }
    hci_setup(); /* reset the descriptors and indexes */
    return 0;
}

static int rtl8821ce_claims(const PciDevice *pd)
{
    return pd->vendor_id == 0x10EC && (pd->device_id == 0xB821 || pd->device_id == 0xC821);
}

static int rtl8821ce_probe(const PciDevice *pd)
{
    W.regs = 0;
    W.ok = 0;
    W.fail = 0;
    W.cfg1 = W.cfg2 = 0;
    W.func_en = W.pw_ctrl = W.mcufw = 0;
    W.cr = 0;
    W.pwr_state = PWR_NOT_TRIED;
    W.pwr_step = 0;
    W.pwr_phase = 0;
    W.was_on = 0;
    W.func_en2 = W.pw_ctrl2 = W.mcufw2 = 0;
    W.cr2 = 0;
    FW.state = FW_NOT_CHECKED;
    u32 cmd = pci_cfg_read(pd->bus, pd->dev, pd->fn, 4);
    pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (cmd & 0xFFFFu) | 0x2u); /* memory space on (the firmware usually did it already) */
    u32 lo = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10 + 4 * PCI_BAR_MMIO);
    if (lo & 1u)
    {
        W.fail = "BAR 2 is not memory";
        return 0;
    }
    u64 base = lo & ~0xFULL;
    if (((lo >> 1) & 3u) == 2u)
        base |= (u64)pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x14 + 4 * PCI_BAR_MMIO) << 32;
    if (!base)
    {
        W.fail = "BAR 2 is empty";
        return 0;
    }
    W.regs = (volatile u8 *)base;
    W.cfg1 = rd32(R_SYS_CFG1);
    W.cfg2 = rd32(R_SYS_CFG2);
    read_state(&W.func_en, &W.pw_ctrl, &W.mcufw, &W.cr);
    if (W.cfg1 == 0xFFFFFFFFu && W.cfg2 == 0xFFFFFFFFu)
    {
        W.fail = "registers read as all ones (card off?)";
        return 0;
    }
    W.ok = 1;
    fw_check(); /* (only looks at a file; the chip is not involved) */

    /* stage 3 needs the chip to reach RAM: bus mastering on, and the DMA rings set up (Linux does this before the power-on, too) */
    int dma_ok = 0;
    DL.state = DL_NOT_TRIED;
    DL.why = 0;
    DL.part = 0;
    DL.piece = 0;
    DL.off = 0;
    DL.mcufw = 0;
    DL.off_after = 0;
    int want_load = RTL8821CE_POWER_ON && RTL8821CE_FW_LOAD && FW.state == FW_OK;
    if (want_load)
    {
        pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (pci_cfg_read(pd->bus, pd->dev, pd->fn, 4) & 0xFFFFu) | 0x6u); /* memory space + bus master */
        dma_ok = dma_setup();
        if (dma_ok)
            hci_setup();
        else
        {
            DL.state = DL_SKIPPED;
            DL.why = "no DMA memory below 4 GiB";
        }
    }

    /* stage 2: power on, the way Linux does. A MAC that is already on is powered off and on again, unless a firmware of an earlier
     * user is still in the chip (0xC078): Linux first tells that firmware to stop through the RPWM register, which is not done here, so
     * such a card is left alone. */
    if (!RTL8821CE_POWER_ON)
        W.pwr_state = PWR_DISABLED;
    else if (W.cr != 0xEA && W.mcufw == 0xC078)
        W.pwr_state = PWR_LEFT_ALONE;
    else
    {
        W.was_on = W.cr != 0xEA;
        W.pwr_step = power_cycle(W.was_on);
        W.pwr_state = W.pwr_step ? PWR_FAILED : PWR_OK;
        if (!W.pwr_step)
            init_system_cfg(); /* the rest of rtw_mac_power_on */
        read_state(&W.func_en2, &W.pw_ctrl2, &W.mcufw2, &W.cr2);

        /* stage 3b: the firmware. A failure leaves the chip powered off again, as Linux does (err_off). */
        if (!W.pwr_step && dma_ok)
        {
            if (fw_download() == 0)
            {
                DL.state = DL_OK;
                DL.mcufw = rd16(R_MCUFW_CTRL);
            }
            else
            {
                DL.state = DL_FAILED;
                DL.mcufw = rd16(R_MCUFW_CTRL);
            }
            if (RTL8821CE_OFF_AFTER || DL.state == DL_FAILED)
            {
                W.pwr_phase = PHASE_OFF;
                DL.off_after = run_steps(POWER_OFF, NPOWER_OFF) == 0 ? 1 : -1;
            }
        }
    }
    return 0; /* never a usable network adapter yet */
}

/* ", fw 24.11 ok" / ", no RTW8821C.BIN" / ", bad RTW8821C.BIN (why)" */
static void status_fw(NicText *t)
{
    switch (FW.state)
    {
    case FW_OK:
        nic_puts(t, ", fw ");
        nic_putd(t, FW.version);
        nic_putc(t, '.');
        nic_putd(t, FW.subversion);
        nic_puts(t, " ok");
        break;
    case FW_MISSING:
        nic_puts(t, ", no " FW_NAME);
        break;
    case FW_BAD:
        nic_puts(t, ", bad fw: ");
        nic_puts(t, FW.why ? FW.why : "?");
        break;
    default:
        break;
    }
}

/* the outcome of stage 3b, appended to the line: ", fw 24.11 loaded and ready (80=c078), off again" / ", load FAILED: why in dmem+4096 (80=0001)" */
static void status_download(NicText *t)
{
    static const char *const PART[] = {"dmem", "imem", "emem"};
    if (DL.state == DL_OK)
    {
        nic_puts(t, ", fw ");
        nic_putd(t, FW.version);
        nic_putc(t, '.');
        nic_putd(t, FW.subversion);
        nic_puts(t, " loaded and ready (80=");
        nic_puth(t, DL.mcufw, 4);
        nic_putc(t, ')');
        nic_puts(t, DL.off_after > 0 ? ", off again" : DL.off_after < 0 ? ", power-off FAILED" : "");
    }
    else if (DL.state == DL_FAILED)
    {
        nic_puts(t, ", load FAILED: ");
        nic_puts(t, DL.why ? DL.why : "?");
        if (DL.piece)
        {
            nic_puts(t, " in ");
            nic_puts(t, PART[DL.part < 3 ? DL.part : 0]);
            nic_putc(t, '+');
            nic_putd(t, DL.off);
        }
        nic_puts(t, " (80=");
        nic_puth(t, DL.mcufw, 4);
        nic_putc(t, ')');
    }
    else if (DL.state == DL_SKIPPED)
    {
        status_fw(t);
        nic_puts(t, ", not loaded: ");
        nic_puts(t, DL.why ? DL.why : "?");
    }
}

static void rtl8821ce_status(char *buf, unsigned cap)
{
    NicText t;
    nic_text_init(&t, buf, cap);
    nic_puts(&t, "RTL8821CE (no driver): ");
    if (!W.ok)
    {
        nic_puts(&t, W.fail ? W.fail : "not probed");
        return;
    }
    nic_puts(&t, "cut ");
    unsigned cut = (W.cfg1 >> 12) & 0xF;
    nic_putc(&t, cut < 7 ? (char)('A' + cut) : '?');
    switch (W.pwr_state)
    {
    case PWR_OK:
    case PWR_FAILED:
        if (DL.state == DL_OK || DL.state == DL_FAILED || DL.state == DL_SKIPPED)
        {
            nic_puts(&t, ", powered on");
            status_download(&t);
            break;
        }
        nic_puts(&t, W.was_on ? ", was on, " : ", was off, ");
        if (W.pwr_state == PWR_OK)
            nic_puts(&t, "powered on");
        else
        {
            nic_puts(&t, W.pwr_phase == PHASE_OFF ? "power-off FAILED at step " : "power-on FAILED at step ");
            nic_putd(&t, (unsigned)W.pwr_step);
        }
        nic_puts(&t, " (CR=");
        nic_puth(&t, W.cr2, 2);
        nic_puts(&t, " 02=");
        nic_puth(&t, W.func_en2, 4);
        nic_puts(&t, " 04=");
        nic_puth(&t, W.pw_ctrl2, 4);
        nic_putc(&t, ')');
        if (DL.state == DL_SKIPPED)
            status_download(&t);
        else
            status_fw(&t);
        break;
    default: /* only read: show the registers as they were */
        nic_puts(&t, " CR=");
        nic_puth(&t, W.cr, 2);
        nic_puts(&t, " 80=");
        nic_puth(&t, W.mcufw, 4);
        nic_puts(&t, " 02=");
        nic_puth(&t, W.func_en, 4);
        nic_puts(&t, " 04=");
        nic_puth(&t, W.pw_ctrl, 4);
        nic_puts(&t, W.pwr_state == PWR_LEFT_ALONE ? " (firmware resident: left alone)" : W.pwr_state == PWR_DISABLED ? " (read only build)" : "");
    }
}

static const unsigned char NO_MAC[6];
static const unsigned char *rtl8821ce_mac(void) { return NO_MAC; }
static int rtl8821ce_link_up(void) { return 0; }
static int rtl8821ce_send(const void *frame, unsigned len) { (void)frame; (void)len; return -1; }
static int rtl8821ce_recv(void *buf, unsigned max) { (void)buf; (void)max; return 0; }

const NicDriver NIC_RTL8821CE = {
    "Realtek RTL8821CE (Wi-Fi)", rtl8821ce_claims, rtl8821ce_probe, rtl8821ce_mac, rtl8821ce_link_up, rtl8821ce_send,
    rtl8821ce_recv, rtl8821ce_status, 0, 0, 0, 0,
};
