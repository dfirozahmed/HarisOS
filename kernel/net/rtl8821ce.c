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
 *   4a  read the board data (MAC address, board type, crystal setting) from the efuse. Seen to work on a real card.
 *   4b-1 rtw_mac_init (queues, MAC timing, receive filter). Seen to work on a real card.
 *   4b-2 the baseband, AGC and RF tables (include/rtl8821ce_phytab.h), then power off again.
 *   4b-3 firmware commands (H2C) and events (C2H); 4c receive and scan;
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
#include "rtl8821ce_phytab.h"
#include "timer.h"
#include "vfs.h"
#include "wpa.h"

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
/* the host test moves the clock by hand (and with every delay) to play minutes of silence in a moment */
unsigned long long rtl8821ce_host_ms(void);
static u64 now_ms(void) { return rtl8821ce_host_ms(); }
static int clock_ok(void) { return 1; }
#else
static u64 now_ms(void) { return timer_ms(); }
static int clock_ok(void) { return timer_ready(); }
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

/* ---- stage 4a: the card's own data in its efuse ----
 * Every chip carries a small one-time-programmable memory: 512 bytes, "physical map". It holds the data of the board in blocks, each with
 * a header (rtw88 efuse.c): the 1-byte header is block[3:0] in bits 7..4 and word_en[3:0] in bits 3..0; when the low 5 bits of the first
 * byte are 0xF it is the 2-byte form, block[2:0] in bits 7..5 of the first byte, block[6:3] in bits 7..4 of the second, word_en in the low 4
 * bits of the second. word_en has a 0 for each of the four 16-bit words of the 8-byte block that follows in the map. Blocks fill a 512 byte
 * "logical map"; the header 0xFF ends the list. In the logical map the card's MAC address is at offset 0xD0 (struct rtw8821c_efuse). */
#define R_EFUSE_CTRL 0x0030
#define R_LDO_EFUSE_CTRL 0x0034
#define EFUSE_PHYSICAL 512
#define EFUSE_PROTECT 96   /* the last bytes of the physical map are protected: no blocks there */
#define EFUSE_LOGICAL 512
#define EFUSE_MAC_AT 0xD0

enum { EF_NOT_TRIED, EF_OK, EF_FAILED };
static struct
{
    int state;
    const char *why;
    unsigned char mac[6];
    int map_ok;                 /* the logical map was decoded: the three values below are real */
    unsigned char rfe, pkg, xtal; /* rfe_option (0xCA, low 5 bits), package type (0xCA bit 5), crystal calibration (0xB9) */
    unsigned char swing2g, swing5g; /* tx_bb_swing_setting_2g / _5g (0xC6, 0xC7) */
    unsigned char txpwr[42];        /* the power calibration of RF path A (logical map 0x10 .. 0x39) */
} EF;

/* rtw_dump_logical_efuse_map. 0 = ok, -1 = the blocks do not fit (corrupt map). `log` must be filled with 0xFF by the caller. */
static int efuse_logical(const u8 *phy, u8 *log)
{
    const unsigned limit = EFUSE_PHYSICAL - EFUSE_PROTECT;
    unsigned idx = 0;
    while (idx < limit)
    {
        u8 hdr1 = phy[idx], hdr2 = phy[idx + 1];
        if (hdr1 == 0xFF || ((hdr1 & 0x1F) == 0xF && hdr2 == 0xFF)) /* end of the list */
            break;
        unsigned blk, word_en;
        if ((hdr1 & 0x1F) == 0xF) /* 2 byte header */
        {
            blk = (((unsigned)hdr2 & 0xF0) >> 1) | (((unsigned)hdr1 >> 5) & 7);
            word_en = hdr2 & 0xF;
            idx += 2;
        }
        else
        {
            blk = ((unsigned)hdr1 & 0xF0) >> 4;
            word_en = hdr1 & 0xF;
            idx += 1;
        }
        for (int i = 0; i < 4; i++)
        {
            if (word_en & BIT(i)) /* this word is not written */
                continue;
            unsigned at = (blk << 3) + ((unsigned)i << 1);
            if (idx + 1 > limit || at + 1 > EFUSE_LOGICAL)
                return -1;
            log[at] = phy[idx];
            log[at + 1] = phy[idx + 1];
            idx += 2;
        }
    }
    return 0;
}

/* rtw_dump_physical_efuse_map: read the 512 bytes one by one through EFUSE_CTRL. The chip must be powered on. */
static int efuse_physical(u8 *map)
{
    wr32(R_LDO_EFUSE_CTRL, rd32(R_LDO_EFUSE_CTRL) & ~(BIT(8) | BIT(9))); /* bank 0: the WLAN part */
    wr8(R_LDO_EFUSE_CTRL + 3, (u8)(rd8(R_LDO_EFUSE_CTRL + 3) & ~BIT(7))); /* the 2.5 V supply for writing is off while reading */
    u32 ctl = rd32(R_EFUSE_CTRL);
    for (unsigned addr = 0; addr < EFUSE_PHYSICAL; addr++)
    {
        ctl &= ~(0xFFu | (0x3FFu << 8));
        ctl |= (addr & 0x3FF) << 8;
        wr32(R_EFUSE_CTRL, ctl & ~BIT(31)); /* ask for this address: BIT_EF_FLAG clear, the chip sets it when the data is there */
        int got = 0;
        for (int i = 0; i < 1000000 && !got; i++)
        {
            udelay(1);
            ctl = rd32(R_EFUSE_CTRL);
            got = (ctl & BIT(31)) != 0;
        }
        if (!got)
            return -1;
        map[addr] = (u8)(ctl & 0xFF);
    }
    return 0;
}

static int valid_mac(const u8 *m)
{
    int zero = 1;
    for (int i = 0; i < 6; i++)
        zero &= m[i] == 0;
    return !zero && !(m[0] & 1);
}

