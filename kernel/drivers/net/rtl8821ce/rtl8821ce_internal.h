/* Shared by the files of the RTL8821CE driver (kernel/net/rtl8821ce/): types, register numbers, chip access and the hooks the host test replaces. Everything in the files of this folder is described in the first comment of rtl8821ce_core.c. */
#ifndef HARIS_RTL8821CE_INTERNAL_H
#define HARIS_RTL8821CE_INTERNAL_H

#include "exec.h"
#include "io.h"
#include "net.h"
#include "../../../net/nic.h"
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


extern Wifi WF;

/* The power-on flow of the PCIe interface as Linux' tables give it: cmd 1 = read the byte, replace the bits of `mask` by those of
 * `value`, write it back; cmd 2 = poll until (byte & mask) == (value & mask). The steps of the USB and SDIO interfaces are left out. */
typedef struct
{
    u16 off;
    u8 cmd, mask, value;
} PwrCmd;

#define PWR_WRITE 1
#define PWR_POLL 2

#ifdef HARIS_HOST
/* the host test (tests/test_rtl8821ce.c) plays the chip: its delay hook is where the "chip" finishes what it was asked to do */
void rtl8821ce_host_udelay(unsigned us);
static inline void udelay(unsigned us) { rtl8821ce_host_udelay(us); }
#else
/* a read of port 0x80 takes about 1 us: a delay that needs no timer */
static inline void udelay(unsigned us)
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}
#endif

#ifdef HARIS_HOST
/* the host test moves the clock by hand (and with every delay) to play minutes of silence in a moment */
unsigned long long rtl8821ce_host_ms(void);
static inline u64 now_ms(void) { return rtl8821ce_host_ms(); }
static inline int clock_ok(void) { return 1; }
#else
static inline u64 now_ms(void) { return timer_ms(); }
static inline int clock_ok(void) { return timer_ready(); }
#endif

#ifdef HARIS_HOST
/* ... and every write to the chip is reported, so that the test can check the exact order of the sequence */
void rtl8821ce_host_log(u32 off, unsigned size, u32 value);
#define WRITE_LOG(off, size, v) rtl8821ce_host_log((off), (size), (u32)(v))
#else
#define WRITE_LOG(off, size, v) ((void)0)
#endif

static inline u8 rd8(u32 off) { return *(volatile u8 *)(WF.regs + off); }
static inline u16 rd16(u32 off) { return *(volatile u16 *)(WF.regs + off); }
static inline u32 rd32(u32 off) { return *(volatile u32 *)(WF.regs + off); }
static inline void wr8(u32 off, u8 v)
{
    *(volatile u8 *)(WF.regs + off) = v;
    WRITE_LOG(off, 1, v);
}
static inline void wr32(u32 off, u32 v)
{
    *(volatile u32 *)(WF.regs + off) = v;
    WRITE_LOG(off, 4, v);
}

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
    /* what the access point can do, from its beacon (for the rate adaptation, see "stage 8") */
    u16 rates;   /* its legacy rates: bit 0..3 = CCK 1, 2, 5.5, 11 Mb/s, bit 4..11 = OFDM 6, 9, 12, 18, 24, 36, 48, 54 Mb/s */
    u8 ht;       /* it has an HT capabilities element (802.11n) */
    u8 ht_mcs0;  /* ... the receive MCS set, MCS 0..7 (one spatial stream) */
    u16 ht_cap;  /* ... its HT capability bits (bit 5 = short guard interval at 20 MHz) */
    u8 vht;      /* it has a VHT capabilities element (802.11ac) */
    u16 vht_map; /* ... its receive MCS map: two bits per spatial stream, 0 = MCS 0-7, 1 = 0-8, 2 = 0-9, 3 = none */
    u32 vht_cap; /* ... its capability bits (bit 5 = short guard interval at 80 MHz) */
    u8 ht_sec;   /* the channel it uses: secondary channel offset (0 none, 1 above, 3 below), 40 MHz in use, VHT width (1 = 80 MHz) and the centre of the 80 MHz channel */
    u8 ht_w40, vht_w, vht_seg0;
    u8 ht_ampdu; /* its HT A-MPDU parameters: the longest aggregate it takes (bits 0-1) and the least spacing it needs between frames (bits 2-4) */
} ScanAp;
/* The channel the radio is tuned to: the primary 20 MHz channel (where management frames go, and the beacons are), and the whole channel of
 * `bw` (0 = 20, 1 = 40, 2 = 80 MHz) around the centre channel; `sc` says where the primary channel sits in it (rtw_update_channel: 0 at
 * 20 MHz; at 40 MHz 1 = the upper half, 2 = the lower half; at 80 MHz 1 / 2 = the third / second 20 MHz channel, 3 / 4 = the topmost / lowest). */
