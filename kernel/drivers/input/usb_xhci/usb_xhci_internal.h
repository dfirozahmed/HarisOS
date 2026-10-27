/* Shared by the files of the xHCI driver (kernel/drivers/input/usb_xhci/): types, constants, controller state types and the register helpers. The public interface is include/usb.h. */
#ifndef HARIS_USB_XHCI_INTERNAL_H
#define HARIS_USB_XHCI_INTERNAL_H

#include "usb.h"
#include "hid_desc.h"
#include "io.h"
#include "pmm.h"
#include "timer.h"
#include "usb_msc.h"
#if defined(__x86_64__)
typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define MAX_CTRL 4
#define RING_N 256 /* TRBs per ring: one 4 KiB page, the last one is a Link TRB */
#define EV_N 256

/* PORTSC bits */
#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR (1u << 4)
#define PORT_PP (1u << 9)
#define PORT_PRC (1u << 21)
#define PORT_CHG (0x7Fu << 17)                           /* change bits: write 1 to clear */
#define PORT_KEEP ((1u << 9) | (3u << 14) | (7u << 25)) /* bits that may be written back unchanged */

/* ---------- small helpers ---------- */

static inline void udelay(unsigned us) /* a read of port 0x80 takes about 1 us */
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}

static inline void delay_ms(unsigned ms) { udelay(ms * 1000u); }

#define WAIT_MS(cond, ms)                                         \
    ({                                                            \
        int _ok = 0;                                              \
        for (unsigned _i = 0; _i < (unsigned)(ms) * 10u; _i++)    \
        {                                                         \
            if (cond)                                             \
            {                                                     \
                _ok = 1;                                          \
                break;                                            \
            }                                                     \
            udelay(100);                                          \
        }                                                         \
        _ok;                                                      \
    })

static inline void mb(void) { __asm__ volatile("mfence" ::: "memory"); }

static inline u32 rd32(volatile u8 *b, u32 off) { return *(volatile u32 *)(b + off); }
static inline void wr32(volatile u8 *b, u32 off, u32 v) { *(volatile u32 *)(b + off) = v; }
static inline void wr64(volatile u8 *b, u32 off, u64 v) { *(volatile u64 *)(b + off) = v; }

/* ---- types, constants and variables used by more than one file ---- */
typedef struct
{
    volatile u32 d0, d1, d2, d3; /* parameter low / high, status, control (bit 0 = cycle) */
} Trb;

typedef struct
{
    Trb *trb;
    u64 phys;
    unsigned idx, cycle;
    u64 last; /* physical address of the TRB written last */
} Ring;

typedef struct
{
    volatile u8 *op, *rt, *db, *ports;
    int csz; /* context size in bytes: 32 or 64 */
    int ac64;
    int nports, nslots;
    volatile u64 *dcbaa;
    Ring cmd;
    Trb *ev;
    u64 ev_phys;
    unsigned ev_idx, ev_cycle;
    volatile u8 *dma; /* scratch page for control-transfer data */
    u64 dma_phys;
    volatile u8 *inctx; /* input context page */
    u64 inctx_phys;
    u8 handled[256]; /* port already looked at while it stays connected */
    u8 tries[256];   /* failed attempts to bring up the device on that port */
} Xhci;

/* Where a device sits in the USB tree. */
typedef struct
{
    int root;          /* root port, 0-based */
    u32 route;         /* route string: 4 bits per hub tier, the port number on that hub */
    int depth;         /* number of hubs between the device and the root port */
    int speed;         /* 1 full, 2 low, 3 high, 4 super */
    int parent;        /* DEV index of the hub it hangs on, -1 for a root port */
    int pport;         /* port on that hub (1-based), or the root port (0-based) */
    int tt_slot, tt_port; /* low/full speed device behind a high speed hub: its transaction translator */
} Loc;

#define MAX_DEV 24

#define HUB_PORTS 15

typedef struct
{
    int used, gone; /* gone: unplugged or broken, to be removed from usb_poll() */
    Xhci *x;
    int slot;
    Loc loc;
    Ring ep0;
    int is_hub, nports;
    u8 handled[HUB_PORTS + 1], tries[HUB_PORTS + 1]; /* per hub port, index = port number */
} Dev;

#define MAX_HID 4

#define KIND_KBD 1