static void efuse_read_mac(void)
{
    u8 phy[EFUSE_PHYSICAL], log[EFUSE_LOGICAL];
    EF.map_ok = 0;
    EF.state = EF_FAILED;
    EF.why = 0;
    if (efuse_physical(phy))
    {
        EF.why = "read timeout";
        return;
    }
    volatile u8 *l = log;
    for (int i = 0; i < EFUSE_LOGICAL; i++)
        l[i] = 0xFF;
    if (efuse_logical(phy, log))
    {
        EF.why = "corrupt map";
        return;
    }
    EF.map_ok = 1;
    EF.rfe = log[0xCA] & 0x1F;
    EF.pkg = (log[0xCA] >> 5) & 1;
    EF.xtal = log[0xB9];
    EF.swing2g = log[0xC6];
    EF.swing5g = log[0xC7];
    for (int i = 0; i < 42; i++)
        EF.txpwr[i] = log[0x10 + i];
    for (int i = 0; i < 6; i++)
        EF.mac[i] = log[EFUSE_MAC_AT + i];
    if (!valid_mac(EF.mac))
    {
        EF.why = "no valid MAC";
        return;
    }
    EF.state = EF_OK;
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

/* ---- stage 4b-1: rtw_mac_init, the MAC's queues and protocol timing (Linux rtw_mac.c + rtw8821c_mac_init) ----
 * What it does, in short: the chip's 64 KiB TX packet memory is cut into 128-byte pages; the top pages are reserved for the firmware's own
 * use (H2C queue, ...), the rest is shared by the transmit queues. The firmware commands (H2C) get a ring inside the reserved pages. Then
 * the timing constants of the 802.11 MAC (slot, SIFS, TXOP limits) and the receive filter are set. No radio is configured here. */
#define R_TX_HANG_CTRL 0x045E
#define R_AMPDU_MAX_TIME_V1 0x0455
#define R_PRECNT_CTRL 0x04E5
#define R_PROT_MODE_CTRL 0x04C8
#define R_BAR_MODE_CTRL 0x04CC
#define R_FAST_EDCA_VOVI 0x1448
#define R_FAST_EDCA_BEBK 0x144C
#define R_INIRTS_RATE_SEL 0x0480
#define R_TIMER0_SRC_SEL 0x05B4
#define R_TXPAUSE 0x0522
#define R_SLOT 0x051B
#define R_PIFS 0x0512
#define R_SIFS 0x0514
#define R_EDCA_VO_PARAM 0x0500
#define R_EDCA_VI_PARAM 0x0504
#define R_RD_NAV_NXT 0x0544
#define R_RXTSF_OFFSET_CCK 0x055E
#define R_TBTT_PROHIBIT 0x0540
#define R_DRVERLYINT 0x0558
#define R_BCNDMATIM 0x0559
#define R_TX_PTCL_CTRL 0x0520
#define R_RXFLTMAP0 0x06A0
#define R_RXFLTMAP1 0x06A2
#define R_RXFLTMAP2 0x06A4
#define R_RCR 0x0608
#define R_RX_PKT_LIMIT 0x060C
#define R_RX_DRVINFO_SZ 0x060F
#define R_TCR 0x0604
#define R_ACKTO_CCK 0x0639
#define R_WMAC_TRXPTCL_CTL_H 0x066C
#define R_SND_PTCL_CTRL 0x0718
#define R_WMAC_OPTION_FUNCTION 0x07D0
#define R_TRXFF_BNDY 0x0114
#define R_RXFF_BNDY 0x011C
#define R_AUTO_LLT_V1 0x0208
#define R_BCNQ_BDNY_V1 0x0424
#define R_BCNQ1_BDNY_V1 0x0456
#define R_H2C_HEAD 0x0244
#define R_H2C_TAIL 0x0248
#define R_H2C_READ_ADDR 0x024C
#define R_H2C_INFO 0x0254
#define R_H2C_PKT_READADDR 0x10D0
#define R_H2C_PKT_WRITEADDR 0x10D4
#define R_TXDMA_OFFSET_CHK 0x020C
#define MAC_TRX_ENABLE 0xFF /* TX/RX DMA, protocol, scheduler, MAC TX and RX: bits 0..7 of the command register */

/* the geometry of this chip (rtw8821c_hw_spec, page_table_8821c[1], rqpn_table_8821c[1]) */
#define TX_PAGE_SIZE_SHIFT 7
#define TXFF_PAGES (65536 >> TX_PAGE_SIZE_SHIFT)
#define RSVD_PAGES (8 + 24 + 8 + 8 + 0 + 4 + 0)         /* driver, H2C extra info, H2C static info, H2C queue, CPU instructions, FW TX buffer, CSI */
#define RSVD_BOUNDARY (TXFF_PAGES - RSVD_PAGES)         /* the first reserved page: 460 */
#define H2CQ_PAGE (TXFF_PAGES - 0 - 4 - 0 - 8)          /* below the CSI (0), FW TX buffer (4) and CPU instructions (0) */
#define H2CQ_SIZE (8 << TX_PAGE_SIZE_SHIFT)
#define RXFF_SIZE 16384
#define C2H_PKT_BUF 256
#define PAGES_HQ 16
#define PAGES_LQ 16
#define PAGES_NQ 16
#define PAGES_EXQ 14
#define PAGES_GAPQ 1
#define PAGES_PUBQ (RSVD_BOUNDARY - PAGES_HQ - PAGES_LQ - PAGES_NQ - PAGES_EXQ - PAGES_GAPQ)
/* queue -> DMA priority queue (0 extra, 1 low, 2 normal, 3 high), the PCIe row */
#define DMA_VO 2
#define DMA_VI 2
#define DMA_BE 1
#define DMA_BK 1
#define DMA_MG 0
#define DMA_HI 3
#define TXDMA_PQ_MAP ((DMA_HI << 14) | (DMA_MG << 12) | (DMA_BK << 10) | (DMA_BE << 8) | (DMA_VI << 6) | (DMA_VO << 4))

/* the protocol constants of rtw8821c.h */
#define WLAN_SLOT_TIME 0x09
#define WLAN_PIFS_TIME 0x19
#define WLAN_SIFS_CCK_CONT_TX 0xA
#define WLAN_SIFS_OFDM_CONT_TX 0xE
#define WLAN_SIFS_CCK_TRX 0x10
#define WLAN_SIFS_OFDM_TRX 0x10
#define WLAN_VO_TXOP_LIMIT 0x186
#define WLAN_VI_TXOP_LIMIT 0x3BC
#define WLAN_RDG_NAV 0x05
#define WLAN_TXOP_NAV 0x1B
#define WLAN_CCK_RX_TSF 0x30
#define WLAN_OFDM_RX_TSF 0x30
#define WLAN_TBTT_PROHIBIT 0x04
#define WLAN_TBTT_HOLD_TIME 0x064
#define WLAN_DRV_EARLY_INT 0x04
#define WLAN_BCN_DMA_TIME 0x02
#define WLAN_RX_FILTER0 0xFFFF
#define WLAN_RX_FILTER1 0x0FFF
#define WLAN_RX_FILTER2 0xFFFF
#define WLAN_RCR_CFG 0xE400220E
#define WLAN_RXPKT_MAX_SZ 12288
#define WLAN_RXPKT_MAX_SZ_512 (WLAN_RXPKT_MAX_SZ >> 9)
#define WLAN_AMPDU_MAX_TIME 0x70
#define WLAN_RTS_LEN_TH 0xFF
#define WLAN_RTS_TX_TIME_TH 0x08
#define WLAN_MAX_AGG_PKT_LIMIT 0x20
#define WLAN_RTS_MAX_AGG_PKT_LIMIT 0x20
#define FAST_EDCA_TH 0x06 /* VO, VI, BE and BK all use 6 */
#define WLAN_BAR_RETRY_LIMIT 0x01
#define WLAN_RA_TRY_RATE_AGG_LIMIT 0x08
#define WLAN_TX_FUNC_CFG1 0x30
#define WLAN_TX_FUNC_CFG2 0x30
#define WLAN_MAC_OPT_NORM_FUNC1 0x98
#define WLAN_MAC_OPT_FUNC2 0xb0810041
#define WLAN_PRE_TXCNT_TIME_TH 0x1E4
#define WLAN_SIFS_CFG (WLAN_SIFS_CCK_CONT_TX | (WLAN_SIFS_OFDM_CONT_TX << 8) | (WLAN_SIFS_CCK_TRX << 16) | (WLAN_SIFS_OFDM_TRX << 24))
#define WLAN_TBTT_TIME (WLAN_TBTT_PROHIBIT | (WLAN_TBTT_HOLD_TIME << 8))
#define WLAN_NAV_CFG (WLAN_RDG_NAV | (WLAN_TXOP_NAV << 16))
#define WLAN_RX_TSF_CFG (WLAN_CCK_RX_TSF | (WLAN_OFDM_RX_TSF) << 8)

enum { MI_NOT_TRIED, MI_OK, MI_FAILED };
static struct
{
    int state;
    const char *why;
    u8 cr; /* the command register once the init is through */
} MI;

static void or32(u32 off, u32 bits) { wr32(off, rd32(off) | bits); }
static void andn32(u32 off, u32 bits) { wr32(off, rd32(off) & ~bits); }

/* rtw_init_trx_cfg: txdma_queue_mapping + priority_queue_cfg + init_h2c. 0 = ok, else the reason. */
static const char *init_trx_cfg(void)
{
    /* txdma_queue_mapping: which transmit queue feeds which priority queue; then the DMA engines and the MAC switch on */
    wr16(R_TXDMA_PQ_MAP, TXDMA_PQ_MAP);
    wr8(R_CR, 0);
    wr8(R_CR, MAC_TRX_ENABLE);
    wr32(R_H2CQ_CSR, BIT(31)); /* BIT_H2CQ_FULL, the CPU is the 3081 kind */

    /* priority_queue_cfg: how many pages each queue owns, where the reserved ones begin, and the chip builds its page chain itself */
    wr16(R_FIFOPAGE_INFO_1, PAGES_HQ);
    wr16(R_FIFOPAGE_INFO_1 + 4, PAGES_LQ);
    wr16(R_FIFOPAGE_INFO_1 + 8, PAGES_NQ);
    wr16(R_FIFOPAGE_INFO_1 + 12, PAGES_EXQ);
    wr16(R_FIFOPAGE_INFO_1 + 16, PAGES_PUBQ);
    or32(R_RQPN_CTRL_2, BIT(31)); /* BIT_LD_RQPN: load the numbers */
    wr16(R_FIFOPAGE_CTRL_2, RSVD_BOUNDARY);
    set8(R_FWHW_TXQ_CTRL + 2, (u8)(BIT(20) >> 16)); /* BIT_EN_WR_FREE_TAIL */
    wr16(R_BCNQ_BDNY_V1, RSVD_BOUNDARY);
    wr16(R_FIFOPAGE_CTRL_2 + 2, RSVD_BOUNDARY);
    wr16(R_BCNQ1_BDNY_V1, RSVD_BOUNDARY);
    wr32(R_RXFF_BNDY, RXFF_SIZE - C2H_PKT_BUF - 1);
    set8(R_AUTO_LLT_V1, BIT(0)); /* BIT_AUTO_INIT_LLT_V1: the chip links its pages, then clears the bit */
    if (!ready32(R_AUTO_LLT_V1, BIT(0), 0, 1000))
        return "llt";
    wr8(R_CR + 3, 0);

    /* init_h2c: the ring of firmware commands lives in the reserved pages */
    u32 head = H2CQ_PAGE << TX_PAGE_SIZE_SHIFT;
    wr32(R_H2C_HEAD, (rd32(R_H2C_HEAD) & 0xFFFC0000u) | head);
    wr32(R_H2C_READ_ADDR, (rd32(R_H2C_READ_ADDR) & 0xFFFC0000u) | head);
    wr32(R_H2C_TAIL, (rd32(R_H2C_TAIL) & 0xFFFC0000u) | (head + H2CQ_SIZE));
    wr8(R_H2C_INFO, (u8)((rd8(R_H2C_INFO) & 0xFC) | 0x01));
    wr8(R_H2C_INFO, (u8)((rd8(R_H2C_INFO) & 0xFB) | 0x04));
    wr8(R_TXDMA_OFFSET_CHK + 1, (u8)((rd8(R_TXDMA_OFFSET_CHK + 1) & 0x7F) | 0x80));
    u32 wp = rd32(R_H2C_PKT_WRITEADDR) & 0x3FFFF, rp = rd32(R_H2C_PKT_READADDR) & 0x3FFFF;
    u32 free_bytes = wp >= rp ? H2CQ_SIZE - (wp - rp) : rp - wp;
    if (free_bytes != H2CQ_SIZE) /* the ring must start empty */
        return "h2c";
    return 0;
}

/* rtw8821c_mac_init: protocol parameters, EDCA timing, beacon timing and the receive filter */
static void init_wmac(void)
{
    wr8(R_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
    set8(R_TX_HANG_CTRL, BIT(2)); /* BIT_EN_EOF_V1 */
    u16 pre_txcnt = WLAN_PRE_TXCNT_TIME_TH | BIT(11); /* BIT_EN_PRECNT */
    wr8(R_PRECNT_CTRL, (u8)(pre_txcnt & 0xFF));
    wr8(R_PRECNT_CTRL + 1, (u8)(pre_txcnt >> 8));
    wr32(R_PROT_MODE_CTRL, WLAN_RTS_LEN_TH | (WLAN_RTS_TX_TIME_TH << 8) | (WLAN_MAX_AGG_PKT_LIMIT << 16) | (WLAN_RTS_MAX_AGG_PKT_LIMIT << 24));
    wr16(R_BAR_MODE_CTRL + 2, WLAN_BAR_RETRY_LIMIT | WLAN_RA_TRY_RATE_AGG_LIMIT << 8);
    wr8(R_FAST_EDCA_VOVI, FAST_EDCA_TH);
    wr8(R_FAST_EDCA_VOVI + 2, FAST_EDCA_TH);
    wr8(R_FAST_EDCA_BEBK, FAST_EDCA_TH);
    wr8(R_FAST_EDCA_BEBK + 2, FAST_EDCA_TH);
    set8(R_INIRTS_RATE_SEL, BIT(5));

    clr8(R_TIMER0_SRC_SEL, BIT(4) | BIT(5) | BIT(6)); /* BIT_TSFT_SEL_TIMER0 */
    wr16(R_TXPAUSE, 0);
    wr8(R_SLOT, WLAN_SLOT_TIME);
    wr8(R_PIFS, WLAN_PIFS_TIME);
    wr32(R_SIFS, WLAN_SIFS_CFG);
    wr16(R_EDCA_VO_PARAM + 2, WLAN_VO_TXOP_LIMIT);
    wr16(R_EDCA_VI_PARAM + 2, WLAN_VI_TXOP_LIMIT);
    wr32(R_RD_NAV_NXT, WLAN_NAV_CFG);
    wr16(R_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);

    set8(R_BCN_CTRL, BIT(3)); /* BIT_EN_BCN_FUNCTION: the TSF timer runs */
    wr32(R_TBTT_PROHIBIT, WLAN_TBTT_TIME);
    wr8(R_DRVERLYINT, WLAN_DRV_EARLY_INT);
    wr8(R_BCNDMATIM, WLAN_BCN_DMA_TIME);
    clr8(R_TX_PTCL_CTRL + 1, (u8)(BIT(12) >> 8)); /* BIT_SIFS_BK_EN */

    wr16(R_RXFLTMAP0, WLAN_RX_FILTER0);
    wr16(R_RXFLTMAP1, WLAN_RX_FILTER1);
    wr16(R_RXFLTMAP2, WLAN_RX_FILTER2);
    wr32(R_RCR, WLAN_RCR_CFG);
    wr8(R_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
    wr8(R_TCR + 2, WLAN_TX_FUNC_CFG2);
    wr8(R_TCR + 1, WLAN_TX_FUNC_CFG1);
    wr8(R_ACKTO_CCK, 0x40);
    set8(R_WMAC_TRXPTCL_CTL_H, BIT(1));
    set8(R_SND_PTCL_CTRL, BIT(6)); /* BIT_DIS_CHK_VHTSIGB_CRC */
    wr32(R_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
    wr8(R_WMAC_OPTION_FUNCTION + 4, WLAN_MAC_OPT_NORM_FUNC1);
}

/* rtw_drv_info_cfg: every received frame carries a 4 * 8 byte PHY status in front of it */
static void init_drv_info(void)
{
    wr8(R_RX_DRVINFO_SZ, 4);
    wr8(R_TRXFF_BNDY + 1, (u8)((rd8(R_TRXFF_BNDY + 1) & 0xF0) | 0xF)); /* "for the rxdesc len = 0 issue" */
    or32(R_RCR, BIT(28));                                              /* BIT_APP_PHYSTS */
    andn32(R_WMAC_OPTION_FUNCTION + 4, BIT(8) | BIT(9));
}

static void mac_init(void)
{
    MI.state = MI_FAILED;
    MI.why = init_trx_cfg();
    if (MI.why)
        return;
    init_wmac();
    init_drv_info();
    MI.cr = rd8(R_CR);
    MI.state = MI_OK;
}

/* ---- stage 4b-2: the baseband, AGC and RF tables (the table part of rtw8821c_phy_set_param) ----
 * The radio is configured by long lists of register writes that Realtek ships as tables (include/rtl8821ce_phytab.h, copied from
 * Linux). A list holds several variants of the same setting for different chip cuts / board types, written as "if (conditions) ...
 * else ... endif" blocks inside the list; only the variant of THIS chip (cut, package, interface, RFE type from the efuse) is written.
 * The RF registers are not memory mapped: they are written through the "SIPI" serial interface (one 32-bit word: register number in
 * bits 27..20, data in bits 19..0 at 0xC90 for path A) and can be read back directly from a window at 0x2800 + 4 * number.
 * Not done here (software state or later stages): rtw_phy_init, the power-tracking and beamforming set-up, calibration (IQK / LCK). */

#define R_RXPSEL 0x0808
#define R_AFE_XTAL_CTRL 0x0024
#define R_AFE_PLL_CTRL 0x0028
#define R_CCK0_FAREPORT 0x0A2C
#define R_RF_SIPI_A 0x0C90
#define R_RF_BASE_A 0x2800
#define RFREG_MASK 0xFFFFF

enum { TAB_MAC, TAB_AGC, TAB_BB, TAB_RF };
enum { PH_NOT_TRIED, PH_OK, PH_SKIPPED };
static struct
{
    int state;
    const char *why;
    unsigned cond;      /* what this chip is, in the layout of a table's condition word */
    unsigned rf_diff;   /* RF registers that read back differently from what was written last */
    unsigned rf_written;
    unsigned rf_regs;   /* distinct RF registers written */
    unsigned char diff_addr[40]; /* the first RF registers that did not read back ... */
    unsigned diff_xor[40];       /* ... and which bits of them differ */
    unsigned char rf_seen[256];
    u32 ch_param[3];    /* TXSF2, TXSF6, TXFILTER as the tables left them */
    unsigned rf_last[256];
} PH;

static void delay_us(unsigned us) { udelay(us); }

/* the cut, package, interface and RFE type that the "if" blocks of the tables ask about (rtw_phy_setup_phy_cond) */
static unsigned phy_cond_word(unsigned cut, unsigned pkg, unsigned rfe)
{
    return (rfe & 0xFF) | (1u << 8) /* INTF_PCIE */ | ((pkg ? pkg : 15) << 12) | (4u << 16) /* plat */ | ((cut ? cut : 15) << 24);
}

/* check_positive: a zero field in the table's condition means "any" */
static int phy_cond_matches(unsigned c)
{
    unsigned d = PH.cond;
    if (((c >> 24) & 0xF) && ((c >> 24) & 0xF) != ((d >> 24) & 0xF))
        return 0;
    if (((c >> 12) & 0xF) && ((c >> 12) & 0xF) != ((d >> 12) & 0xF))
        return 0;
    if (((c >> 8) & 0xF) && ((c >> 8) & 0xF) != ((d >> 8) & 0xF))
        return 0;
    return (c & 0xFF) == (d & 0xFF);
}

static void phy_rf_write(unsigned addr, unsigned data)
{
    addr &= 0xFF;
    wr32(R_RF_SIPI_A, ((addr << 20) | (data & RFREG_MASK)) & 0x0FFFFFFF);
    delay_us(13);
    PH.rf_seen[addr] = 1;
    PH.rf_last[addr] = data & RFREG_MASK;
    PH.rf_written++;
}

static void phy_cfg(int kind, unsigned addr, unsigned data)
{
    switch (kind)
    {
    case TAB_MAC:
        wr8(addr, (u8)data);
        break;
    case TAB_AGC:
        wr32(addr, data);
        break;
    case TAB_BB:
        if (addr == 0xFE)
            delay_us(50000);
        else if (addr == 0xFD)
            delay_us(5000);
        else if (addr == 0xFC)
            delay_us(1000);
        else if (addr == 0xFB)
            delay_us(50);
        else if (addr == 0xFA)
            delay_us(5);
        else if (addr == 0xF9)
            delay_us(1);
        else
            wr32(addr, data);
        break;
    default: /* TAB_RF, path A */
        if (addr == 0xFFE)
            delay_us(50000);
        else if (addr == 0xFE)
            delay_us(100);
        else
        {
            phy_rf_write(addr, data);
            delay_us(1);
        }
        break;
    }
}

/* rtw_parse_tbl_phy_cond: bit 31 of the first word = "this is a condition" (branch in bits 29..28: 0 if, 1 elif, 2 else, 3 endif);
 * bit 30 = "the conditions are complete, decide now"; anything else is a register write while the current block is selected. */
static void phy_table_run(int kind, const unsigned *t, unsigned words)
{
    unsigned pos_cond = 0;
    int matched = 1, skipped = 0;
    for (unsigned i = 0; i + 1 < words; i += 2)
    {
        unsigned w = t[i];
        if (w & BIT(31))
        {
            unsigned branch = (w >> 28) & 3;
            if (branch == 3)
            {
                matched = 1;
                skipped = 0;
            }
            else if (branch == 2)
                matched = skipped ? 0 : 1;
            else
                pos_cond = w;
        }
        else if (w & BIT(30))
        {
            if (!skipped)
            {
                if (phy_cond_matches(pos_cond))
                {
                    matched = 1;
                    skipped = 1;
                }
                else
                {
                    matched = 0;
                    skipped = 0;
                }
            }
            else
                matched = 0;
        }
        else if (matched)
            phy_cfg(kind, w, t[i + 1]);
    }
}

#define TAB_WORDS(t) ((unsigned)(sizeof(t) / sizeof((t)[0])))

static void mask32(u32 off, u32 mask, u32 value)
{
    unsigned shift = 0;
    while (!((mask >> shift) & 1))
        shift++;
    wr32(off, (rd32(off) & ~mask) | ((value << shift) & mask));
}

static void phy_init(unsigned cut)
{
    PH.state = PH_SKIPPED;
    PH.why = 0;
    PH.rf_diff = PH.rf_written = 0;
    PH.rf_regs = 0;
    for (int i = 0; i < 256; i++)
        PH.rf_seen[i] = 0;
    if (!EF.map_ok)
    {
        PH.why = "efuse";
        return;
    }
    if (EF.rfe != 0 && EF.rfe != 2 && EF.rfe != 4 && EF.rfe != 6) /* the only board types rtw8821c_rfe_defs has: Linux refuses the others */
    {
        PH.why = "rfe";
        return;
    }
    PH.cond = phy_cond_word(cut, EF.pkg, EF.rfe);

    /* power on the baseband and RF domains, then pulse the baseband reset */
    u8 val = rd8(R_SYS_FUNC_EN);
    val |= BIT(6); /* BIT_FEN_PCIEA */
    wr8(R_SYS_FUNC_EN, val);
    val |= BIT(0) | BIT(1); /* BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST */
    wr8(R_SYS_FUNC_EN, val);
    val &= (u8) ~(BIT(0) | BIT(1));
    wr8(R_SYS_FUNC_EN, val);
    val |= BIT(0) | BIT(1);
    wr8(R_SYS_FUNC_EN, val);
    wr8(R_RF_CTRL, BIT(0) | BIT(1) | BIT(2)); /* BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB */
    delay_us(10);
    wr8(R_WLRF1 + 3, BIT(0) | BIT(1) | BIT(2));
    delay_us(10);

    andn32(R_RXPSEL, BIT(28) | BIT(29)); /* BIT_RX_PSEL_RST: held while the tables are written */

    phy_table_run(TAB_MAC, rtl8821ce_tab_mac, TAB_WORDS(rtl8821ce_tab_mac));
    phy_table_run(TAB_BB, rtl8821ce_tab_bb, TAB_WORDS(rtl8821ce_tab_bb));
    phy_table_run(TAB_AGC, rtl8821ce_tab_agc, TAB_WORDS(rtl8821ce_tab_agc));
    if (EF.rfe == 2 || EF.rfe == 4) /* rtw8821c_rfe_defs: these board types carry an extra AGC table (the BTG antenna switch) */
        phy_table_run(TAB_AGC, rtl8821ce_tab_agc_btg2, TAB_WORDS(rtl8821ce_tab_agc_btg2));
    phy_table_run(TAB_RF, rtl8821ce_tab_rf_a, TAB_WORDS(rtl8821ce_tab_rf_a));

    unsigned xtal = EF.xtal & 0x3F;
    mask32(R_AFE_XTAL_CTRL, 0x7E000000, xtal);
    mask32(R_AFE_PLL_CTRL, 0x7E, xtal);
    mask32(R_CCK0_FAREPORT, BIT(18) | BIT(22), 0);
    or32(R_RXPSEL, BIT(28) | BIT(29));
    PH.ch_param[0] = rd32(0x0A24); /* REG_TXSF2 */
    PH.ch_param[1] = rd32(0x0A28); /* REG_TXSF6 */
    PH.ch_param[2] = rd32(0x0AAC); /* REG_TXFILTER */

    /* the RF registers answer at 0x2800 + 4 * number: do they hold what was last written? */
    for (int a = 0; a < 256; a++)
        if (PH.rf_seen[a] && (rd32(R_RF_BASE_A + ((unsigned)a << 2)) & RFREG_MASK) != PH.rf_last[a])
        {
            if (PH.rf_diff < sizeof PH.diff_addr)
            {
                PH.diff_addr[PH.rf_diff] = (unsigned char)a;
                PH.diff_xor[PH.rf_diff] = (rd32(R_RF_BASE_A + ((unsigned)a << 2)) & RFREG_MASK) ^ PH.rf_last[a];
            }
            PH.rf_diff++;
        }
    for (int a = 0; a < 256; a++)
        PH.rf_regs += PH.rf_seen[a];
    PH.state = PH_OK;
}

/* Some RF bits are driven by the chip itself (mode, channel and bandwidth, bias settings) and read back differently from what was
 * written, even on a healthy card: 26 of about 90 registers on the real one. Only if more than half differ is the write path suspect. */
static int phy_rf_suspect(void) { return PH.rf_diff * 2 > PH.rf_regs; }

/* ---- stage 4b-3: the first firmware command (H2C through the mailbox registers) ----
 * A short command goes to the firmware through one of four mailboxes: two 32-bit registers each (HMEBOX0_EX = bytes 4..7, HMEBOX0 =
 * bytes 0..3, written last). The chip sets the box's bit in HMETFR when the low word is written and the firmware clears it when it has
 * read the command: that is the proof that the firmware runs and listens. The command used is "tell the Bluetooth coexistence code not
 * to ignore WLAN actions, off" (H2C 0x63), the default state: it changes nothing and the firmware answers it with no packet (the
 * receive DMA is still off). The same sequence as rtw_fw_send_h2c_command(). */
#define R_HMETFR 0x01CC
#define R_HMEBOX0 0x01D0
#define R_HMEBOX0_EX 0x01F0
#define H2C_CMD_IGNORE_WLAN_ACTION 0x63

enum { H2_NOT_TRIED, H2_OK, H2_FAILED };
static struct
{
    int state;
    const char *why;
} H2;

static int h2c_box_free(void)
{
    for (int i = 0; i < 30; i++) /* 3 ms in steps of 100 us, like Linux */
    {
        if (!(rd8(R_HMETFR) & BIT(0)))
            return 1;
        udelay(100);
    }
    return 0;
}

static void h2c_probe(void)
{
    H2.state = H2_FAILED;
    H2.why = 0;
    if (!h2c_box_free())
    {
        H2.why = "busy";
        return;
    }
    wr32(R_HMEBOX0_EX, 0);
    wr32(R_HMEBOX0, H2C_CMD_IGNORE_WLAN_ACTION); /* bit 8 = enable = 0 */
    for (int i = 0; i < 200; i++) /* up to 20 ms for the firmware to take it */
    {
        if (!(rd8(R_HMETFR) & BIT(0)))
        {
            H2.state = H2_OK;
            return;
        }
        udelay(100);
    }
    H2.why = "no ack";
}

/* ---- stage 4c-1: antenna path ("Wi-Fi only") and channel ----
 * Wi-Fi and Bluetooth share the radio front end of this card. Two things must be told to the chip before a frame can be received:
 *   - who owns the antenna: the Bluetooth coexistence block has "grant" lines (GNT_WL / GNT_BT) and a switch (DPDT, driven by the RFE
 *     control pins) that points the antenna at Wi-Fi or at Bluetooth. This is the part of Linux' rtw_coex_set_ant_path() for the phase
 *     "Wi-Fi only" (GNT_BT forced low, GNT_WL forced high, the switch to Wi-Fi), without the Bluetooth bookkeeping around it;
 *   - which channel: rtw8821c_set_channel(): baseband, RF synthesizer and MAC clock settings for the channel, 20 MHz wide.
 * The grant lines are in a register of the Bluetooth/LTE coexistence block that is only reachable through an indirect interface
 * (0x1700 control, 0x1704 data to write, 0x1708 data read). */
#define R_AFE_CTRL1 0x0024
#define R_SYS_CTRL 0x0000
#define R_SYS_SDIO_CTRL 0x0070
#define R_CTRL_TYPE 0x0067
#define R_DATA_SC 0x0483
#define R_USTIME_TSF 0x055C
#define R_USTIME_EDCA 0x0638
#define R_WMAC_TRXPTCL_CTL 0x0668
#define R_CCK_CHECK 0x0454
#define R_RXCCAMSK 0x0814
#define R_CCA_FLTR 0x0A20
#define R_ENTXCCK 0x0A80
#define R_ENRXCCA 0x0A84
#define R_TXSF2 0x0A24
#define R_TXSF6 0x0A28
#define R_TXFILTER 0x0AAC
#define R_CLKTRK 0x0860
#define R_ADCCLK 0x08AC
#define R_ADC160 0x08C4
#define R_CHFIR 0x08F0
#define R_ACBB0 0x0948
#define R_ACBBRXFIR 0x094C
#define R_TXDFIR 0x0C20
#define R_TXSCALE_A 0x0C1C
#define R_RFECTL 0x0CB8
#define R_RFE_CTRL8 0x0CB4
#define R_DMEM_CTRL 0x1080
#define R_LTECOEX_CTRL 0x1700
#define R_LTECOEX_WDATA 0x1704
#define R_LTECOEX_RDATA 0x1708
#define LTECOEX_READY BIT(29)
#define LTE_COEX_CTRL 0x38 /* the register of the coexistence block that holds the grant lines */
#define RF_LUTDBG 0xDF
#define RF_XTALX2 0xB8
#define RF18_BAND_2G 0x0
#define RF18_BAND_5G 0x10100
#define RF18_BAND_MASK 0x10300
#define RF18_CHANNEL_MASK 0xFF
#define RF18_RFSI_GE 0x20000
#define RF18_RFSI_GT 0x40000
#define RF18_RFSI_MASK 0x60000
#define RF18_BW_MASK 0xC00
#define RF18_BW_20M 0xC00
#define B_BTG_SWITCH 0x10000
#define B_CTRL_SWITCH 0x40000
#define B_WL_SWITCH 0x500000
#define B_WLG_SWITCH 0x200000
#define B_WLA_SWITCH 0x800000
#define BTG_CCA 0xE
#define BTG_LNA 0xFC84
#define WLG_CCA 0x12
#define WLG_LNA 0x7532
#define DPDT_CTRL_PIN 0x77
#define PTA_CTRL_PIN 0x66
#define ANTDIC_CTRL_PIN 0x88
#define MAC_CLK_SPEED 0x50

/* the indirect interface: read or write one 32-bit register of the coexistence block; 0 = ok */
static int lte_read(u16 off, u32 *val)
{
    if (!ready32(R_LTECOEX_CTRL, LTECOEX_READY, LTECOEX_READY, 1000))
        return -1;
    wr32(R_LTECOEX_CTRL, 0x800F0000u | off);
    *val = rd32(R_LTECOEX_RDATA);
    return 0;
}

static int lte_write(u16 off, u32 val)
{
    if (!ready32(R_LTECOEX_CTRL, LTECOEX_READY, LTECOEX_READY, 1000))
        return -1;
    wr32(R_LTECOEX_WDATA, val);
    wr32(R_LTECOEX_CTRL, 0xC00F0000u | off);
    return 0;
}

static int lte_write_mask(u16 off, u32 mask, u32 val)
{
    u32 shift = 0, tmp = 0;
    while (!((mask >> shift) & 1))
        shift++;
    if (lte_read(off, &tmp))
        tmp = 0; /* Linux: "failed to read indirect register", carries on with 0 */
    tmp = (tmp & ~mask) | ((val << shift) & mask);
    return lte_write(off, tmp);
}

static void coex_gnt_bt(u32 state)
{
    lte_write_mask(LTE_COEX_CTRL, 0xC000, state);
    lte_write_mask(LTE_COEX_CTRL, 0x0C00, state);
}

static void coex_gnt_wl(u32 state)
{
    lte_write_mask(LTE_COEX_CTRL, 0x3000, state);
    lte_write_mask(LTE_COEX_CTRL, 0x0300, state);
}

/* rtw_coex_coex_ctrl_owner(true): the Wi-Fi side controls the shared path (this chip has no extra BTG register) */
static void coex_owner_wifi(void) { set8(R_SYS_SDIO_CTRL + 3, (u8)(BIT(26) >> 24)); }

enum { SW_TO_BT, SW_TO_WLG, SW_TO_WLA, SW_TO_NOCARE, SW_TO_WLG_BT };
enum { SWCTRL_BBSW, SWCTRL_PTA, SWCTRL_ANTDIV, SWCTRL_MAC, SWCTRL_BT, SWCTRL_FW };
static struct
{
    int ant_switch_exist, wlg_at_btg, polarity;
    unsigned module_type;
    unsigned cur_switch_status;
    int rfe_btg;
} CX;

static void mask8(u32 off, u32 mask, u8 value)
{
    unsigned shift = 0;
    mask &= 0xFF;
    while (!((mask >> shift) & 1))
        shift++;
    wr8(off, (u8)((rd8(off) & ~mask) | ((value << shift) & mask)));
}

/* rtw8821c_coex_cfg_rfe_type: what the board's RFE type says about its antenna wiring */
static void coex_rfe_type(unsigned rfe)
{
    CX.module_type = rfe;
    CX.polarity = 0;
    CX.ant_switch_exist = 1;
    CX.wlg_at_btg = 0;
    switch (rfe)
    {
    case 2:
    case 10:
    case 7:
    case 15: /* BTG: the 2.4 GHz Wi-Fi signal goes in at the Bluetooth port */
        CX.wlg_at_btg = 1;
        break;
    case 3:
    case 11:
        CX.polarity = 1;
        break;
    case 4:
    case 12:
        CX.wlg_at_btg = 1;
        CX.polarity = 1;
        break;
    case 5:
    case 13:
    case 6:
    case 14: /* no antenna switch on the board */
        CX.ant_switch_exist = 0;
        break;
    default:
        break;
    }
    CX.rfe_btg = rfe == 2 || rfe == 4 || rfe == 7 || rfe == 0xA || rfe == 0xC || rfe == 0xF; /* hal->rfe_btg */
}

/* rtw8821c_coex_cfg_ant_switch */
static void coex_ant_switch(unsigned ctrl_type, unsigned pos_type)
{
    unsigned switch_status = ((ctrl_type << 8) & 0xFF00) | pos_type;
    unsigned regval = 0;
    if (switch_status == CX.cur_switch_status)
        return;
    if (CX.wlg_at_btg)
    {
        ctrl_type = SWCTRL_BBSW;
        pos_type = CX.polarity ? SW_TO_WLA : SW_TO_WLG_BT;
    }
    CX.cur_switch_status = switch_status;
    int polarity_inverse = CX.polarity == 1;
    switch (ctrl_type)
    {
    default:
    case SWCTRL_BBSW:
        andn32(R_LED_CFG, BIT(23)); /* BIT_DPDT_SEL_EN */
        or32(R_LED_CFG, BIT(24));   /* BIT_DPDT_WL_SEL */
        mask8(R_RFE_CTRL8, 0xFF, DPDT_CTRL_PIN); /* the DPDT is driven by RFE pins 8 and 9 */
        if (pos_type == SW_TO_WLG_BT)
        {
            if (CX.module_type != 0x4 && CX.module_type != 0x2)
                regval = 0x3;
            else
                regval = !polarity_inverse ? 0x2 : 0x1;
        }
        else if (pos_type == SW_TO_WLG)
            regval = !polarity_inverse ? 0x2 : 0x1;
        else
            regval = !polarity_inverse ? 0x1 : 0x2;
        mask32(R_RFE_CTRL8, 0xF0000000u, regval);
        break;
    case SWCTRL_PTA:
        andn32(R_LED_CFG, BIT(23));
        or32(R_LED_CFG, BIT(24));
        mask8(R_RFE_CTRL8, 0xFF, PTA_CTRL_PIN);
        regval = !polarity_inverse ? 0x2 : 0x1;
        mask32(R_RFE_CTRL8, 0xF0000000u, regval);
        break;
    case SWCTRL_ANTDIV:
        andn32(R_LED_CFG, BIT(23));
        or32(R_LED_CFG, BIT(24));
        mask8(R_RFE_CTRL8, 0xFF, ANTDIC_CTRL_PIN);
        break;
    case SWCTRL_MAC:
        or32(R_LED_CFG, BIT(23));
        regval = !polarity_inverse ? 0x0 : 0x1;
        mask8(R_PAD_CTRL1, BIT(0), (u8)regval); /* BIT_SW_DPDT_SEL_DATA */
        break;
    case SWCTRL_FW:
        andn32(R_LED_CFG, BIT(23));
        or32(R_LED_CFG, BIT(24));
        break;
    case SWCTRL_BT:
        andn32(R_LED_CFG, BIT(23));
        andn32(R_LED_CFG, BIT(24));
        break;
    }
    if (ctrl_type == SWCTRL_BT)
    {
        clr8(R_CTRL_TYPE, BIT(5));
        clr8(R_CTRL_TYPE, BIT(4));
    }
    else
    {
        set8(R_CTRL_TYPE, BIT(5));
        set8(R_CTRL_TYPE, BIT(4));
    }
}

/* rtw_coex_set_ant_path(COEX_SET_ANT_WONLY) */
static void coex_wifi_only(unsigned rfe)
{
    CX.cur_switch_status = 0;
    coex_rfe_type(rfe);
    coex_gnt_bt(1); /* COEX_GNT_SET_SW_LOW: Bluetooth gets no grant */
    coex_gnt_wl(3); /* COEX_GNT_SET_SW_HIGH: Wi-Fi always has it */
    coex_owner_wifi();
    if (CX.ant_switch_exist)
        coex_ant_switch(SWCTRL_BBSW, SW_TO_WLG);
}

static unsigned rf_read(unsigned addr) { return rd32(R_RF_BASE_A + ((addr & 0xFF) << 2)) & RFREG_MASK; }
static void rf_write_mask(unsigned addr, unsigned mask, unsigned data)
{
    mask &= RFREG_MASK;
    if (mask != RFREG_MASK)
    {
        unsigned shift = 0;
        while (!((mask >> shift) & 1))
            shift++;
        data = (rf_read(addr) & ~mask) | (data << shift);
    }
    phy_rf_write(addr, data); /* the write path of the tables: SIPI, then 13 us */
}

/* rtw8821c_switch_rf_set: which RF input the receiver listens to (0 BTG, 1 WLG, 2 WLA) */
static void switch_rf_set(int to)
{
    or32(R_DMEM_CTRL, BIT(16)); /* BIT_WL_RST */
    or32(R_SYS_CTRL, BIT(26));  /* BIT_FEN_EN */
    u32 reg = rd32(R_RFECTL);
    switch (to)
    {
    case 0:
        reg |= B_BTG_SWITCH;
        reg &= ~(B_CTRL_SWITCH | B_WL_SWITCH | B_WLG_SWITCH | B_WLA_SWITCH);
        mask32(R_ENRXCCA, 0xFF0000, BTG_CCA);
        mask32(R_ENTXCCK, 0xFFFF, BTG_LNA);
        break;
    case 1:
        reg |= B_WL_SWITCH | B_WLG_SWITCH;
        reg &= ~(B_BTG_SWITCH | B_CTRL_SWITCH | B_WLA_SWITCH);
        mask32(R_ENRXCCA, 0xFF0000, WLG_CCA);
        mask32(R_ENTXCCK, 0xFFFF, WLG_LNA);
        break;
    case 2:
        reg |= B_WL_SWITCH | B_WLA_SWITCH;
        reg &= ~(B_BTG_SWITCH | B_CTRL_SWITCH | B_WLG_SWITCH);
        break;
    default:
        break;
    }
    wr32(R_RFECTL, reg);
}

/* rtw8821c_set_channel() for a 20 MHz channel: baseband, bb swing, MAC, RF, RX filter. ch_param are the three filter values that
 * rtw8821c_phy_set_param() reads back after the tables; bb_swing_* come from the efuse (0xC6 and 0xC7). */
static void set_channel_20(unsigned channel, const u32 ch_param[3], unsigned bb_swing_2g, unsigned bb_swing_5g)
{
    /* rtw8821c_set_channel_bb */
    if (channel <= 14)
    {
        mask32(R_RXPSEL, BIT(28), 1);
        mask32(R_CCK_CHECK, BIT(7), 0);
        mask32(R_ENTXCCK, BIT(18), 0);
        mask32(R_RXCCAMSK, 0x0000FC00, 15);
        mask32(R_TXSCALE_A, 0xF00, 0);
        mask32(R_CLKTRK, 0x1FFE0000, 0x96A);
        if (channel == 14) /* rtw_regd_srrc() is false here: the default CCK filter */
        {
            mask32(R_TXSF2, 0xFFFFFFFFu, 0x0000B81C);
            mask32(R_TXSF6, 0xFFFF, 0x0000);
            mask32(R_TXFILTER, 0xFFFFFFFFu, 0x00003667);
        }
        else
        {
            mask32(R_TXSF2, 0xFFFFFFFFu, ch_param[0]);
            mask32(R_TXSF6, 0xFFFF, ch_param[1] & 0xFFFF);
            mask32(R_TXFILTER, 0xFFFFFFFFu, ch_param[2]);
        }
    }
    else if (channel > 35)
    {
        mask32(R_ENTXCCK, BIT(18), 1);
        mask32(R_CCK_CHECK, BIT(7), 1);
        mask32(R_RXPSEL, BIT(28), 0);
        mask32(R_RXCCAMSK, 0x0000FC00, 15);
        if (channel >= 36 && channel <= 64)
            mask32(R_TXSCALE_A, 0xF00, 1);
        else if (channel >= 100 && channel <= 144)
            mask32(R_TXSCALE_A, 0xF00, 2);
        else if (channel >= 149)
            mask32(R_TXSCALE_A, 0xF00, 3);
        if (channel >= 36 && channel <= 48)
            mask32(R_CLKTRK, 0x1FFE0000, 0x494);
        else if (channel >= 52 && channel <= 64)
            mask32(R_CLKTRK, 0x1FFE0000, 0x453);
        else if (channel >= 100 && channel <= 116)
            mask32(R_CLKTRK, 0x1FFE0000, 0x452);
        else if (channel >= 118 && channel <= 177)
            mask32(R_CLKTRK, 0x1FFE0000, 0x412);
    }
    /* the 20 MHz case of the bandwidth setting */
    u32 val32 = rd32(R_ADCCLK);
    val32 &= 0xFFCFFC00u;
    val32 |= 0x10010000u;
    mask32(R_ADCCLK, 0xFFFFFFFFu, val32);
    mask32(R_ADC160, BIT(30), 1);

    /* rtw8821c_set_channel_bb_swing */
    static const u32 SWING2SETTING[4] = {0x200, 0x16A, 0x101, 0x0B6};
    unsigned swing = channel <= 14 ? bb_swing_2g : bb_swing_5g;
    if (swing > 9)
        swing = 0;
    mask32(R_TXSCALE_A, 0xFFE00000u, SWING2SETTING[swing / 3]);

    /* rtw_set_channel_mac */
    wr8(R_DATA_SC, 0); /* no secondary channels at 20 MHz */
    u32 v = rd32(R_WMAC_TRXPTCL_CTL);
    v &= ~0x180u; /* BIT_RFMOD: 20 MHz */
    wr32(R_WMAC_TRXPTCL_CTL, v);
    v = rd32(R_AFE_CTRL1) & ~0x300000u; /* BIT_MAC_CLK_SEL, set to MAC_CLK_HW_DEF_80M = 0 */
    wr32(R_AFE_CTRL1, v);
    wr8(R_USTIME_TSF, MAC_CLK_SPEED);
    wr8(R_USTIME_EDCA, MAC_CLK_SPEED);
    u8 value8 = (u8)(rd8(R_CCK_CHECK) & ~0x80);
    if (channel >= 36 && channel <= 177) /* IS_CH_5G_BAND */
        value8 |= 0x80;
    wr8(R_CCK_CHECK, value8);

    /* rtw8821c_set_channel_rf */
    u32 rf18 = rf_read(0x18);
    rf18 &= ~(RF18_BAND_MASK | RF18_CHANNEL_MASK | RF18_RFSI_MASK | RF18_BW_MASK);
    rf18 |= channel <= 14 ? RF18_BAND_2G : RF18_BAND_5G;
    rf18 |= channel & RF18_CHANNEL_MASK;
    if (channel >= 100 && channel <= 140)
        rf18 |= RF18_RFSI_GE;
    else if (channel > 140)
        rf18 |= RF18_RFSI_GT;
    rf18 |= RF18_BW_20M;
    if (channel <= 14)
    {
        switch_rf_set(CX.rfe_btg ? 0 : 1);
        rf_write_mask(RF_LUTDBG, BIT(6), 1);
        rf_write_mask(0x64, 0xF, 0xF);
    }
    else
    {
        switch_rf_set(2);
        rf_write_mask(RF_LUTDBG, BIT(6), 0);
    }
    rf_write_mask(0x18, RFREG_MASK, rf18);
    rf_write_mask(RF_XTALX2, BIT(19), 0);
    rf_write_mask(RF_XTALX2, BIT(19), 1);

    /* rtw8821c_set_channel_rxdfir, 20 MHz */
    mask32(R_ACBB0, BIT(29) | BIT(28), 0x2);
    mask32(R_ACBBRXFIR, BIT(29) | BIT(28), 0x2);
    mask32(R_TXDFIR, BIT(31), 0x1);
    mask32(R_CHFIR, BIT(31), 0x0);
}

enum { CH_NOT_TRIED, CH_OK, CH_FAILED };
static struct
{
    int state;
    unsigned channel;
    unsigned rf18; /* RF register 0x18 as read back */
} CHN;

/* tune to a channel (20 MHz) with the antenna given to Wi-Fi, and check by reading RF register 0x18 back */
static void channel_probe(unsigned channel)
{
    coex_wifi_only(EF.rfe);
    set_channel_20(channel, PH.ch_param, EF.swing2g, EF.swing5g);
    CHN.channel = channel;
    CHN.rf18 = rf_read(0x18);
    CHN.state = (CHN.rf18 & RF18_CHANNEL_MASK) == channel && (CHN.rf18 & RF18_BW_MASK) == RF18_BW_20M ? CH_OK : CH_FAILED;
}

/* ---- stage 4c-2: receive ----
 * The chip writes every frame it receives into the next buffer of the receive ring (the 32 descriptors set up in dma_setup(), each
 * pointing at its own 12 KiB buffer) and moves its write index, which the host can read at 0x3B4 (bits 27..16). The host reads the
 * frames from the buffers and gives the index it has reached back (bits 15..0), so that the chip may reuse the buffers behind it.
 * Each buffer holds a 24 byte receive descriptor, then 32 bytes of PHY status (signal strength etc., the "driver info" of
 * rtw_drv_info_cfg), then the 802.11 frame including its 4 byte FCS. Descriptor word 0: bits 13..0 frame length, 19..16 driver info
 * size in units of 8 bytes, 25..24 extra shift; word 2 bit 28 marks a message from the firmware (C2H) instead of a received frame.
 * What this stage does: switch the receiver on with Linux' default filter and count what arrives for a while, without answering
 * anything. The receive filter accepts beacons of every network (the "associated BSSID" check is off), so on a busy channel the beacons
 * of all access points around are received. */
#define R_RXBD_IDX_MPDUQ 0x03B4
#define RCR_RX_DEFAULT 0xF410400Eu /* hal->rcr: APP_FCS | APP_MIC | APP_ICV | PKTCTL_DLEN | HTC_LOC_CTRL | APP_PHYSTS | AB | AM | APM, + VHT_DACK */
#define RX_DESC_SIZE 24

enum { RXS_NOT_TRIED, RXS_OK };
static struct
{
    int state;
    unsigned rp;       /* the next buffer the host reads */
    unsigned frames;   /* frames received (not counting messages from the firmware) */
    unsigned beacons;
    unsigned probe_resp;
    unsigned mgmt_other, ctrl, data;
    unsigned c2h;      /* messages from the firmware */
    unsigned decrypted; /* protected data frames that the chip decrypted */
    unsigned icv_err;   /* protected data frames whose decryption failed (wrong or missing key) */
    unsigned bad;      /* descriptors that did not make sense, or frames with a bad checksum */
} RX;

static u32 vle32(const volatile u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static u32 rx_buf_dma(unsigned i) { return D.dma + (u32)(D.mem + Q_COUNT * PAGE + PAGE - D.mem) + i * RX_BUF_SLOT; }

static void scan_note(const volatile u8 *f, unsigned len);
static void conn_note(const volatile u8 *f, unsigned len);
static void eapol_note(const volatile u8 *f, unsigned len);
static int wifi_rx_data(const volatile u8 *f, unsigned len, u32 w0);
static void link_try(void);
static int link_is_up(void);
static void alive_note(const volatile u8 *f, unsigned len);

/* one received buffer: classify it. `b` is the buffer, as the chip filled it. */
static void rx_one(const volatile u8 *b)
{
    u32 w0 = vle32(b), w2 = vle32(b + 8);
    unsigned len = w0 & 0x3FFF, drv = ((w0 >> 16) & 0xF) * 8, shift = (w0 >> 24) & 3;
    int crc_err = (w0 >> 14) & 1;
    if (w2 & BIT(28)) /* C2H */
    {
        RX.c2h++;
        return;
    }
    if (len <= 4 || drv > 32 || RX_DESC_SIZE + drv + shift + len > RX_BUF_SIZE || crc_err)
    {
        RX.bad++;
        return;
    }
    const volatile u8 *f = b + RX_DESC_SIZE + drv + shift;
    unsigned fc0 = f[0], type = (fc0 >> 2) & 3, subtype = fc0 >> 4;
    RX.frames++;
    if (type != 1)
        alive_note(f, len - 4);
    if (type == 0)
    {
        if (subtype == 8)
            RX.beacons++;
        else if (subtype == 5)
            RX.probe_resp++;
        else
            RX.mgmt_other++;
        if (subtype == 11 || subtype == 1 || subtype == 12 || subtype == 10) /* authentication, association response, deauthentication, disassociation */
            conn_note(f, len - 4);
        if (subtype == 8 || subtype == 5)
            scan_note(f, len - 4); /* the frame without its FCS */
    }
    else if (type == 1)
        RX.ctrl++;
    else
    {
        RX.data++;
        if (f[1] & 0x40) /* the protected bit: the chip tried to decrypt (descriptor word 0: enc type bits 22..20, "software decrypt" bit 27, ICV error bit 15) */
        {
            if ((w0 >> 15) & 1)
                RX.icv_err++;
            else if (!((w0 >> 27) & 1) && ((w0 >> 20) & 7))
                RX.decrypted++;
        }
        if (type == 2 && !wifi_rx_data(f, len - 4, w0)) /* (once the link is up, the data path takes it) */
            eapol_note(f, len - 4); /* (only kept while the handshake runs) */
    }
}

/* take every buffer the chip has finished, re-arm it, tell the chip how far we are */
/* start counting afresh: the buffers the chip fills next are the ones after the last it filled (an earlier listening left its index where it was) */
static void rx_restart(void)
{
    RX.rp = (rd32(R_RXBD_IDX_MPDUQ) >> 16) & 0xFFF;
    if (RX.rp >= RX_COUNT)
        RX.rp = 0;
    RX.frames = RX.beacons = RX.probe_resp = RX.mgmt_other = RX.ctrl = RX.data = RX.c2h = RX.bad = RX.decrypted = RX.icv_err = 0;
}

static void rx_poll(void)
{
    unsigned hw = (rd32(R_RXBD_IDX_MPDUQ) >> 16) & 0xFFF;
    if (hw >= RX_COUNT)
        return; /* nonsense from the register: do nothing */
    unsigned n = hw >= RX.rp ? hw - RX.rp : RX_COUNT - (RX.rp - hw);
    if (!n)
        return;
    u8 *ring = D.mem + Q_COUNT * PAGE;
    u8 *bufs = ring + PAGE;
    while (n--)
    {
        rx_one(bufs + RX.rp * RX_BUF_SLOT);
        u8 *bd = ring + RX.rp * 8; /* give the buffer back: size, packet size 0, address */
        put16(bd, RX_BUF_SIZE);
        put16(bd + 2, 0);
        put32(bd + 4, rx_buf_dma(RX.rp));
        if (++RX.rp >= RX_COUNT)
            RX.rp = 0;
    }
    wr16(R_RXBD_IDX_MPDUQ, (u16)RX.rp);
}

/* ---- stage 4c-3: scan ----
 * Passive scan: tune to each channel in turn, listen a little longer than a beacon interval (access points send a beacon about every
 * 102 ms) and note every network heard. A beacon (or a probe response) is a management frame: 24 byte header (the sender's BSSID is the
 * third address), 12 bytes of fixed fields (timestamp, beacon interval, capability bits), then a list of information elements,
 * each tag (1 byte), length (1 byte), data. Element 0 is the network name (SSID), 3 the channel the access point is on (2.4 GHz),
 * 61 (HT operation) holds it for 5 GHz, 48 is the WPA2 (RSN) security description, a vendor element with 00:50:F2 type 1 is WPA. */
#define SCAN_DWELL_MS 300
#define SCAN_AP_MAX 32

typedef struct
{
    u8 bssid[6];
    char ssid[33];
    u8 channel;  /* the channel the access point says it is on, else the one we heard it on */
    u8 sec;      /* 0 open, 1 WEP (privacy bit only), 2 WPA, 3 WPA2 */
    u8 heard_on; /* the channel the scan was tuned to */
    u16 cap;     /* the capability bits of its beacon */
    u8 rsn_len;  /* the RSN element (WPA2 security description) without its tag and length; 0 = none or too long to keep */
    u8 rsn[40];
    unsigned beacons;
} ScanAp;

static const u8 SCAN_CHANNELS[] = {1, 6, 11, 36, 40, 44, 48, 149, 153, 157, 161};
#define SCAN_NCH ((unsigned)sizeof SCAN_CHANNELS)
static struct
{
    int state;
    ScanAp ap[SCAN_AP_MAX];
    unsigned n;
    unsigned per_channel[SCAN_NCH]; /* frames heard on each channel */
    unsigned channels_ok;           /* channels that could be tuned */
    unsigned cur_channel;           /* where the receiver listens now */
    unsigned dropped;               /* networks that did not fit the table */
    unsigned beacons;               /* beacons heard during the whole scan */
} SC;


/* one management frame (without its FCS): remember the network it belongs to */
static void scan_note(const volatile u8 *f, unsigned len)
{
    if (len < 36 + 2)
        return;
    unsigned fc0 = f[0];
    if ((fc0 & 0xFC) != 0x80 && (fc0 & 0xFC) != 0x50) /* beacon or probe response */
        return;
    u8 bssid[6];
    for (int i = 0; i < 6; i++)
        bssid[i] = f[16 + i];
    unsigned cap = (unsigned)f[34] | (unsigned)f[35] << 8;
    char ssid[33];
    unsigned ssid_len = 0;
    int have_ssid = 0, named = 0;
    unsigned chan = 0, sec = (cap & 0x10) ? 1 : 0;
    u8 rsn[40];
    unsigned rsn_len = 0;
    for (unsigned p = 36; p + 2 <= len;)
    {
        unsigned tag = f[p], tl = f[p + 1];
        if (p + 2 + tl > len)
            break; /* an element that runs past the frame: stop reading */
        const volatile u8 *d = f + p + 2;
        if (tag == 0 && !have_ssid && tl <= 32)
        {
            for (unsigned i = 0; i < tl; i++)
            {
                unsigned c = d[i];
                ssid[i] = c >= 32 && c < 127 ? (char)c : '?';
                if (c)
                    named = 1;
            }
            ssid_len = named ? tl : 0; /* a hidden network sends an empty name or only zeros */

            have_ssid = 1;
        }
        else if (tag == 3 && tl >= 1)
            chan = d[0];
        else if (tag == 61 && tl >= 1 && !chan)
            chan = d[0];
        else if (tag == 48)
        {
            sec = 3;
            rsn_len = tl <= sizeof rsn ? tl : 0;
            for (unsigned i = 0; i < rsn_len; i++)
                rsn[i] = d[i];
        }
        else if (tag == 221 && tl >= 4 && d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xF2 && d[3] == 1 && sec < 2)
            sec = 2;
        p += 2 + tl;
    }
    if (!have_ssid)
        return;
    ssid[ssid_len] = 0;
    for (unsigned i = 0; i < SC.n; i++)
    {
        ScanAp *a = &SC.ap[i];
        int same = 1;
        for (int k = 0; k < 6; k++)
            if (a->bssid[k] != bssid[k])
                same = 0;
        if (same)
        {
            a->beacons++;
            return;
        }
    }
    if (SC.n >= SCAN_AP_MAX)
    {
        SC.dropped++;
        return;
    }
    ScanAp *a = &SC.ap[SC.n++];
    for (int k = 0; k < 6; k++)
        a->bssid[k] = bssid[k];
    for (unsigned i = 0; i <= ssid_len; i++)
        a->ssid[i] = ssid[i];
    a->channel = (u8)(chan ? chan : SC.cur_channel);
    a->sec = (u8)sec;
    a->heard_on = (u8)SC.cur_channel;
    a->beacons = 1;
    a->cap = (u16)cap;
    a->rsn_len = (u8)rsn_len;
    for (unsigned i = 0; i < rsn_len; i++)
        a->rsn[i] = rsn[i];
}

/* the whole scan: for each channel, switch, listen, collect. The receiver stays on all the time. */
static void scan_run(void)
{
    SC.n = SC.dropped = SC.channels_ok = 0;
    for (unsigned i = 0; i < SCAN_NCH; i++)
        SC.per_channel[i] = 0;
    SC.state = 1;
    rx_restart();
    wr32(R_RCR, RCR_RX_DEFAULT);
    wr8(R_CR, MAC_TRX_ENABLE);
    for (unsigned i = 0; i < SCAN_NCH; i++)
    {
        SC.cur_channel = SCAN_CHANNELS[i];
        set_channel_20(SC.cur_channel, PH.ch_param, EF.swing2g, EF.swing5g);
        if ((rf_read(0x18) & RF18_CHANNEL_MASK) != SC.cur_channel) /* the synthesizer did not take the channel */
            continue;
        SC.channels_ok++;
        unsigned before = RX.frames;
        for (unsigned ms = 0; ms < SCAN_DWELL_MS; ms++)
        {
            udelay(1000);
            rx_poll();
        }
        SC.per_channel[i] = RX.frames - before;
    }
    SC.beacons = RX.beacons;
    RX.state = RXS_OK;
}

/* The result of the last scan as lines of text, for the SYS_WIFISCAN system call (net.h: net_wifi_scan_line). Line 0 is a summary, lines
 * 1 .. n one network each, line n + 1 the frames heard on every channel. Returns the length of the line, or -1 if there is no such line
 * (no scan yet, or past the end). */
static const char *const SEC_NAME[] = {"open", "WEP", "WPA", "WPA2"};

static void put_pad(NicText *t, const char *s, unsigned width)
{
    unsigned n = 0;
    while (s[n])
        n++;
    nic_puts(t, s);
    for (; n < width; n++)
        nic_putc(t, ' ');
}

int rtl8821ce_scan_line(unsigned index, char *buf, unsigned cap)
{
    if (!SC.state || !buf || cap < 2)
        return -1;
    NicText t;
    nic_text_init(&t, buf, cap);
    if (index == 0)
    {
        unsigned f2 = 0, f5 = 0;
        for (unsigned i = 0; i < SCAN_NCH; i++)
            if (SCAN_CHANNELS[i] <= 14)
                f2 += SC.per_channel[i];
            else
                f5 += SC.per_channel[i];
        nic_putd(&t, SC.n);
        nic_puts(&t, " networks, ");
        nic_putd(&t, SC.channels_ok);
        nic_putc(&t, '/');
        nic_putd(&t, SCAN_NCH);
        nic_puts(&t, " channels tuned, frames 2.4G ");
        nic_putd(&t, f2);
        nic_puts(&t, " 5G ");
        nic_putd(&t, f5);
        nic_puts(&t, ", beacons ");
        nic_putd(&t, SC.beacons);
        if (SC.dropped)
        {
            nic_puts(&t, ", not listed ");
            nic_putd(&t, SC.dropped);
        }
    }
    else if (index <= SC.n)
    {
        const ScanAp *a = &SC.ap[index - 1];
        nic_puts(&t, "ch ");
        if (a->channel < 100)
            nic_putc(&t, ' ');
        if (a->channel < 10)
            nic_putc(&t, ' ');
        nic_putd(&t, a->channel);
        nic_puts(&t, "  ");
        put_pad(&t, SEC_NAME[a->sec & 3], 4);
        nic_puts(&t, "  ");
        for (int i = 0; i < 6; i++)
        {
            nic_puth(&t, a->bssid[i], 2);
            if (i < 5)
                nic_putc(&t, ':');
        }
        nic_puts(&t, "  ");
        nic_puts(&t, a->ssid[0] ? a->ssid : "(hidden)");
        nic_puts(&t, "  x");
        nic_putd(&t, a->beacons);
    }
    else if (index == SC.n + 1)
    {
        nic_puts(&t, "frames per channel:");
        for (unsigned i = 0; i < SCAN_NCH; i++)
        {
            nic_putc(&t, ' ');
            nic_putd(&t, SCAN_CHANNELS[i]);
            nic_putc(&t, ':');
            nic_putd(&t, SC.per_channel[i]);
        }
    }
    else
        return -1;
    return (int)t.n;
}

/* ---- stage 5a-1: transmit power ----
 * Before the first frame goes out, the chip must be told how strongly to send each rate. The number is an index (about half a decibel
 * per step; a rule of Realtek's chips, not something this driver checks) and it is made of three parts, as in Linux' phy.c:
 *   - the base from the efuse: the factory calibrated value for the channel group (and the OFDM or CCK offset of that group);
 *   - the "power by rate" table: how much weaker the higher rates are sent, relative to the last rate of the section (54M, 11M);
 *   - the regulatory limit (include/rtl8821ce_txpwr.h): per country group and channel. The country of this PC is not known to the
 *     driver, so it uses the world-wide row: the lowest limit of every country group that has one for the channel.
 * The index of a rate is  base + min(by-rate offset, limit, no-limit) , at most 0x3F; the chip takes four rates per 32-bit register
 * (0x1D00 + rate / 4 * 4), one byte per rate. Only the rates of the CCK (2.4 GHz) and OFDM sections at 20 MHz are computed: they are
 * the ones used for management frames. */
#include "rtl8821ce_txpwr.h"

#define TXPWR_MAX_INDEX 0x3F
#define R_TXAGC_A 0x1D00
#define RATE_CCK_FIRST 0
#define RATE_OFDM_FIRST 4
#define RATE_OFDM_LAST 11
#define RS_CCK 0
#define RS_OFDM 1

static const u8 CH5G[49] = {36,  38,  40,  42,  44,  46,  48,  52,  54,  56,  58,  60,  62,  64,  100, 102, 104,
                            106, 108, 110, 112, 116, 118, 120, 122, 124, 126, 128, 132, 134, 136, 138, 140, 142,
                            144, 149, 151, 153, 155, 157, 159, 161, 165, 167, 169, 171, 173, 175, 177};

static int sext4(unsigned v) { return (v & 8) ? (int)(v & 0xF) - 16 : (int)(v & 0xF); }

/* rtw_get_channel_group: which of the efuse's base values a channel uses */
static int txpwr_group(unsigned channel, int cck)
{
    switch (channel)
    {
    case 1:
    case 2:
    case 36:
    case 38:
    case 40:
    case 42:
        return 0;
    case 3:
    case 4:
    case 5:
    case 44:
    case 46:
    case 48:
    case 50:
        return 1;
    case 6:
    case 7:
    case 8:
    case 52:
    case 54:
    case 56:
    case 58:
        return 2;
    case 9:
    case 10:
    case 11:
    case 60:
    case 62:
    case 64:
        return 3;
    case 12:
    case 13:
    case 100:
    case 102:
    case 104:
    case 106:
        return 4;
    case 14:
        return cck ? 5 : 4;
    case 108:
    case 110:
    case 112:
    case 114:
        return 5;
    case 116:
    case 118:
    case 120:
    case 122:
        return 6;
    case 124:
    case 126:
    case 128:
    case 130:
        return 7;
    case 132:
    case 134:
    case 136:
    case 138:
        return 8;
    case 140:
    case 142:
    case 144:
        return 9;
    case 149:
    case 151:
    case 153:
    case 155:
        return 10;
    case 157:
    case 159:
    case 161:
        return 11;
    case 165:
    case 167:
    case 169:
    case 171:
        return 12;
    case 173:
    case 175:
    case 177:
        return 13;
    default:
        return 0; /* Linux warns and uses group 0 */
    }
}

/* the base index of the efuse, path A (rtw_phy_get_2g_tx_power_index / rtw_phy_get_5g_tx_power_index for a CCK or OFDM rate) */
static u8 txpwr_base(const u8 *txpwr, unsigned channel, unsigned rate)
{
    int cck = rate <= 3;
    unsigned group = (unsigned)txpwr_group(channel, cck);
    if (channel <= 14)
    {
        if (cck)
            return txpwr[group];                                       /* cck_base[group] */
        return (u8)(txpwr[6 + group] + sext4(txpwr[11]));              /* bw40_base[group] + the OFDM difference (low nibble of ht_1s_diff) */
    }
    return (u8)(txpwr[18 + group] + sext4(txpwr[18 + 14]));            /* the 5 GHz block starts at 18: bw40_base[14], then ht_1s_diff */
}

/* one value of the "power by rate" table: four rates per register in the BCD-like form 0x32343638 = 32, 34, 36, 38; 0 if the table has none */
static int txpwr_pg(unsigned band, unsigned rate)
{
    unsigned reg = 0xC20 + (rate / 4) * 4;
    for (unsigned i = 0; i < sizeof rtl8821ce_tab_pg / sizeof rtl8821ce_tab_pg[0]; i++)
        if (rtl8821ce_tab_pg[i][0] == band && rtl8821ce_tab_pg[i][1] == 0 && rtl8821ce_tab_pg[i][3] == reg)
        {
            unsigned b = (rtl8821ce_tab_pg[i][5] >> ((rate % 4) * 8)) & 0xFF; /* bcd2bin of one byte */
            return (int)((b >> 4) * 10 + (b & 0xF));
        }
    return 0;
}

/* the world-wide limit: the lowest of the entries of all country groups for this band, 20 MHz, rate section and channel */
static int txpwr_limit_ww(unsigned band, unsigned channel, unsigned rs)
{
    int lim = TXPWR_MAX_INDEX;
    for (unsigned i = 0; i < sizeof rtl8821ce_tab_txpwr_lmt / sizeof rtl8821ce_tab_txpwr_lmt[0]; i++)
    {
        const short *e = rtl8821ce_tab_txpwr_lmt[i];
        if ((unsigned)e[1] == band && e[2] == 0 && (unsigned)e[3] == rs && (unsigned)e[4] == channel)
        {
            int v = e[5] > TXPWR_MAX_INDEX ? TXPWR_MAX_INDEX : e[5] < -TXPWR_MAX_INDEX ? -TXPWR_MAX_INDEX : e[5];
            if (v < lim)
                lim = v;
        }
    }
    return lim;
}

static int txpwr_channel_known(unsigned channel)
{
    if (channel >= 1 && channel <= 14)
        return 1;
    for (unsigned i = 0; i < sizeof CH5G; i++)
        if (CH5G[i] == channel)
            return 1;
    return 0;
}

/* the power index of one CCK / OFDM rate at 20 MHz for path A: rtw_phy_get_tx_power_index with the world-wide limit and no SAR limit */
static u8 tx_power_index(const u8 *txpwr, unsigned channel, unsigned rate)
{
    unsigned band = channel <= 14 ? 0 : 1;
    unsigned rs = rate <= 3 ? RS_CCK : RS_OFDM;
    unsigned base_rate = rs == RS_CCK ? 3 : RATE_OFDM_LAST; /* the last rate of the section: the one all offsets are relative to */
    int base_pg = txpwr_pg(band, base_rate);
    int offset = txpwr_pg(band, rate) - base_pg;
    int limit = txpwr_channel_known(channel) ? txpwr_limit_ww(band, channel, rs) - base_pg : TXPWR_MAX_INDEX; /* an unknown channel: Linux gives up on the limit */
    int m = offset < limit ? offset : limit;
    if (m > TXPWR_MAX_INDEX)
        m = TXPWR_MAX_INDEX;
    u8 p = (u8)(txpwr_base(txpwr, channel, rate) + m); /* an unsigned byte, as in Linux: a negative sum becomes a large number and is capped */
    return p > TXPWR_MAX_INDEX ? TXPWR_MAX_INDEX : p;
}

/* write the indices of the rates used for management frames: CCK on 2.4 GHz, OFDM on both bands (4 rates per register, byte = rate % 4) */
static void tx_power_apply(const u8 *txpwr, unsigned channel)
{
    unsigned first = channel <= 14 ? RATE_CCK_FIRST : RATE_OFDM_FIRST;
    u32 word = 0;
    for (unsigned rate = first; rate <= RATE_OFDM_LAST; rate++)
    {
        word |= (u32)tx_power_index(txpwr, channel, rate) << ((rate & 3) * 8);
        if ((rate & 3) == 3)
        {
            wr32(R_TXAGC_A + (rate & 0xFC), word);
            word = 0;
        }
    }
}

/* ---- stage 5a-2: the first frame on the air ----
 * A management frame goes to the chip through the management queue's ring: one buffer descriptor (16 bytes) = two entries, the first
 * pointing at the 48 byte transmit descriptor (what to do with the frame: length, queue, rate, ...), the second at the frame itself.
 * The host writes the index of the next free entry to 0x3B0; the chip reads the entry, fetches both pieces by DMA, sends the frame and
 * moves its own read index (the upper half of the same register). The frame is given without its 4 byte FCS: the chip adds it. The
 * sequence number is filled in by the hardware (EN_HWSEQ), and the rate is fixed (no rate adaptation: USE_RATE, no fallback).
 * Used here for a probe request: "is there a network around? answer me" sent to everyone (broadcast), which every phone and laptop does
 * when it looks for networks. The access points answer with probe responses, which only reach us if the chip knows its own address
 * (written to 0x610), and which the scan then lists like beacons. */
#define R_MACID 0x0610
#define R_TXBD_IDX_MGMTQ 0x03B0
#define TX_QSEL_MGMT 18
#define TX_RATEID_G 7
#define TX_RATEID_B_20M 8
#define RATE_6M 4
#define RATE_1M 0
#define MGMT_RING_LEN 128

/* the host tests of the earlier stages switch the probe-request test off (it resets the receive counters they look at) */
static int TXP_ENABLE = 1;

static struct
{
    int state;       /* 0 not tried, 1 done */
    unsigned wp;     /* the next free entry of the management ring */
    unsigned sent;   /* frames the chip took */
    unsigned failed; /* frames the chip did not take within the time allowed */
    unsigned replies; /* probe responses heard while it ran */
    int variant;                /* which way of starting the transmit DMA worked first (0 = the plain one, -1 = none did) */
    int kick32;                 /* variant 4: the index is written as a 32-bit register */
    unsigned exq_before, exq_after, pub_before, pub_after; /* free pages of the extra queue (the one the management queue is mapped to) and of the public pool, before the first frame and after the last try */
    unsigned channel;
} TXP;

/* one frame (without FCS, at most 1500 bytes) into the management queue and wait until the chip has fetched it: 0, or -1 */
static int tx_mgmt(const u8 *frame, unsigned len, unsigned channel)
{
    if (!D.ready || !D.stage || len < 24 || len > 1500)
        return -1;
    unsigned rp = (rd32(R_TXBD_IDX_MGMTQ) >> 16) & 0xFFF;
    if ((TXP.wp + 1) % MGMT_RING_LEN == rp) /* the ring is full */
        return -1;
    volatile u8 *s = D.stage;
    u8 *d = D.stage;
    zero_bytes(d, TX_DESC_SIZE);
    for (unsigned i = 0; i < len; i++)
        s[TX_DESC_SIZE + i] = frame[i];
    unsigned bmc = frame[4] & 1; /* the destination address is a group address */
    put32(d + 0, len | (TX_DESC_SIZE << 16) | (bmc << 24) | BIT(26) | BIT(31)); /* size, offset to the frame, bmc, last segment, no queue/sequence select */
    put32(d + 4, (unsigned)TX_QSEL_MGMT << 8 | (unsigned)(channel <= 14 ? TX_RATEID_B_20M : TX_RATEID_G) << 16); /* MAC id 0, queue, rate id */
    put32(d + 12, BIT(8) | BIT(10)); /* use the rate of this descriptor, no rate fallback */
    put32(d + 16, channel <= 14 ? RATE_1M : RATE_6M);
    put32(d + 32, BIT(15)); /* hardware sequence number */

    u8 *bd = D.mem + Q_MGMT * PAGE + TXP.wp * 16;
    zero_bytes(bd, 16);
    put16(bd + 0, TX_DESC_SIZE);
    put16(bd + 2, (TX_DESC_SIZE + len - 1) / 128 + 1); /* the length in 128 byte units, no OWN bit */
    put32(bd + 4, D.stage_dma);
    put16(bd + 8, len);
    put16(bd + 10, 0);
    put32(bd + 12, D.stage_dma + TX_DESC_SIZE);
    fence();
    unsigned next = (TXP.wp + 1) % MGMT_RING_LEN;
    if (TXP.kick32)
        wr32(R_TXBD_IDX_MGMTQ, next);
    else
        wr16(R_TXBD_IDX_MGMTQ, (u16)next);
    TXP.wp = next;
    for (int i = 0; i < 50000; i++) /* up to 50 ms for the chip to fetch it */
    {
        if (((rd32(R_TXBD_IDX_MGMTQ) >> 16) & 0xFFF) == next)
            return 0;
        udelay(1);
    }
    return -1;
}

/* a probe request for any network (empty SSID), asking with the basic rates of the band; returns its length */
static unsigned build_probe_request(u8 *f, const u8 *mac, unsigned channel)
{
    unsigned n = 0;
    f[n++] = 0x40; /* frame control: management, probe request */
    f[n++] = 0x00;
    f[n++] = 0;    /* duration */
    f[n++] = 0;
    for (int i = 0; i < 6; i++) /* addr1: everybody */
        f[n++] = 0xFF;
    for (int i = 0; i < 6; i++) /* addr2: us */
        f[n++] = mac[i];
    for (int i = 0; i < 6; i++) /* addr3: any BSSID */
        f[n++] = 0xFF;
    f[n++] = 0; /* sequence control: filled in by the hardware */
    f[n++] = 0;
    f[n++] = 0; /* SSID element: empty = any network */
    f[n++] = 0;
    f[n++] = 1; /* supported rates */
    if (channel <= 14)
    {
        static const u8 R2[8] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24}; /* 1, 2, 5.5, 11 (basic), 6, 9, 12, 18 Mb/s */
        f[n++] = 8;
        for (int i = 0; i < 8; i++)
            f[n++] = R2[i];
        f[n++] = 50; /* extended rates: 24, 36, 48, 54 */
        f[n++] = 4;
        f[n++] = 0x30;
        f[n++] = 0x48;
        f[n++] = 0x60;
        f[n++] = 0x6C;
    }
    else
    {
        static const u8 R5[8] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C}; /* 6 (basic), 9, 12 (basic), 18, 24 (basic), 36, 48, 54 Mb/s */
        f[n++] = 8;
        for (int i = 0; i < 8; i++)
            f[n++] = R5[i];
    }
    return n;
}

/* tune to a channel with its transmit power, and make the chip answer to our address */
static void tune_for_tx(unsigned channel)
{
    set_channel_20(channel, PH.ch_param, EF.swing2g, EF.swing5g);
    tx_power_apply(EF.txpwr, channel);
    for (int i = 0; i < 6; i++)
        wr8(R_MACID + (u32)i, EF.mac[i]);
}

/* The first real card did not take a frame from the ring with the plain start (Linux' own sequence as far as it was ported). So the
 * test tries, one frame at a time, a few ways of starting the transmit DMA, each one adding to the last, and remembers the first that
 * worked. 0: the plain start. 1: the DMA interface of the PCI block is reset once more (bit 20 of the PCI control register off and on).
 * 2: the transmit pause register and the "drop / sleep" registers of the MAC ids are cleared. 3: all eight enable bits of the top
 * byte of the PCI control register (Linux sets 0xF7). 4: the ring index is written as a 32-bit register. */
#define TX_VARIANTS 5

static void tx_variant_setup(int v)
{
    wr16(0x0380, MGMT_RING_LEN); /* the ring, as hci_setup() wrote it */
    wr32(0x0310, D.dma + Q_MGMT * PAGE);
    wr32(R_TXBD_RWPTR_CLR, 0xFFFFFFFFu);
    TXP.wp = 0;
    TXP.kick32 = 0;
    if (v >= 1)
    {
        u32 p = rd32(R_PCI_CTRL);
        wr32(R_PCI_CTRL, p & ~BIT(20));
        udelay(100);
        wr32(R_PCI_CTRL, p | BIT(20));
        udelay(100);
    }
    if (v >= 2)
    {
        wr16(R_TXPAUSE, 0);
        wr32(0x04D0, 0); /* REG_MACID_PKT_DROP0 */
        wr32(0x04D4, 0); /* REG_MACID_PKT_SLEEP */
    }
    if (v >= 3)
        wr8(R_PCI_CTRL + 3, (u8)(rd8(R_PCI_CTRL + 3) | 0xFF));
    if (v >= 4)
        TXP.kick32 = 1;
}

/* listen for `ms` milliseconds, taking in what arrives */
static void listen_ms(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++)
    {
        udelay(1000);
        rx_poll();
    }
}

/* ask for probe responses three times on a channel, listening 100 ms after each, and count the answers. A way of starting the transmit
 * DMA counts as working if the chip says it fetched the frame (its read index moved) OR an access point answered: on the first real
 * card the index never moved, and it was not known whether the frame went out all the same. */
static void tx_probe_test(unsigned channel)
{
    TXP.state = 1;
    TXP.channel = channel;
    TXP.sent = TXP.failed = 0;
    TXP.variant = -1;
    TXP.kick32 = 0;
    TXP.exq_before = rd16(0x023E); /* REG_FIFOPAGE_INFO_4 + 2: free pages of the extra queue */
    TXP.pub_before = rd16(0x0242); /* REG_FIFOPAGE_INFO_5 + 2: free pages of the public pool */
    if (!D.ready || EF.state != EF_OK)
        return;
    tune_for_tx(channel);
    rx_restart();
    wr32(R_RCR, RCR_RX_DEFAULT);
    wr8(R_CR, MAC_TRX_ENABLE);
    u8 frame[96];
    unsigned len = build_probe_request(frame, EF.mac, channel);
    for (int v = 0; v < TX_VARIANTS && TXP.variant < 0; v++) /* the first frame decides how the DMA is started */
    {
        tx_variant_setup(v);
        unsigned answers = RX.probe_resp;
        int fetched = tx_mgmt(frame, len, channel) == 0;
        listen_ms(100);
        if (fetched || RX.probe_resp > answers)
        {
            TXP.variant = v;
            TXP.sent++;
        }
    }
    if (TXP.variant < 0)
    {
        TXP.failed = 3; /* no way worked: do not try more */
        TXP.exq_after = rd16(0x023E);
        TXP.pub_after = rd16(0x0242);
    }
    else
        for (int k = 1; k < 3; k++)
        {
            if (tx_mgmt(frame, len, channel) == 0)
                TXP.sent++;
            else
                TXP.failed++;
            listen_ms(100);
        }
    TXP.replies = RX.probe_resp;
}



/* ---- stage 5b-0: which network to join ----
 * WIFI.CFG sits in the top folder of the boot disk (pconfig/WIFI.CFG in the repository, which git ignores): two lines, "ssid=NAME" and
 * "psk=PASSPHRASE". The passphrase is a secret: it is kept in this module's memory for the key derivation of the WPA2 handshake, and
 * nothing here prints it, logs it, or puts a part of it into a status line or a test message. */
#define CFG_NAME "WIFI.CFG"
#define CFG_FILE_MAX 512

enum { CFG_NOT_TRIED, CFG_OK, CFG_NONE, CFG_BAD };
static struct
{
    int state;
    const char *why;   /* CFG_BAD: "ssid" or "psk" (which line is wrong), static text */
    char ssid[33];
    unsigned ssid_len;
    char psk[64];      /* the passphrase, 8 .. 63 characters */
    unsigned psk_len;
} CFG;

/* the text between the end of the key and the end of the line, without a trailing carriage return or spaces; -1 if the line is not `key` */
static int cfg_value(const char *line, unsigned n, const char *key, const char **value)
{
    unsigned k = 0;
    while (key[k])
        k++;
    if (n < k + 1)
        return -1;
    for (unsigned i = 0; i < k; i++)
        if (line[i] != key[i])
            return -1;
    if (line[k] != '=')
        return -1;
    *value = line + k + 1;
    unsigned len = n - k - 1;
    while (len && (line[k + len] == '\r' || line[k + len] == ' ' || line[k + len] == '\t'))
        len--;
    return (int)len;
}

/* the lines of the file (size bytes in buf) into CFG; sets CFG.state */
static void cfg_parse(const char *buf, unsigned size)
{
    CFG.state = CFG_NONE;
    int got_ssid = 0, got_psk = 0;
    unsigned start = 0;
    for (unsigned i = 0; i <= size; i++)
    {
        if (i < size && buf[i] != '\n')
            continue;
        const char *value = 0;
        int len = cfg_value(buf + start, i - start, "ssid", &value);
        if (len >= 0)
        {
            got_ssid = 1;
            if (len < 1 || len > 32)
            {
                CFG.state = CFG_BAD;
                CFG.why = "ssid";
                return;
            }
            for (int k = 0; k < len; k++)
                CFG.ssid[k] = value[k];
            CFG.ssid[len] = 0;
            CFG.ssid_len = (unsigned)len;
        }
        len = cfg_value(buf + start, i - start, "psk", &value);
        if (len >= 0)
        {
            got_psk = 1;
            if (len < 8 || len > 63)
            {
                CFG.state = CFG_BAD;
                CFG.why = "psk";
                return;
            }
            for (int k = 0; k < len; k++)
                CFG.psk[k] = value[k];
            CFG.psk[len] = 0;
            CFG.psk_len = (unsigned)len;
        }
        start = i + 1;
    }
    if (!got_ssid)
    {
        CFG.state = CFG_BAD;
        CFG.why = "ssid";
        return;
    }
    if (!got_psk)
    {
        CFG.state = CFG_BAD;
        CFG.why = "psk";
        return;
    }
    CFG.state = CFG_OK;
}

/* the access point of the wanted network in the scan list: the one with the most beacons heard that has this exact name and WPA2 */
static struct
{
    int found;
    unsigned index;
} TARGET;

static void pick_target(void)
{
    TARGET.found = 0;
    if (CFG.state != CFG_OK || !SC.state)
        return;
    for (unsigned i = 0; i < SC.n; i++)
    {
        const ScanAp *a = &SC.ap[i];
        if (a->sec != 3 || a->ssid[0] == 0)
            continue;
        unsigned k = 0;
        while (k < CFG.ssid_len && a->ssid[k] == CFG.ssid[k])
            k++;
        if (k != CFG.ssid_len || a->ssid[k] != 0)
            continue;
        if (!TARGET.found || a->beacons > SC.ap[TARGET.index].beacons)
        {
            TARGET.found = 1;
            TARGET.index = i;
        }
    }
}

static void cfg_load(void)
{
    CFG.state = CFG_NONE;
    CFG.why = 0;
    CFG.ssid_len = CFG.psk_len = 0;
    volatile char *wipe = CFG.psk;
    for (unsigned i = 0; i < sizeof CFG.psk; i++)
        wipe[i] = 0;
    int node = exec_find(CFG_NAME);
    if (node < 0)
        return;
    const VfsNode *vn = vfs_node(node);
    if (!vn || vn->is_dir || vn->size == 0 || vn->size > CFG_FILE_MAX)
        return;
    char buf[CFG_FILE_MAX];
    if (vfs_read(node, 0, buf, vn->size) == (int)vn->size)
        cfg_parse(buf, vn->size);
    volatile char *wipe_buf = buf; /* the file's text does not stay on the stack */
    for (unsigned i = 0; i < sizeof buf; i++)
        wipe_buf[i] = 0;
}

/* ---- stage 5b-1: authentication and association ----
 * Joining a network takes two short exchanges of management frames with the access point (AP), both unicast to its BSSID:
 *   1. authentication: we send "algorithm 0 (open system), sequence 1, status 0"; it answers "algorithm 0, sequence 2, status 0 = fine".
 *      (Open system is only a formality; the real proof of the passphrase comes later, in the WPA2 handshake.)
 *   2. association: we send who we are (capability bits, listen interval, network name, rates, and in the RSN element the WPA2 security
 *      we want: group and pairwise cipher CCMP (AES), key management PSK); it answers with a status (0 = fine) and the association id.
 * Every frame a station receives that is addressed to its own MAC address is acknowledged by the chip itself, and our frames are
 * retried by the chip until the AP acknowledges. After association the AP expects the WPA2 handshake (the next stage); this stage
 * ends by telling the AP that we are leaving (deauthentication), so that no half-joined station is left behind.
 * Only 5 GHz and legacy OFDM rates are offered for now: no HT / VHT elements, so the link would be 54 Mb/s at best. */
#define CAP_ESS 0x0001
#define CAP_PRIVACY 0x0010

enum { RSN_OK, RSN_BAD_VERSION, RSN_NO_CCMP, RSN_NO_PSK, RSN_PMF_REQUIRED, RSN_GROUP };

/* Can we join with the security the RSN element of this access point describes? The element: version (2), group cipher suite (4),
 * pairwise count (2) and suites (4 each), key management count (2) and suites (4 each), capabilities (2). A suite is the OUI 00-0F-AC
 * and a number: ciphers 2 = TKIP, 4 = CCMP; key management 2 = PSK (WPA2-Personal), 8 = SAE (WPA3). Capability bit 6 = management
 * frame protection REQUIRED, which needs a key exchange this driver does not have. */
static int rsn_check(const u8 *e, unsigned len)
{
    if (len < 8 || le16(e) != 1)
        return RSN_BAD_VERSION;
    unsigned p = 2;
    int group_ccmp = e[p] == 0x00 && e[p + 1] == 0x0F && e[p + 2] == 0xAC && e[p + 3] == 4;
    p += 4;
    unsigned n = le16(e + p);
    p += 2;
    int ccmp = 0;
    for (unsigned i = 0; i < n; i++, p += 4)
    {
        if (p + 4 > len)
            return RSN_BAD_VERSION;
        if (e[p] == 0x00 && e[p + 1] == 0x0F && e[p + 2] == 0xAC && e[p + 3] == 4)
            ccmp = 1;
    }
    if (p + 2 > len)
        return RSN_BAD_VERSION;
    n = le16(e + p);
    p += 2;
    int psk = 0;
    for (unsigned i = 0; i < n; i++, p += 4)
    {
        if (p + 4 > len)
            return RSN_BAD_VERSION;
        if (e[p] == 0x00 && e[p + 1] == 0x0F && e[p + 2] == 0xAC && e[p + 3] == 2)
            psk = 1;
    }
    unsigned caps = p + 2 <= len ? le16(e + p) : 0;
    if (!ccmp)
        return RSN_NO_CCMP;
    if (!psk)
        return RSN_NO_PSK;
    if (caps & 0x0040)
        return RSN_PMF_REQUIRED;
    if (!group_ccmp)
        return RSN_GROUP;
    return RSN_OK;
}

static const char *const RSN_WHY[] = {"ok", "rsn", "ccmp", "akm", "pmf", "grp"};

static struct
{
    int state;          /* 0 not tried, 1 tried */
    u8 bssid[6];
    unsigned channel;
    int auth_seen;      /* an authentication answer arrived */
    unsigned auth_status;
    int assoc_seen;
    unsigned assoc_status;
    unsigned aid;
    unsigned dropped;   /* the AP sent a deauthentication or disassociation: its reason code + 1 (0 = none) */
    unsigned auth_tries, assoc_tries;
} CONN;

static int mac_is(const volatile u8 *a, const u8 *b)
{
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static unsigned vget16(const volatile u8 *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

/* a management frame from the receive path (without FCS): is it an answer from the access point we talk to? */
static void conn_note(const volatile u8 *f, unsigned len)
{
    unsigned fc0 = f[0];
    /* (an access point that goes down says goodbye to everybody: a deauthentication / disassociation may be addressed to a group address) */
    int to_us = len >= 10 && mac_is(f + 4, EF.mac);
    int goodbye = len >= 10 && (f[4] & 1) && (fc0 == 0xC0 || fc0 == 0xA0);
    if (!CONN.state || len < 26 || (!to_us && !goodbye) || !mac_is(f + 10, CONN.bssid))
        return;
    if (fc0 == 0xB0 && len >= 30 && vget16(f + 24) == 0 && vget16(f + 26) == 2) /* authentication: open system, second message */
    {
        CONN.auth_status = vget16(f + 28);
        CONN.auth_seen = 1;
    }
    else if (fc0 == 0x10 && len >= 30) /* association response: status, then the association id (the top two bits are always set) */
    {
        CONN.assoc_status = vget16(f + 26);
        CONN.aid = len >= 30 ? vget16(f + 28) & 0x3FFF : 0;
        CONN.assoc_seen = 1;
    }
    else if (fc0 == 0xC0 || fc0 == 0xA0) /* deauthentication / disassociation: the reason code */
        CONN.dropped = vget16(f + 24) + 1;
}

/* the 24 byte management header to a unicast address: frame control, duration 0, receiver, us, BSSID, sequence (filled in by the chip) */
static unsigned mgmt_header(u8 *f, unsigned fc0, const u8 *bssid)
{
    unsigned n = 0;
    f[n++] = (u8)fc0;
    f[n++] = 0x00;
    f[n++] = 0;
    f[n++] = 0;
    for (int i = 0; i < 6; i++)
        f[n++] = bssid[i];
    for (int i = 0; i < 6; i++)
        f[n++] = EF.mac[i];
    for (int i = 0; i < 6; i++)
        f[n++] = bssid[i];
    f[n++] = 0;
    f[n++] = 0;
    return n;
}

static unsigned put_u16(u8 *f, unsigned n, unsigned v)
{
    f[n++] = (u8)v;
    f[n++] = (u8)(v >> 8);
    return n;
}

static unsigned build_auth(u8 *f, const u8 *bssid)
{
    unsigned n = mgmt_header(f, 0xB0, bssid);
    n = put_u16(f, n, 0); /* algorithm: open system */
    n = put_u16(f, n, 1); /* transaction sequence number 1 */
    n = put_u16(f, n, 0); /* status: success (the field is reserved in a request) */
    return n;
}

static unsigned build_deauth(u8 *f, const u8 *bssid)
{
    unsigned n = mgmt_header(f, 0xC0, bssid);
    return put_u16(f, n, 3); /* reason 3: the station is leaving the network */
}

/* the association request; the security it asks for is what rsn_check() accepted: CCMP for both ciphers and PSK */
static unsigned build_assoc(u8 *f, const u8 *bssid, const char *ssid, unsigned ssid_len, unsigned channel)
{
    unsigned n = mgmt_header(f, 0x00, bssid);
    n = put_u16(f, n, CAP_ESS | CAP_PRIVACY | (channel <= 14 ? 0x0400 : 0)); /* an infrastructure station, with privacy (and short slot time at 2.4 GHz) */
    n = put_u16(f, n, 10);                                                    /* listen interval, in beacon intervals */
    f[n++] = 0;                                                               /* SSID */
    f[n++] = (u8)ssid_len;
    for (unsigned i = 0; i < ssid_len; i++)
        f[n++] = (u8)ssid[i];
    f[n++] = 1; /* supported rates: the legacy OFDM rates, 6, 12 and 24 Mb/s basic (the AP's own basic rates must be among ours) */
    f[n++] = 8;
    static const u8 R5[8] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
    static const u8 R2[8] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24};
    for (int i = 0; i < 8; i++)
        f[n++] = channel <= 14 ? R2[i] : R5[i];
    if (channel <= 14)
    {
        f[n++] = 50; /* extended supported rates: 24, 36, 48, 54 Mb/s */
        f[n++] = 4;
        f[n++] = 0x30;
        f[n++] = 0x48;
        f[n++] = 0x60;
        f[n++] = 0x6C;
    }
    static const u8 RSN[22] = {48, 20, 1, 0,                  /* the RSN element: version 1 */
                               0x00, 0x0F, 0xAC, 4,           /* group cipher CCMP */
                               1, 0, 0x00, 0x0F, 0xAC, 4,     /* one pairwise cipher: CCMP */
                               1, 0, 0x00, 0x0F, 0xAC, 2,     /* one key management: PSK */
                               0, 0};                         /* no capabilities: no management frame protection */
    for (int i = 0; i < 22; i++)
        f[n++] = RSN[i];
    return n;
}

#define CONN_TRIES 3
#define CONN_WAIT_MS 120

/* send a frame until the wanted answer has arrived (up to CONN_TRIES times); returns how many frames were sent */
static unsigned conn_exchange(const u8 *frame, unsigned len, unsigned channel, int *flag)
{
    unsigned sent = 0;
    for (int t = 0; t < CONN_TRIES && !*flag; t++)
    {
        if (tx_mgmt(frame, len, channel) == 0)
            sent++;
        for (int ms = 0; ms < CONN_WAIT_MS && !*flag; ms++)
        {
            udelay(1000);
            rx_poll();
        }
    }
    return sent;
}

static void handshake_run(void);
static void connect_exchange(void);

/* the whole attempt: authenticate, associate, leave */
static int CONN_ENABLE = 1; /* the host tests of the earlier stages switch it off */

static void connect_try(void)
{
    CONN.state = 0;
    if (!CONN_ENABLE || !TARGET.found || TXP.variant < 0 || !TXP.sent || SC.ap[TARGET.index].rsn_len == 0)
        return;
    const ScanAp *ap = &SC.ap[TARGET.index];
    if (rsn_check(ap->rsn, ap->rsn_len) != RSN_OK)
        return;
    for (int i = 0; i < 6; i++)
        CONN.bssid[i] = ap->bssid[i];
    CONN.channel = ap->channel;
    tx_variant_setup(TXP.variant < 0 ? 0 : TXP.variant); /* the way the DMA was found to start, and fresh ring pointers */
    tune_for_tx(CONN.channel);
    rx_restart();
    wr32(R_RCR, RCR_RX_DEFAULT);
    wr8(R_CR, MAC_TRX_ENABLE);
    connect_exchange();
}

/* authenticate, associate, 4-way handshake, keys in the chip; the chip is tuned to CONN.channel and CONN.bssid is the access point. The first
 * connection (connect_try) and every later reconnection (link_service) go through here. A link that did not come up is torn down again. */
static void connect_exchange(void)
{
    CONN.state = 1;
    CONN.auth_seen = CONN.assoc_seen = 0;
    CONN.auth_status = CONN.assoc_status = CONN.aid = CONN.dropped = 0;
    CONN.auth_tries = CONN.assoc_tries = 0;
    u8 frame[160];
    unsigned len = build_auth(frame, CONN.bssid);
    CONN.auth_tries = conn_exchange(frame, len, CONN.channel, &CONN.auth_seen);
    if (CONN.auth_seen && CONN.auth_status == 0)
    {
        len = build_assoc(frame, CONN.bssid, CFG.ssid, CFG.ssid_len, CONN.channel);
        CONN.assoc_tries = conn_exchange(frame, len, CONN.channel, &CONN.assoc_seen);
        if (CONN.assoc_seen && CONN.assoc_status == 0)
            handshake_run();
        link_try(); /* a handshake that worked and put the keys in the chip: the card stays connected */
        if (!link_is_up()) /* nothing worked out: tidy up */
        {
            len = build_deauth(frame, CONN.bssid);
            if (tx_mgmt(frame, len, CONN.channel) == 0)
                listen_ms(20);
        }
    }
}

/* ---- stage 6c-2: the keys in the chip ----
 * With the keys from the handshake the chip can encrypt and decrypt the data frames itself (AES-CCMP). It keeps the keys in a table of 32
 * entries, the "CAM": a key (16 bytes), the address it belongs to, its type, whether it is a group key, and the key id. An entry is written
 * as eight 32-bit words, each through a command register (0x670) after the data register (0x674), exactly as Linux' rtw_sec_write_cam does.
 * The first four entries are the group keys, found by their key id (the "default key search" mode Linux uses): the group key of the access
 * point (key id 1 here) goes to entry 1. The pairwise key goes to the first free entry after them (4), found by the address of the access point.
 * The security engine itself is switched on first (bit 9 of the command register, and the decrypt / default key bits of the config register). */
#define R_SEC_CMD 0x0670
#define R_SEC_WRITE 0x0674
#define R_SEC_CONFIG 0x0680
#define CAM_TYPE_AES 4
#define CAM_PAIRWISE_ENTRY 4

static void sec_enable(void)
{
    wr16(R_CR, (u16)(rd16(R_CR) | BIT(9))); /* RTW_SEC_ENGINE_EN */
    /* decrypt on receive (BIT 3), encrypt on transmit (BIT 2), and the default key search for unicast and broadcast, both directions */
    wr16(R_SEC_CONFIG, (u16)(rd16(R_SEC_CONFIG) | BIT(2) | BIT(3) | BIT(0) | BIT(1) | BIT(6) | BIT(7)));
}

/* rtw_sec_write_cam: entry `idx` = key (16 bytes) with key id `keyidx`, group or pairwise, for the address `addr` */
static void cam_write(unsigned idx, const u8 *key, unsigned keyidx, int group, const u8 *addr)
{
    u32 base = idx << 3; /* eight words per entry */
    for (int i = 7; i >= 0; i--)
    {
        u32 content;
        switch (i)
        {
        case 0:
            content = (keyidx & 3) | ((u32)CAM_TYPE_AES << 2) | ((u32)(group ? 1 : 0) << 6) | BIT(15) | (u32)addr[0] << 16 | (u32)addr[1] << 24;
            break;
        case 1:
            content = (u32)addr[2] | (u32)addr[3] << 8 | (u32)addr[4] << 16 | (u32)addr[5] << 24;
            break;
        case 6:
        case 7:
            content = 0;
            break;
        default:
        {
            unsigned j = (unsigned)(i - 2) << 2;
            content = (u32)key[j] | (u32)key[j + 1] << 8 | (u32)key[j + 2] << 16 | (u32)key[j + 3] << 24;
            break;
        }
        }
        wr32(R_SEC_WRITE, content);
        wr32(R_SEC_CMD, BIT(16) | BIT(31) | (base + (u32)i)); /* write enable, poll */
    }
}

/* entries 0 .. CAM_PAIRWISE_ENTRY off again (a word 0 without the "valid" bit): before a new association, so that no key of the old one is left */
static void cam_clear(void)
{
    for (unsigned idx = 0; idx <= CAM_PAIRWISE_ENTRY; idx++)
    {
        wr32(R_SEC_WRITE, 0);
        wr32(R_SEC_CMD, BIT(16) | BIT(31) | (idx << 3));
    }
}

/* ---- stage 6c-1: the WPA2 4-way handshake over the air ----
 * After the association the access point wants proof that we know the passphrase, without anybody sending the passphrase: it sends a random
 * number (message 1), we answer with ours and a signature that only someone with the same key could make (message 2), it checks, sends the
 * group key encrypted and signed (message 3), and we confirm (message 4). The messages are EAPOL frames, carried in ordinary data frames:
 * the 802.11 data header, the 8 byte LLC/SNAP header AA AA 03 00 00 00 + ethertype 88 8E, then the EAPOL packet. The key arithmetic is
 * in wpa.c; this part moves the frames. The keys are not installed in the chip yet (the next stage): no data can be sent after this one. */
#define EAPOL_LLC_LEN 8

static int mac_eq(const volatile u8 *a, const u8 *b) { return mac_is(a, b); }

/* the one EAPOL packet waiting to be handled (the access point sends one at a time and repeats it when it is not answered) */
static struct
{
    int pending;
    unsigned len;
    u8 b[EAPOL_MAX];
    unsigned dropped; /* packets that did not fit or arrived while one was waiting */
} EAPQ;

static int HS_LISTEN; /* the receive path keeps EAPOL packets only while the handshake runs */

/* a data frame from the receive path (without FCS): an EAPOL packet from our access point? */
static void eapol_note(const volatile u8 *f, unsigned len)
{
    if (!HS_LISTEN || len < 24 + EAPOL_LLC_LEN + 4)
        return;
    unsigned fc0 = f[0], fc1 = f[1];
    if ((fc0 & 0x0C) != 0x08 || (fc1 & 3) != 2) /* a data frame, from the distribution system (the access point) */
        return;
    if (!mac_eq(f + 4, EF.mac) || !mac_eq(f + 10, CONN.bssid))
        return;
    unsigned hdr = 24 + ((fc0 & 0x80) ? 2 : 0); /* the QoS control field of a QoS data frame */
    if (fc1 & 0x80)
        hdr += 4; /* an HT control field */
    static const u8 LLC[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    if (len < hdr + EAPOL_LLC_LEN + 4)
        return;
    for (int i = 0; i < 8; i++)
        if (f[hdr + (unsigned)i] != LLC[i])
            return;
    unsigned n = len - hdr - EAPOL_LLC_LEN;
    if (EAPQ.pending || n > EAPOL_MAX)
    {
        EAPQ.dropped++;
        return;
    }
    for (unsigned i = 0; i < n; i++)
        EAPQ.b[i] = f[hdr + EAPOL_LLC_LEN + i];
    EAPQ.len = n;
    EAPQ.pending = 1;
}

/* random bytes for our nonce. On the PC: RDRAND when the processor has it, mixed with the time stamp counter read at many unequal moments; the
 * result is hashed. (Not a certified random generator: the nonce must only be unpredictable and different every time.) */
#ifdef HARIS_HOST
void rtl8821ce_host_random(u8 *out, unsigned n);
static void wpa_random(u8 *out, unsigned n) { rtl8821ce_host_random(out, n); }
#else
static u64 read_tsc(void)
{
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (u64)hi << 32 | lo;
}

static int have_rdrand(void)
{
    unsigned a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (c >> 30) & 1;
}

static void wpa_random(u8 *out, unsigned n)
{
    Sha1 s;
    u8 digest[20];
    unsigned counter = 0;
    int rd = have_rdrand();
    while (n)
    {
        sha1_init(&s);
        for (int i = 0; i < 16; i++)
        {
            u64 v = read_tsc();
            sha1_update(&s, &v, 8);
            for (unsigned k = 0; k < (read_tsc() & 0x3F); k++) /* the time of the next read depends on how long this loop ran */
                io_in8(0x80);
        }
        if (rd)
            for (int i = 0; i < 4; i++)
            {
                u64 r = 0;
                unsigned char ok = 0;
                for (int tries = 0; tries < 10 && !ok; tries++)
                    __asm__ volatile("rdrand %0; setc %1" : "=r"(r), "=qm"(ok));
                sha1_update(&s, &r, 8);
            }
        sha1_update(&s, EF.mac, 6);
        sha1_update(&s, &counter, sizeof counter);
        sha1_final(&s, digest);
        counter++;
        unsigned take = n < 20 ? n : 20;
        for (unsigned i = 0; i < take; i++)
            out[i] = digest[i];
        out += take;
        n -= take;
    }
}
#endif

/* an EAPOL packet to the access point in a data frame: sent through the management queue (no security, hardware sequence number) */
static int tx_eapol(const u8 *eapol, unsigned len, unsigned channel)
{
    u8 frame[24 + EAPOL_LLC_LEN + EAPOL_MAX];
    unsigned n = mgmt_header(frame, 0x08, CONN.bssid); /* data, no flags yet */
    frame[1] = 0x01;                                   /* to the distribution system: it goes to the access point, address 3 is the destination */
    static const u8 LLC[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    for (int i = 0; i < 8; i++)
        frame[n++] = LLC[i];
    for (unsigned i = 0; i < len; i++)
        frame[n++] = eapol[i];
    return tx_mgmt(frame, n, channel);
}

enum { HS_NONE = 0, HS_OK = 1 };
static struct
{
    int state;        /* 0 not tried, 1 tried */
    int result;       /* HS_OK, 0 = nothing came from the access point, else the negative WPA4_E_ code that ended it */
    unsigned rx_msgs; /* EAPOL packets taken */
    unsigned tx_msgs;
    u8 pmk[32];
    int keys;         /* the keys are in the chip */
    unsigned dec, icv; /* protected frames decrypted / failed in the second after the keys went in */
} HS;
static Wpa4 W4;
#define HS_WAIT_MS 6000
#define HS_DECRYPT_MS 1500

static void handshake_run(void)
{
    HS.state = 1;
    HS.result = 0;
    HS.keys = 0;
    HS.dec = HS.icv = 0;
    HS.rx_msgs = HS.tx_msgs = 0;
    EAPQ.pending = 0;
    EAPQ.dropped = 0;
    wpa_pmk(CFG.psk, CFG.psk_len, CFG.ssid, CFG.ssid_len, HS.pmk);
    static const u8 RSN[22] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0}; /* as in the association request */
    wpa4_init(&W4, HS.pmk, CONN.bssid, EF.mac, RSN, 22);
    HS_LISTEN = 1;
    u8 snonce[32], reply[EAPOL_MAX];
    for (unsigned ms = 0; ms < HS_WAIT_MS; ms++)
    {
        udelay(1000);
        rx_poll();
        if (!EAPQ.pending)
            continue;
        HS.rx_msgs++;
        wpa_random(snonce, 32);
        unsigned rl = 0;
        int r = wpa4_rx(&W4, EAPQ.b, EAPQ.len, snonce, reply, &rl);
        EAPQ.pending = 0;
        if (r == WPA4_SEND_MSG2 || r == WPA4_SEND_MSG4)
        {
            if (tx_eapol(reply, rl, CONN.channel) == 0)
                HS.tx_msgs++;
            if (r == WPA4_SEND_MSG4)
            {
                HS.result = HS_OK;
                break;
            }
        }
        else if (r == WPA4_E_MIC || r == WPA4_E_NONCE || r == WPA4_E_KEYDATA || r == WPA4_E_NO_GTK || r == WPA4_E_FORMAT)
        {
            HS.result = r; /* the access point's message 3 does not fit what we derived: no use waiting for more */
            break;
        }
        /* WPA4_IGNORED, a replayed counter, a short frame: wait for the next one */
    }
    if (HS.result == HS_OK)
    {
        /* message 4 is out: the keys go into the chip, and for a while we listen to see whether it can decrypt what the access point sends */
        static const u8 BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        sec_enable();
        cam_write(CAM_PAIRWISE_ENTRY, W4.ptk + 32, 0, 0, CONN.bssid);
        cam_write(W4.gtk_id, W4.gtk, W4.gtk_id, 1, BCAST);
        HS.keys = 1;
        RX.decrypted = RX.icv_err = 0;
        listen_ms(HS_DECRYPT_MS);
        HS.dec = RX.decrypted;
        HS.icv = RX.icv_err;
    }
    HS_LISTEN = 0;
    for (unsigned i = 0; i < sizeof HS.pmk; i++) /* the master key is not kept around */
        ((volatile u8 *)HS.pmk)[i] = 0;
}

/* ---- stage 7: data ----
 * With the keys in the chip the card is a network adapter: Ethernet frames of the network stack go out as encrypted 802.11 data frames and
 * come in again. The conversion, in short:
 *   out:  802.11 header (data, to the access point: address 1 = the BSSID, 2 = us, 3 = the real destination), then the 8 byte CCMP header (the packet
 *         number, which the receiver uses against replays, and the key id), then LLC/SNAP (AA AA 03 00 00 00 + the ethertype), then the payload.
 *         The chip does the AES: the descriptor says "security type 3" and it encrypts everything after the CCMP header and appends the 8 byte MIC.
 *   in:   the chip decrypts and checks the MIC, and leaves the frame as it was: header, CCMP header, plain LLC/SNAP + payload, MIC, FCS.
 *         Address 1 is the destination, address 3 the source (the frame comes from the access point). The Ethernet frame is destination, source,
 *         ethertype, payload. Only frames the chip says it decrypted are taken (an unprotected frame could come from anybody), from our access
 *         point, to us or to a group address, with a packet number larger than the last one seen (the replay check the chip does not do). */
#define BEQ_RING_LEN 256
#define R_TXBD_IDX_BEQ 0x03A8
#define TX_QSEL_BE 0 /* traffic class 0: the best-effort queue */
#define RATE_24M 8   /* the fixed rate of data frames: no rate adaptation yet (the firmware's would need more H2C set-up) */
#define ETH_MAX 1514
#define RXE_SLOTS 8

typedef struct
{
    unsigned len;
    u8 b[ETH_MAX + 6];
} EthFrame;

static struct
{
    int adapter;              /* the card is the network adapter (set by the first connection, then for good: it reconnects by itself) */
    int up;                   /* associated, keys in the chip: data can flow */
    u64 tx_pn;                /* the last packet number we used */
    u64 rx_pn_pairwise;       /* the largest we accepted */
    u64 rx_pn_group[4];       /* ... per group key id: a new group key starts counting again, the old one may still be in use for a moment */
    unsigned seq;             /* 802.11 sequence number */
    unsigned tx_frames, tx_failed;
    unsigned rx_frames, rx_dropped, rx_replay;
    unsigned be_wp;           /* the next free entry of the best-effort ring */
    unsigned head, tail;      /* the queue of received frames */
    EthFrame q[RXE_SLOTS];
    /* keeping the link alive (link_service) */
    u64 last_rx;              /* when a frame from the access point (beacon, data ...) was last heard */
    u64 last_probe;           /* when we last asked it whether it is there */
    u64 next_try;             /* not before this time the next reconnection is tried */
    unsigned backoff;         /* ms between reconnection attempts: doubles with every failure up to LINK_RETRY_MAX_MS */
    unsigned fails;           /* attempts since the link was lost that did not work */
    unsigned reconnects;      /* connections made again */
    unsigned rekeys;          /* group keys renewed by the access point and installed */
    unsigned eapol_bad;       /* EAPOL-Key frames of the access point that were refused (bad signature, replay ...) */
} LINK;

#define LINK_QUIET_MS 3000     /* nothing heard from the access point for this long: ask it */
#define LINK_DEAD_MS 9000      /* ... and still nothing: it is gone, connect again */
#define LINK_PROBE_MS 1000     /* between two such questions */
#define LINK_RETRY_MIN_MS 500
#define LINK_RETRY_MAX_MS 30000

static int ONLINE_ENABLE = 1; /* the host tests of the earlier stages switch it off (a link that stays up changes what they look at) */

static const u8 LLC_SNAP[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};

/* one prepared frame (802.11 header + CCMP header + LLC/SNAP + payload, no MIC, no FCS) into the best-effort queue; 0 once the chip fetched it */
static int tx_be(const u8 *frame, unsigned len, unsigned seq)
{
    if (!D.ready || !D.stage || len < 24 + 8 + 8 || len > 24 + 8 + 8 + ETH_MAX)
        return -1;
    unsigned rp = (rd32(R_TXBD_IDX_BEQ) >> 16) & 0xFFF;
    if ((LINK.be_wp + 1) % BEQ_RING_LEN == rp) /* the ring is full */
        return -1;
    volatile u8 *s = D.stage;
    u8 *d = D.stage;
    zero_bytes(d, TX_DESC_SIZE);
    for (unsigned i = 0; i < len; i++)
        s[TX_DESC_SIZE + i] = frame[i];
    put32(d + 0, len | (TX_DESC_SIZE << 16) | BIT(26));                                              /* size, offset to the frame, last segment */
    put32(d + 4, (unsigned)TX_QSEL_BE << 8 | (unsigned)TX_RATEID_G << 16 | 3u << 22);                /* MAC id 0, queue, rate id, security type 3 = AES */
    put32(d + 12, BIT(8) | BIT(10));                                                                 /* the rate of this descriptor, no fallback */
    put32(d + 16, RATE_24M);
    put32(d + 36, (seq & 0xFFF) << 12);                                                              /* the sequence number we put in the header */
    u8 *bd = D.mem + Q_BE * PAGE + LINK.be_wp * 16;
    zero_bytes(bd, 16);
    put16(bd + 0, TX_DESC_SIZE);
    put16(bd + 2, (TX_DESC_SIZE + len - 1) / 128 + 1);
    put32(bd + 4, D.stage_dma);
    put16(bd + 8, len);
    put16(bd + 10, 0);
    put32(bd + 12, D.stage_dma + TX_DESC_SIZE);
    fence();
    unsigned next = (LINK.be_wp + 1) % BEQ_RING_LEN;
    wr16(R_TXBD_IDX_BEQ, (u16)next);
    LINK.be_wp = next;
    for (int i = 0; i < 50000; i++)
    {
        if (((rd32(R_TXBD_IDX_BEQ) >> 16) & 0xFFF) == next)
            return 0;
        udelay(1);
    }
    return -1;
}

/* an Ethernet frame of the network stack to the access point */
static int wifi_send(const void *eth, unsigned len)
{
    if (!LINK.up || CONN.dropped || !eth || len < 14 || len > ETH_MAX)
        return -1;
    const u8 *e = (const u8 *)eth;
    u8 f[24 + 8 + 8 + ETH_MAX];
    unsigned n = 0;
    f[n++] = 0x08; /* data */
    f[n++] = 0x41; /* to the distribution system, protected */
    f[n++] = 0;
    f[n++] = 0;
    for (int i = 0; i < 6; i++) /* address 1: the receiver, the access point */
        f[n++] = CONN.bssid[i];
    for (int i = 0; i < 6; i++) /* address 2: the transmitter, us */
        f[n++] = EF.mac[i];
    for (int i = 0; i < 6; i++) /* address 3: the destination of the Ethernet frame */
        f[n++] = e[i];
    unsigned seq = LINK.seq++ & 0xFFF;
    f[n++] = (u8)(seq << 4);
    f[n++] = (u8)(seq >> 4);
    u64 pn = ++LINK.tx_pn;
    f[n++] = (u8)pn; /* the CCMP header: PN0, PN1, reserved, key id (0) with the "extended IV" bit, PN2 .. PN5 */
    f[n++] = (u8)(pn >> 8);
    f[n++] = 0;
    f[n++] = 0x20;
    f[n++] = (u8)(pn >> 16);
    f[n++] = (u8)(pn >> 24);
    f[n++] = (u8)(pn >> 32);
    f[n++] = (u8)(pn >> 40);
    for (int i = 0; i < 6; i++) /* LLC / SNAP: then the ethertype */
        f[n++] = LLC_SNAP[i];
    f[n++] = e[12];
    f[n++] = e[13];
    for (unsigned i = 14; i < len; i++)
        f[n++] = e[i];
    if (tx_be(f, n, seq) != 0)
    {
        LINK.tx_failed++;
        return -1;
    }
    LINK.tx_frames++;
    return 0;
}

static unsigned vget_be16(const volatile u8 *p) { return (unsigned)p[0] << 8 | p[1]; }

/* a data frame from the receive path, without its FCS, with word 0 of its receive descriptor: 1 if it was taken care of (kept or dropped as
 * not for us), 0 if the link is not up and the handshake code may want it */
static int wifi_rx_data(const volatile u8 *f, unsigned len, u32 w0)
{
    if (!LINK.up)
        return 0;
    unsigned fc0 = f[0], fc1 = f[1];
    if ((fc0 & 0x0C) != 0x08 || ((fc0 >> 4) != 0 && (fc0 >> 4) != 8)) /* data or QoS data (not null frames etc.) */
        return 1;
    if ((fc1 & 3) != 2 || !mac_is(f + 10, CONN.bssid)) /* from the access point only */
        return 1;
    int group = f[4] & 1;
    if (!group && !mac_is(f + 4, EF.mac))
        return 1;
    /* protected, and decrypted by the chip without an error (descriptor word 0: type of encryption bits 22..20, "software decrypt" bit 27, ICV error bit 15) */
    if (!(fc1 & 0x40) || ((w0 >> 15) & 1) || ((w0 >> 27) & 1) || !((w0 >> 20) & 7))
    {
        LINK.rx_dropped++;
        return 1;
    }
    unsigned hdr = 24 + ((fc0 & 0x80) ? 2 : 0) + ((fc1 & 0x80) ? 4 : 0);
    if (len < hdr + 8 + 8 + 2)
    {
        LINK.rx_dropped++;
        return 1;
    }
    const volatile u8 *c = f + hdr; /* the CCMP header */
    if (!(c[3] & 0x20))
    {
        LINK.rx_dropped++;
        return 1;
    }
    u64 pn = (u64)c[0] | (u64)c[1] << 8 | (u64)c[4] << 16 | (u64)c[5] << 24 | (u64)c[6] << 32 | (u64)c[7] << 40;
    u64 *last = group ? &LINK.rx_pn_group[c[3] >> 6] : &LINK.rx_pn_pairwise; /* (the key id: the top two bits of the fourth byte) */
    if (pn <= *last)
    {
        LINK.rx_replay++;
        return 1;
    }
    const volatile u8 *p = f + hdr + 8; /* LLC / SNAP */
    for (int i = 0; i < 6; i++)
        if (p[i] != LLC_SNAP[i])
        {
            LINK.rx_dropped++;
            return 1;
        }
    unsigned eth_type = vget_be16(p + 6);
    if (eth_type == 0x888E) /* EAPOL (a group key renewal): not for the network stack, link_service() answers it. The 802.1X header knows its length. */
    {
        unsigned n = len - hdr - 8 - 8;
        unsigned body = n >= 4 ? vget_be16(p + 8 + 2) : 0;
        if (n >= 4 && 4 + body <= n)
            n = 4 + body;
        if (group || EAPQ.pending || n > EAPOL_MAX)
        {
            LINK.rx_dropped++;
            return 1;
        }
        *last = pn;
        for (unsigned i = 0; i < n; i++)
            EAPQ.b[i] = p[8 + i];
        EAPQ.len = n;
        EAPQ.pending = 1;
        return 1;
    }
    unsigned avail = len - hdr - 8 - 8; /* the payload, and the 8 byte MIC after it when the chip leaves it */
    unsigned plen = avail;
    if (eth_type == 0x0800 && avail >= 20) /* IPv4: the header knows how long the packet is */
    {
        unsigned total = vget_be16(p + 8 + 2);
        if (total >= 20 && total <= avail)
            plen = total;
    }
    else if (eth_type == 0x0806 && avail >= 28) /* ARP: 28 bytes */
        plen = 28;
    else if (avail >= 8)
        plen = avail - 8; /* some other protocol: take the MIC off */
    if (plen > ETH_MAX - 14)
        plen = ETH_MAX - 14;
    if (mac_is(f + 16, EF.mac)) /* our own broadcast, sent back by the access point */
    {
        LINK.rx_dropped++;
        return 1;
    }
    if ((LINK.head + 1) % RXE_SLOTS == LINK.tail)
    {
        LINK.rx_dropped++; /* the queue is full: the network stack is behind */
        return 1;
    }
    *last = pn;
    EthFrame *q = &LINK.q[LINK.head];
    for (int i = 0; i < 6; i++)
    {
        q->b[i] = f[4 + i];       /* destination: address 1 */
        q->b[6 + i] = f[16 + i];  /* source: address 3 */
    }
    q->b[12] = p[6];
    q->b[13] = p[7];
    for (unsigned i = 0; i < plen; i++)
        q->b[14 + i] = p[8 + i];
    q->len = 14 + plen;
    LINK.head = (LINK.head + 1) % RXE_SLOTS;
    LINK.rx_frames++;
    return 1;
}

static const unsigned char NO_MAC[6];
static const unsigned char *rtl8821ce_mac_up(void) { return LINK.adapter ? EF.mac : NO_MAC; }
static int link_is_up(void) { return LINK.up && !CONN.dropped; }

/* the handshake worked and the keys are in the chip: from now on the card carries the traffic of the network stack */
static void link_try(void)
{
    if (!ONLINE_ENABLE || !HS.state || HS.result != HS_OK || !HS.keys)
        return;
    LINK.tx_pn = LINK.rx_pn_pairwise = 0; /* new keys: both sides count packet numbers from the start */
    for (int i = 0; i < 4; i++)
        LINK.rx_pn_group[i] = 0;
    LINK.seq = 0;
    LINK.head = LINK.tail = 0;
    EAPQ.pending = 0;
    if (!LINK.adapter) /* the first connection; a later one goes on counting, and the transmit ring keeps its place (the chip's read index is where it is) */
    {
        LINK.tx_frames = LINK.tx_failed = LINK.rx_frames = LINK.rx_dropped = LINK.rx_replay = 0;
        LINK.be_wp = 0;
        LINK.reconnects = LINK.rekeys = LINK.eapol_bad = LINK.fails = 0;
    }
    LINK.last_rx = LINK.last_probe = now_ms();
    LINK.adapter = 1;
    LINK.up = 1;
}

/* ---- keeping the connection ----
 * A Wi-Fi link is not a cable: it ends when the access point reboots, when we are out of range for a while, when it changes the channel, or when it
 * simply decides that we are gone. And it needs care while it lasts: the access point renews the group key (the one for broadcast traffic) about
 * once an hour and throws out a station that does not take part. link_service(), called from every poll of the network stack, does
 *   - noticing the end: a deauthentication / disassociation (conn_note), or silence. The access point sends a beacon about every 100 ms, so after
 *     LINK_QUIET_MS without any frame from it we send it a probe request (it answers those) every LINK_PROBE_MS, and after LINK_DEAD_MS we give up.
 *   - connecting again: authenticate, associate, 4-way handshake on the old channel; every third failure a new scan first (the access point may be
 *     on another channel), with the pause between attempts doubling from 0.5 s to 30 s. The network stack notices nothing but a few lost packets.
 *   - the group key renewal and a repeated message 3 (see keys_service). */

/* a frame the receive path heard (not a control frame, without FCS): is it from our access point? Then it is alive. */
static void alive_note(const volatile u8 *f, unsigned len)
{
    if (!LINK.up || len < 24 || !mac_is(f + 10, CONN.bssid))
        return;
    LINK.last_rx = now_ms();
}

/* a protected EAPOL-Key frame from the access point while the link is up (wifi_rx_data put it in EAPQ) */
static void keys_service(void)
{
    static const u8 BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    u8 reply[EAPOL_MAX];
    unsigned rl = 0;
    unsigned ki = EAPQ.len >= 7 ? (unsigned)EAPQ.b[5] << 8 | EAPQ.b[6] : 0;
    int r = WPA4_IGNORED;
    if (!(ki & 0x08)) /* no pairwise bit: the group key renewal */
    {
        r = wpa4_group_rx(&W4, EAPQ.b, EAPQ.len, reply, &rl);
        if (r == WPA4_SEND_GROUP2)
        {
            /* the new key goes in before the answer leaves: the access point may use it at once. The group keys sit in the entries 0..3 by key
             * id, so the old key stays valid next to it until the access point stops using it. */
            cam_write(W4.gtk_id, W4.gtk, W4.gtk_id, 1, BCAST);
            LINK.rx_pn_group[W4.gtk_id & 3] = 0;
            LINK.rekeys++;
        }
    }
    else if (ki & 0x100)
    {
        /* message 3 of the 4-way handshake again: our message 4 was lost and the access point asks once more. It is answered, but the keys are NOT
         * installed a second time: that would start the packet numbers again under the same key, and a packet number must never be used twice.
         * (A new message 1, which would renew the pairwise key, is not handled; an access point that insists ends the link and we connect again.) */
        static const u8 NO_NONCE[32];
        r = wpa4_rx(&W4, EAPQ.b, EAPQ.len, NO_NONCE, reply, &rl);
    }
    EAPQ.pending = 0;
    if (r == WPA4_SEND_GROUP2 || r == WPA4_SEND_MSG4)
    {
        u8 e[14 + EAPOL_MAX];
        for (int i = 0; i < 6; i++)
        {
            e[i] = CONN.bssid[i];
            e[6 + i] = EF.mac[i];
        }
        e[12] = 0x88;
        e[13] = 0x8E;
        for (unsigned i = 0; i < rl; i++)
            e[14 + i] = reply[i];
        wifi_send(e, 14 + rl); /* encrypted with the pairwise key, like everything now */
    }
    else if (r < 0)
        LINK.eapol_bad++;
}

/* the end of the link: no data until it is made again; the keys are wiped from the chip, the first new attempt is at once */
static void link_lost(u64 now)
{
    LINK.up = 0;
    LINK.fails = 0;
    LINK.backoff = LINK_RETRY_MIN_MS;
    LINK.next_try = now;
    cam_clear();
}

static void reconnect_try(void)
{
    if (LINK.fails % 3 == 2) /* looked on the old channel twice in vain: where is the access point now? */
    {
        scan_run();
        pick_target();
        if (TARGET.found)
        {
            const ScanAp *ap = &SC.ap[TARGET.index];
            if (rsn_check(ap->rsn, ap->rsn_len) == RSN_OK)
            {
                for (int i = 0; i < 6; i++)
                    CONN.bssid[i] = ap->bssid[i];
                CONN.channel = ap->channel;
            }
        }
        tune_for_tx(CONN.channel); /* (the scan left the radio on its last channel) */
    }
    cam_clear();
    connect_exchange();
    u64 now = now_ms();
    if (LINK.up)
    {
        LINK.fails = 0;
        LINK.reconnects++;
        return;
    }
    LINK.fails++;
    LINK.next_try = now + LINK.backoff;
    LINK.backoff = LINK.backoff * 2 > LINK_RETRY_MAX_MS ? LINK_RETRY_MAX_MS : LINK.backoff * 2;
}

static void link_service(void)
{
    if (!clock_ok())
        return;
    u64 now = now_ms();
    if (LINK.up)
    {
        u64 quiet = now - LINK.last_rx;
        if (CONN.dropped || quiet >= LINK_DEAD_MS)
            link_lost(now);
        else if (quiet >= LINK_QUIET_MS && now - LINK.last_probe >= LINK_PROBE_MS)
        {
            u8 f[64];
            unsigned n = build_probe_request(f, EF.mac, CONN.channel);
            for (int i = 0; i < 6; i++) /* directed: addressed to the access point, which answers it with a probe response */
            {
                f[4 + i] = CONN.bssid[i];
                f[16 + i] = CONN.bssid[i];
            }
            tx_mgmt(f, n, CONN.channel);
            LINK.last_probe = now;
        }
    }
    if (LINK.up)
    {
        if (EAPQ.pending)
            keys_service();
    }
    else if (now >= LINK.next_try)
        reconnect_try();
}

static int rtl8821ce_recv_up(void *buf, unsigned max)
{
    if (!LINK.adapter)
        return 0;
    rx_poll();
    link_service();
    if (!LINK.up || LINK.head == LINK.tail)
        return 0;
    EthFrame *q = &LINK.q[LINK.tail];
    unsigned n = q->len <= max ? q->len : max;
    u8 *o = (u8 *)buf;
    for (unsigned i = 0; i < n; i++)
        o[i] = q->b[i];
    LINK.tail = (LINK.tail + 1) % RXE_SLOTS;
    return (int)n;
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
    cfg_load();

    /* stage 3 needs the chip to reach RAM: bus mastering on, and the DMA rings set up (Linux does this before the power-on, too) */
    int dma_ok = 0;
    DL.state = DL_NOT_TRIED;
    EF.state = EF_NOT_TRIED;
    MI.state = MI_NOT_TRIED;
    MI.why = 0;
    PH.state = PH_NOT_TRIED;
    PH.why = 0;
    H2.state = H2_NOT_TRIED;
    H2.why = 0;
    CHN.state = CH_NOT_TRIED;
    RX.state = RXS_NOT_TRIED;
    LINK.up = 0;
    LINK.adapter = 0;
    CONN.state = 0;
    TARGET.found = 0;
    HS.state = 0;
    SC.state = 0;
    SC.n = 0;
    TXP.state = 0;
    TXP.wp = 0;
    TXP.replies = 0;
    EF.why = 0;
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
                efuse_read_mac(); /* the chip is powered on and the firmware runs: read the board's data while we can */
                mac_init();       /* stage 4b-1: queues and MAC timing; the receive DMA is switched off again at the end */
                if (MI.state == MI_OK)
                    phy_init((W.cfg1 >> 12) & 0xF); /* stage 4b-2: the baseband / AGC / RF tables */
                if (MI.state == MI_OK)
                    h2c_probe(); /* stage 4b-3: does the firmware read a command? */
                if (PH.state == PH_OK && !phy_rf_suspect() && H2.state == H2_OK)
                    channel_probe(6); /* stage 4c-1: antenna to Wi-Fi, channel 6 */
                if (CHN.state == CH_OK)
                    scan_run(); /* stage 4c-2/3: listen on every channel, note the networks */
                if (TXP_ENABLE && SC.state && SC.channels_ok)
                    tx_probe_test(149); /* stage 5a: the first frames on the air: probe requests on channel 149 */
                pick_target();
                connect_try();
            }
            else
            {
                DL.state = DL_FAILED;
                DL.mcufw = rd16(R_MCUFW_CTRL);
            }
            if (LINK.up) /* connected: the card stays on, the DMA engines too, and the network stack takes it */
                return 1;
            /* The DMA engines stay on from mac_init() to here, as in Linux: switching the command register off and on again between the
             * stages made the transmit queues lose their pages (see tx_probe_test()). Only now, before the power-off, they go off. */
            wr8(R_CR, 0);
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
        if (PH.state == PH_OK && phy_rf_suspect())
        {
            /* the RF read-back differs: the line has room for the register numbers instead of the usual details */
            nic_puts(t, ", phy rf");
            nic_putd(t, PH.rf_diff);
            nic_putc(t, ':');
            for (unsigned i = 0; i < PH.rf_diff && i < 7; i++) /* register number, then the differing bits */
            {
                nic_puth(t, PH.diff_addr[i], 2);
                nic_puth(t, PH.diff_xor[i], 5);
            }
            nic_puts(t, DL.off_after > 0 ? ", off" : ", power-off FAILED");
            return;
        }
        int all_ok = MI.state == MI_OK && PH.state == PH_OK && H2.state == H2_OK && SC.state; /* then the line has room for the scan result instead of the firmware version */
        if (!all_ok)
        {
            nic_puts(t, ", fw ");
            nic_putd(t, FW.version);
            nic_putc(t, '.');
            nic_putd(t, FW.subversion);
        }
        if (EF.state == EF_OK && !CONN.state) /* (the address leaves the line when a connection was tried: no room) */
        {
            nic_puts(t, ", MAC ");
            for (int i = 0; i < 6; i++)
            {
                nic_puth(t, EF.mac[i], 2);
                if (i < 5)
                    nic_putc(t, ':');
            }
        }
        else if (EF.state == EF_FAILED)
        {
            nic_puts(t, ", efuse: ");
            nic_puts(t, EF.why ? EF.why : "?");
        }
        if (MI.state == MI_OK && PH.state == PH_OK && H2.state == H2_OK) /* the three proven steps in one word: the line has to stay short */
        {
            if (!CONN.state) /* (a connection that got as far as being tried says more than this) */
                nic_puts(t, ", init ok");
        }
        else
        {
        if (MI.state == MI_OK)
            nic_puts(t, ", mac ok");
        else if (MI.state == MI_FAILED)
        {
            nic_puts(t, ", mac FAILED: ");
            nic_puts(t, MI.why ? MI.why : "?");
        }
        if (PH.state == PH_OK)
        {
            nic_puts(t, ", phy ");
            nic_puts(t, "ok");
        }
        else if (PH.state == PH_SKIPPED)
        {
            nic_puts(t, ", no phy: ");
            nic_puts(t, PH.why ? PH.why : "?");
        }
        if (H2.state == H2_OK)
            nic_puts(t, ", h2c ok");
        else if (H2.state == H2_FAILED)
        {
            nic_puts(t, ", h2c ");
            nic_puts(t, H2.why ? H2.why : "?");
        }
        }
        if (SC.state && TXP.state && !TXP.sent && TXP.failed)
        {
            /* the chip took no frame: show what it looked like, instead of the scan summary (the line has room for one of the two) */
            nic_puts(t, ", tx0/");
            nic_putd(t, TXP.failed);
            nic_putc(t, ' ');
            nic_puth(t, TXP.exq_before, 4);
            nic_puth(t, TXP.exq_after, 4);
            nic_putc(t, ' ');
            nic_puth(t, TXP.pub_before, 4);
            nic_puth(t, TXP.pub_after, 4);
        }
        else if (SC.state)
        {
            nic_puts(t, ", ");
            nic_putd(t, SC.n);
            nic_puts(t, "ap");
            if (TXP.state)
            {
                nic_puts(t, ", tx");
                nic_putd(t, TXP.sent);
                if (TXP.variant > 0) /* the plain start did not work, this one did */
                {
                    nic_puts(t, " v");
                    nic_putd(t, (unsigned)TXP.variant);
                }
                if (TXP.failed)
                {
                    nic_putc(t, '/');
                    nic_putd(t, TXP.sent + TXP.failed);
                }
                nic_puts(t, " pr");
                nic_putd(t, TXP.replies);
            }
            if (CFG.state == CFG_OK)
            {
                nic_puts(t, ", net ");
                if (TARGET.found)
                {
                    int why = SC.ap[TARGET.index].rsn_len ? rsn_check(SC.ap[TARGET.index].rsn, SC.ap[TARGET.index].rsn_len) : RSN_BAD_VERSION;
                    if (why == RSN_OK)
                    {
                        nic_puts(t, "ch");
                        nic_putd(t, SC.ap[TARGET.index].channel);
                    }
                    else /* the AP's security is not one this driver can join (RSN_WHY) */
                        nic_puts(t, RSN_WHY[why]);
                }
                else
                    nic_puts(t, "absent");
                if (CONN.state)
                {
                    if (!CONN.auth_seen)
                        nic_puts(t, ", auth none");
                    else if (CONN.auth_status)
                    {
                        nic_puts(t, ", auth s");
                        nic_putd(t, CONN.auth_status);
                    }
                    else if (!CONN.assoc_seen)
                        nic_puts(t, ", auth ok, assoc none");
                    else if (CONN.assoc_status)
                    {
                        nic_puts(t, ", auth ok, assoc s");
                        nic_putd(t, CONN.assoc_status);
                    }
                    else
                    {
                        nic_puts(t, ", assoc ok"); /* (an association needs the authentication to have worked) */
                        if (HS.state)
                        {
                            nic_puts(t, ", 4way ");
                            if (HS.result == HS_OK)
                            {
                                nic_puts(t, "ok");
                                if (HS.keys)
                                {
                                    nic_puts(t, ", dec ");
                                    nic_putd(t, HS.dec < 99 ? HS.dec : 99);
                                    if (HS.icv)
                                    {
                                        nic_puts(t, ", icv ");
                                        nic_putd(t, HS.icv < 99 ? HS.icv : 99);
                                    }
                                }
                            }
                            else if (HS.result == 0)
                                nic_puts(t, "none");
                            else if (HS.result == WPA4_E_MIC)
                                nic_puts(t, "mic"); /* the signature of message 3 does not fit: the passphrase is wrong */
                            else
                            {
                                nic_putc(t, 'e');
                                nic_putd(t, (unsigned)(-HS.result));
                            }
                        }
                    }
                }
            }
            else if (CFG.state == CFG_BAD)
            {
                nic_puts(t, ", cfg ");
                nic_puts(t, CFG.why ? CFG.why : "?");
            }
            else if (CFG.state == CFG_NONE)
                nic_puts(t, ", no cfg");
        }
        else if (CHN.state == CH_OK)
        {
            nic_puts(t, ", ch");
            nic_putd(t, CHN.channel);
            nic_puts(t, " ok");
        }
        else if (CHN.state == CH_FAILED)
        {
            nic_puts(t, ", ch");
            nic_putd(t, CHN.channel);
            nic_puts(t, " rf18=");
            nic_puth(t, CHN.rf18, 5);
        }
        nic_puts(t, DL.off_after > 0 ? ", off" : DL.off_after < 0 ? ", power-off FAILED" : "");
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
    if (LINK.adapter)
    {
        nic_puts(&t, "RTL8821CE Wi-Fi: ");
        nic_puts(&t, CFG.ssid);
        nic_puts(&t, " ch");
        nic_putd(&t, CONN.channel);
        nic_puts(&t, link_is_up() ? ", up" : ", reconnecting");
        nic_puts(&t, ", tx ");
        nic_putd(&t, LINK.tx_frames);
        nic_puts(&t, " rx ");
        nic_putd(&t, LINK.rx_frames);
        if (LINK.reconnects) /* (only when it happened: rc = reconnections, gk = group key renewals) */
        {
            nic_puts(&t, ", rc ");
            nic_putd(&t, LINK.reconnects);
        }
        if (LINK.rekeys)
        {
            nic_puts(&t, ", gk ");
            nic_putd(&t, LINK.rekeys);
        }
        return;
    }
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
            if (DL.state != DL_OK)
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


const NicDriver NIC_RTL8821CE = {
    "Realtek RTL8821CE (Wi-Fi)", rtl8821ce_claims, rtl8821ce_probe, rtl8821ce_mac_up, link_is_up, wifi_send,
    rtl8821ce_recv_up, rtl8821ce_status, 0, 0, 0, 0,
};