typedef struct
{
    unsigned prim, center, bw, sc;
} Chan;
#define ETH_MAX 1514
#define RXE_SLOTS 64 /* frames waiting for the network stack: a burst of an aggregate comes in one go */

typedef struct
{
    unsigned len;
    u8 b[ETH_MAX + 6];
} EthFrame;

/* ---- types, constants and variables used by more than one file ---- */
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

enum { PHASE_OFF = 1, PHASE_ON = 2 };

#define FW_NAME "RTW8821C.BIN"

#define FW_HDR_SIZE 64

#define FW_CHKSUM_SIZE 8

enum { FW_NOT_CHECKED, FW_OK, FW_MISSING, FW_BAD };

typedef struct
{
    int state;
    const char *why;                   /* FW_BAD: static text */
    unsigned version, subversion;      /* 24.11 */
    unsigned year, month, day;
    unsigned size, dmem, imem, emem;   /* bytes of the three parts (without their checksums) */
    unsigned dmem_addr, imem_addr, emem_addr; /* where in the chip's memory they go */
    int node;                          /* the file in the VFS */
} Anon_FW;
extern Anon_FW FW;

#define RTL8821CE_FW_LOAD 1 /* -DRTL8821CE_FW_LOAD=0: stop after the power-on */

#define RTL8821CE_OFF_AFTER 1 /* power the chip off again after the firmware test, so that a reboot finds it as Linux would */

#define R_SYS_CLK_CTRL 0x0008

#define R_TXDMA_PQ_MAP 0x010C

#define R_FIFOPAGE_CTRL_2 0x0204

#define R_TXDMA_STATUS 0x0210

#define R_RQPN_CTRL_2 0x022C

#define R_FIFOPAGE_INFO_1 0x0230

#define R_FWHW_TXQ_CTRL 0x0420

#define R_BCN_CTRL 0x0550

#define R_CPU_DMEM_CON 0x1080

#define R_DDMA_CH0SA 0x1200

#define R_DDMA_CH0DA 0x1204

#define R_DDMA_CH0CTRL 0x1208

#define R_H2CQ_CSR 0x1330

#define OCP_TXBUF 0x18780000u /* where the chip's DMA engine sees its transmit buffer */

#define OCP_DMEM 0x00200000u  /* addresses below this are instruction memory (IMEM / EMEM), from here on data memory */

#define TX_DESC_SIZE 48

#define CHUNK_MAX 0x1000u

#define R_PCI_CTRL 0x0300

#define R_TXBD_RWPTR_CLR 0x039C

#define R_TXBD_BCN_WORK 0x0383

#define TXBD_OWN_BIT 15

enum { Q_BK, Q_BE, Q_VI, Q_VO, Q_BCN, Q_MGMT, Q_HI0, Q_H2C, Q_COUNT };

#define RX_COUNT 128           /* Linux uses 512 buffers; a burst of an aggregate (up to 32 frames) has to find room while the host is busy */

#define RX_BUF_SIZE (4096 - 128) /* one frame of at most 3895 bytes (the longest MPDU we say we take) with its descriptor; no A-MSDU, so no jumbo buffers */

#define RX_BUF_SLOT 4096       /* a buffer and its slack, a multiple of 4096 */

#define PAGE 4096u

typedef struct
{
    int ready;
    u8 *mem;
    u32 dma;                /* the address the chip uses for mem */
    u8 *stage;              /* a transmit descriptor and up to CHUNK_MAX bytes of firmware */
    u32 stage_dma;
} Anon_D;
extern Anon_D D;