#define KIND_MOUSE 2

typedef struct
{
    int active, kind;
    Xhci *x;
    int slot, dci, dev, rlen, errs;
    Ring ring;
    volatile u8 *buf;
    u64 buf_phys;
    int rp;       /* mouse in report protocol: reports are decoded with hm (the boot layout has no wheel) */
    HidMouse hm;  /* where the buttons, X, Y and wheel sit in that report */
} Hid;

#define RDESC_MAX 1024 /* longest mouse report descriptor we read; longer = stay in boot protocol */

typedef struct
{
    int dx, dy, wheel; /* movement not yet handed out */
    unsigned buttons;
} Anon_MS;
extern Anon_MS MS;

#define MAX_MSC 4

#define MSC_BUF_PAGES 16 /* 64 KiB: the most one SCSI command moves */

typedef struct
{
    int active;
    Xhci *x;
    int slot, dev, iface;
    int in_dci, out_dci;   /* device context index of the endpoints: 2 * number + 1 for IN, 2 * number for OUT */
    int in_addr, out_addr; /* endpoint addresses as in the descriptor (bit 7 = IN) */
    unsigned in_mps, out_mps;
    Ring rin, rout;
    volatile u8 *buf;
    u64 buf_phys;
    int errs; /* transfers that failed in a row */
    UsbMsc msc;
} Msc;

#define KQ_N 16

/* The bulk endpoints of the first bulk-only mass-storage interface (alternate setting 0). */
typedef struct
{
    int iface, in_addr, out_addr;
    unsigned in_mps, out_mps, in_burst, out_burst;
} MscDesc;

/* ---- usb_xhci_core.c ---- */
u32 pci_read(u32 bus, u32 dev, u32 fn, u32 off);
void pci_write(u32 bus, u32 dev, u32 fn, u32 off, u32 v);
extern Xhci CTRL[MAX_CTRL];
extern int NCTRL;
extern Dev DEV[MAX_DEV];
extern u8 RDESC[RDESC_MAX];
extern Hid HID[MAX_HID];
extern Msc MSC[MAX_MSC];
extern u8 KQ[KQ_N][8];
extern unsigned KQ_HEAD;
extern unsigned KQ_TAIL;
void kq_push(const volatile u8 *r, int len);
void kq_push_empty(void);
volatile u8 *dma_page(Xhci *x, u64 *phys);
int ring_init(Xhci *x, Ring *r);
void ring_put(Ring *r, u64 param, u32 status, u32 control);
int ev_next(Xhci *x, Trb *out);
void hid_event(Xhci *x, const Trb *e);
void port_event(Xhci *x, const Trb *e);
void dispatch(Xhci *x, const Trb *e);
int command(Xhci *x, u64 param, u32 control, u32 *slot_out);
int control(Xhci *x, int slot, Ring *r, u8 type, u8 req, u16 val, u16 idx, u16 len, int in);
void ep_recover(Xhci *x, int slot, int dci, Ring *r);
int ctrl_ok(int code);
volatile u32 *ctx_at(Xhci *x, int n);
void inctx_clear(Xhci *x);

/* ---- usb_xhci_hid.c ---- */
void hid_queue(Hid *h);
void hid_event(Xhci *x, const Trb *e);
void port_event(Xhci *x, const Trb *e);
u32 ep_interval(int speed, u32 bint);

/* ---- usb_xhci_enum.c ---- */
void drop_slot(Xhci *x, u32 slot);
int dev_alloc(void);
void dev_remove(int di);
void fill_slot(volatile u32 *c, const Loc *L, int entries, int hub_ports, int think_time);
int bring_up(Xhci *x, const Loc *L);
void hub_scan(Xhci *x, int di);
int hub_setup(Xhci *x, int di, u32 cfg_value);

/* ---- usb_xhci_msc.c ---- */
void pump(void);
int msc_find(Xhci *x, u32 total, MscDesc *md);
int msc_setup(Xhci *x, int di, const Loc *L, u32 cfg_value, const MscDesc *md);

/* ---- usb_xhci_dev.c ---- */
int bring_up(Xhci *x, const Loc *L);
void rescan(Xhci *x);

/* ---- usb_xhci_ctrl.c ---- */
void pump(void);

#endif /* __x86_64__ */

#endif