enum { EF_NOT_TRIED, EF_OK, EF_FAILED };

typedef struct
{
    int state;
    const char *why;
    unsigned char mac[6];
    int map_ok;                 /* the logical map was decoded: the three values below are real */
    unsigned char rfe, pkg, xtal; /* rfe_option (0xCA, low 5 bits), package type (0xCA bit 5), crystal calibration (0xB9) */
    unsigned char swing2g, swing5g; /* tx_bb_swing_setting_2g / _5g (0xC6, 0xC7) */
    unsigned char txpwr[42];        /* the power calibration of RF path A (logical map 0x10 .. 0x39) */
} Anon_EF;
extern Anon_EF EF;

enum { DL_NOT_TRIED, DL_OK, DL_FAILED, DL_SKIPPED };

typedef struct
{
    int state;
    const char *why;     /* DL_FAILED, DL_SKIPPED: static text */
    int part;            /* 0 DMEM, 1 IMEM, 2 EMEM */
    int piece;           /* the failure happened while a piece of that part was being sent (DL.part, DL.off say which) */
    unsigned off;        /* byte offset of the piece in its part */
    u16 mcufw;           /* MCUFW_CTRL at the end */
    int off_after;       /* 1 = powered off again; -1 = the power-off failed */
} Anon_DL;
extern Anon_DL DL;

#define R_TXPAUSE 0x0522

#define R_RCR 0x0608

#define MAC_TRX_ENABLE 0xFF /* TX/RX DMA, protocol, scheduler, MAC TX and RX: bits 0..7 of the command register */

enum { MI_NOT_TRIED, MI_OK, MI_FAILED };

typedef struct
{
    int state;
    const char *why;
    u8 cr; /* the command register once the init is through */
} Anon_MI;
extern Anon_MI MI;

#define R_RXPSEL 0x0808

#define R_RF_BASE_A 0x2800

#define RFREG_MASK 0xFFFFF

enum { PH_NOT_TRIED, PH_OK, PH_SKIPPED };

typedef struct
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
} Anon_PH;
extern Anon_PH PH;

#define R_HMETFR 0x01CC

#define R_HMEBOX0 0x01D0

#define R_HMEBOX0_EX 0x01F0

enum { H2_NOT_TRIED, H2_OK, H2_FAILED };

typedef struct
{
    int state;
    const char *why;
} Anon_H2;
extern Anon_H2 H2;

#define RF18_CHANNEL_MASK 0xFF

enum { CH_NOT_TRIED, CH_OK, CH_FAILED };

typedef struct
{
    int state;
    unsigned channel;
    unsigned rf18; /* RF register 0x18 as read back */
} Anon_CHN;
extern Anon_CHN CHN;

#define RCR_RX_DEFAULT 0xF410400Eu /* hal->rcr: APP_FCS | APP_MIC | APP_ICV | PKTCTL_DLEN | HTC_LOC_CTRL | APP_PHYSTS | AB | AM | APM, + VHT_DACK */

enum { RXS_NOT_TRIED, RXS_OK };

typedef struct
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
} Anon_RX;
extern Anon_RX RX;

static const u8 SCAN_CHANNELS[] = {1, 6, 11, 36, 40, 44, 48, 149, 153, 157, 161};

#define SCAN_NCH ((unsigned)sizeof SCAN_CHANNELS)

typedef struct
{
    int state;
    ScanAp ap[SCAN_AP_MAX];
    unsigned n;
    unsigned per_channel[SCAN_NCH]; /* frames heard on each channel */
    unsigned channels_ok;           /* channels that could be tuned */
    unsigned cur_channel;           /* where the receiver listens now */
    unsigned dropped;               /* networks that did not fit the table */
    unsigned beacons;               /* beacons heard during the whole scan */
} Anon_SC;
extern Anon_SC SC;

#define TX_RATEID_G 7

typedef struct
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
} Anon_TXP;
extern Anon_TXP TXP;

enum { CFG_NOT_TRIED, CFG_OK, CFG_NONE, CFG_BAD };

typedef struct
{
    int state;
    const char *why;   /* CFG_BAD: "ssid" or "psk" (which line is wrong), static text */
    char ssid[33];
    unsigned ssid_len;
    char psk[64];      /* the passphrase, 8 .. 63 characters */
    unsigned psk_len;
    int rate;          /* CFG_RATE_*: how fast to try to be, from the optional line "rate=legacy / ht / auto" */
    unsigned bw;       /* the widest channel to use: 0 = 20, 1 = 40, 2 = 80 MHz (the optional line "bw=20 / 40 / 80") */
    int ampdu;         /* aggregation allowed (the optional line "ampdu=off / on") */
} Anon_CFG;
extern Anon_CFG CFG;

/* the access point of the wanted network in the scan list: the one with the most beacons heard that has this exact name and WPA2 */
typedef struct
{
    int found;
    unsigned index;
} Anon_TARGET;
extern Anon_TARGET TARGET;

enum { RSN_OK, RSN_BAD_VERSION, RSN_NO_CCMP, RSN_NO_PSK, RSN_PMF_REQUIRED, RSN_GROUP };

static const char *const RSN_WHY[] = {"ok", "rsn", "ccmp", "akm", "pmf", "grp"};

typedef struct
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
    u16 rates;   /* the capabilities of the access point (copied from its scan entry by conn_set_caps) */
    u8 ht, ht_mcs0;
    u16 ht_cap;
    u8 vht;
    u16 vht_map;
    u32 vht_cap;
    u8 ht_sec, ht_w40, vht_w, vht_seg0;
    u8 ht_ampdu;
    Chan cc;     /* the channel we use: the primary channel CONN.channel, and the wider channel around it that the radio is tuned to (chan_plan) */
} Anon_CONN;
extern Anon_CONN CONN;

typedef struct
{
    int on;           /* the firmware chooses the rates (an HT access point, rate not "legacy", and the firmware took the commands) */
    int ht, vht;      /* what was offered and accepted */
    unsigned rate_id; /* the rate table of the firmware */
    unsigned tx_rate; /* the highest rate, written in the descriptor where the firmware ignores it */
    unsigned sgi;
    unsigned bw;      /* 0 = 20, 1 = 40, 2 = 80 MHz */
    u32 mask;         /* the rate mask that was sent */
} Anon_RA;
extern Anon_RA RA;

/* the one EAPOL packet waiting to be handled (the access point sends one at a time and repeats it when it is not answered) */
typedef struct
{
    int pending;
    unsigned len;
    u8 b[EAPOL_MAX];
    unsigned dropped; /* packets that did not fit or arrived while one was waiting */
} Anon_EAPQ;
extern Anon_EAPQ EAPQ;

enum { HS_NONE = 0, HS_OK = 1 };

typedef struct
{
    int state;        /* 0 not tried, 1 tried */
    int result;       /* HS_OK, 0 = nothing came from the access point, else the negative WPA4_E_ code that ended it */
    unsigned rx_msgs; /* EAPOL packets taken */
    unsigned tx_msgs;
    u8 pmk[32];
    int keys;         /* the keys are in the chip */
    unsigned dec, icv; /* protected frames decrypted / failed in the second after the keys went in */
} Anon_HS;
extern Anon_HS HS;

#define BEQ_RING_LEN 256

#define R_TXBD_IDX_BEQ 0x03A8

#define TX_QSEL_BE 0 /* traffic class 0: the best-effort queue */

#define RATE_24M 8   /* the fixed rate of data frames: no rate adaptation yet (the firmware's would need more H2C set-up) */

typedef struct
{
    int adapter;              /* the card is the network adapter (set by the first connection, then for good: it reconnects by itself) */
    int want;                 /* a connection was made: when it is lost, connect again (not for a card that never connected, or after a refusal) */
    int scanning;             /* a scan is going on: the radio is on other channels, nothing can be sent */
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
    u64 up_since;             /* when the link came up */
    u64 last_probe;           /* when we last asked it whether it is there */
    u64 next_try;             /* not before this time the next reconnection is tried */
    unsigned backoff;         /* ms between reconnection attempts: doubles with every failure up to LINK_RETRY_MAX_MS */
    unsigned fails;           /* attempts since the link was lost that did not work */
    unsigned reconnects;      /* connections made again */
    unsigned rekeys;          /* group keys renewed by the access point and installed */
    unsigned eapol_bad;       /* EAPOL-Key frames of the access point that were refused (bad signature, replay ...) */
} Anon_LINK;
extern Anon_LINK LINK;

#define LINK_QUIET_MS 3000     /* nothing heard from the access point for this long: ask it */

#define LINK_DEAD_MS 9000      /* ... and still nothing: it is gone, connect again */

#define LINK_PROBE_MS 1000     /* between two such questions */

#define LINK_RETRY_MIN_MS 500

#define LINK_RETRY_MAX_MS 30000

static const u8 LLC_SNAP[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};

#define BA_WIN 32           /* the frames our reorder buffer holds: the window we offer; a power of two, so that the sequence number wraps in step with the slot */

typedef struct
{
    int used;
    u64 pn;
    u64 t; /* when it arrived */
    EthFrame f;
} BaSlot;

typedef struct
{
    int rx_on;         /* an agreement for the receive side of traffic class 0 */
    unsigned rx_head;  /* the sequence number that comes next */
    unsigned buffered; /* frames waiting for a gap to be filled */
    BaSlot slot[BA_WIN];
    unsigned rx_late, rx_dups, rx_timeouts, rx_sessions;
    int tx_state;      /* 0 none yet, 1 request sent, 2 in use, 3 refused or given up (until the next connection) */
    u8 tx_token;
    unsigned tx_tries;
    u64 tx_req_ms;
    unsigned tx_buf;   /* the window the access point accepted */
    u64 tx_cool;       /* not before this time a new request (after the access point ended an agreement) */
} Anon_BA;
extern Anon_BA BA;

typedef struct
{
    int pending;
    unsigned len;
    u8 b[48];
} Anon_ACTQ;
extern Anon_ACTQ ACTQ;

/* The first scan and connection (see "starting in the background" below). pending: still to be done. busy: a thread is doing it (rtl8821ce_boot_run);
 * the chip belongs to that thread meanwhile, nobody else sends, receives or starts a scan. */
typedef struct
{
    int pending;
    int busy;
} Anon_BOOT;
extern Anon_BOOT BOOT;

/* A join started from the Wi-Fi window (rtl8821ce_join_begin): the chip belongs to its thread, like with BOOT.busy. */
typedef struct
{
    int busy;
    int done;
    int result;
    char ssid[33];
    char psk[64];
} Anon_JOIN;
extern Anon_JOIN JOIN;

/* ---- rtl8821ce_core.c ---- */
int run_steps(const PwrCmd *t, int n);
int power_cycle(int already_on);
void read_state(u16 *func_en, u16 *pw_ctrl, u16 *mcufw, u8 *cr);

/* ---- rtl8821ce_fw.c ---- */
unsigned le16(const u8 *p);
void fw_check(void);
static inline void fence(void) { __asm__ volatile("mfence" ::: "memory"); }
void put16(u8 *p, unsigned v);
void put32(u8 *p, u32 v);
void zero_bytes(u8 *p, unsigned n);
void wr16(u32 off, u16 v);
void set8(u32 off, u8 bits);
void clr8(u32 off, u8 bits);
int ready32(u32 off, u32 mask, u32 value, int tries);
int dma_setup(void);
void hci_setup(void);
void init_system_cfg(void);

/* ---- rtl8821ce_efuse.c ---- */
void efuse_read_mac(void);
int fw_download(void);

/* ---- rtl8821ce_mac.c ---- */
void or32(u32 off, u32 bits);
void andn32(u32 off, u32 bits);
void mac_init(void);

/* ---- rtl8821ce_phy.c ---- */
void phy_rf_write(unsigned addr, unsigned data);
void mask32(u32 off, u32 mask, u32 value);
void phy_init(unsigned cut);
int phy_rf_suspect(void);

/* ---- rtl8821ce_chan.c ---- */
extern unsigned H2C_BOX;
void h2c_probe(void);
unsigned rf_read(unsigned addr);
void set_channel_bw(unsigned channel, unsigned bw, unsigned sc, const u32 ch_param[3], unsigned bb_swing_2g, unsigned bb_swing_5g);
void set_channel_20(unsigned channel, const u32 ch_param[3], unsigned bb_swing_2g, unsigned bb_swing_5g);
void channel_probe(unsigned channel);

/* ---- rtl8821ce_rx.c ---- */
void scan_note(const volatile u8 *f, unsigned len);
void act_note(const volatile u8 *f, unsigned len);
void conn_note(const volatile u8 *f, unsigned len);
void eapol_note(const volatile u8 *f, unsigned len);
int wifi_rx_data(const volatile u8 *f, unsigned len, u32 w0);
void link_try(void);
int link_is_up(void);
void alive_note(const volatile u8 *f, unsigned len);
void rx_restart(void);
void rx_poll(void);

/* ---- rtl8821ce_scan.c ---- */
void scan_note(const volatile u8 *f, unsigned len);
int scan_chan_begin(unsigned i);
void scan_start_state(void);
void scan_end_state(void);
void scan_run(void);

/* ---- rtl8821ce_txpwr.c ---- */
void tx_power_apply_c(const u8 *txpwr, const Chan *c);

/* ---- rtl8821ce_tx.c ---- */
extern int TXP_ENABLE;
int tx_mgmt(const u8 *frame, unsigned len, unsigned channel);
unsigned build_probe_request(u8 *f, const u8 *mac, unsigned channel);
void tune_chan(const Chan *c);
void tx_variant_setup(int v);
void listen_ms(unsigned ms);
void tx_probe_test(unsigned channel);

/* ---- rtl8821ce_join.c ---- */
void pick_target(void);
void cfg_load(void);
int rsn_check(const u8 *e, unsigned len);
int use_ht(void);
int use_vht(void);
void conn_set_caps(const ScanAp *ap);
int mac_is(const volatile u8 *a, const u8 *b);
unsigned vget16(const volatile u8 *p);
void conn_note(const volatile u8 *f, unsigned len);
unsigned mgmt_header(u8 *f, unsigned fc0, const u8 *bssid);
unsigned build_auth(u8 *f, const u8 *bssid);
unsigned build_deauth(u8 *f, const u8 *bssid);
unsigned build_assoc(u8 *f, const u8 *bssid, const char *ssid, unsigned ssid_len, unsigned channel);
unsigned conn_exchange(const u8 *frame, unsigned len, unsigned channel, int *flag);
void handshake_run(void);
void connect_exchange(void);
void connect_try(void);

/* ---- rtl8821ce_speed.c ---- */
void ra_stop(void);
void connect_exchange(void);

/* ---- rtl8821ce_keys.c ---- */
void cam_write(unsigned idx, const u8 *key, unsigned keyidx, int group, const u8 *addr);
void cam_clear(void);
void eapol_note(const volatile u8 *f, unsigned len);
extern Wpa4 W4;
void handshake_run(void);

/* ---- rtl8821ce_data.c ---- */
extern int ONLINE_ENABLE;
void rx_deliver(const EthFrame *e, u64 pn, u64 *last);
void ba_rx_frame(unsigned seq, u64 pn, const EthFrame *e);
void ba_expire(u64 now);
void ba_rx_start(unsigned ssn);
void ba_rx_stop(void);
void ba_reset(void);

/* ---- rtl8821ce_addba.c ---- */
void act_note(const volatile u8 *f, unsigned len);
void act_service(void);
void ba_tx_service(u64 now);
int wifi_send(const void *eth, unsigned len);
int wifi_rx_data(const volatile u8 *f, unsigned len, u32 w0);
const unsigned char *rtl8821ce_mac_up(void);
int link_is_up(void);
void link_try(void);

/* ---- rtl8821ce_link.c ---- */
void alive_note(const volatile u8 *f, unsigned len);
void scan_service(u64 now);
int scan_active(void);
extern int DEFER_ENABLE;
void boot_finish(void);
void link_service(void);

/* ---- rtl8821ce_select.c ---- */
void scan_service(u64 now);
int scan_active(void);
void wipe(volatile char *p, unsigned n);

#endif
