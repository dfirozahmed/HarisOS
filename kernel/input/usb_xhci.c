/* xHCI host controller + USB HID boot-protocol mouse. See include/usb.h.
 *
 * Everything is polled and everything lives in identity-mapped RAM, so a physical address
 * from the page allocator doubles as a pointer. Memory shared with the controller is only
 * touched through volatile pointers (no libc here: a plain fill loop could be turned into
 * a call to memset(), which does not exist in this freestanding build).
 *
 * Steps for each controller:  PCI scan -> take ownership from the firmware -> reset ->
 * command ring + event ring -> run -> for each connected root port: reset, Enable Slot,
 * Address Device, read descriptors, and if it is a boot mouse: Configure Endpoint, then
 * queue one interrupt-IN transfer at a time and turn every report into dx/dy/buttons. */
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

static void udelay(unsigned us) /* a read of port 0x80 takes about 1 us */
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}

static void delay_ms(unsigned ms) { udelay(ms * 1000u); }

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

static void mb(void) { __asm__ volatile("mfence" ::: "memory"); }

static u32 rd32(volatile u8 *b, u32 off) { return *(volatile u32 *)(b + off); }
static void wr32(volatile u8 *b, u32 off, u32 v) { *(volatile u32 *)(b + off) = v; }
static void wr64(volatile u8 *b, u32 off, u64 v) { *(volatile u64 *)(b + off) = v; }

/* ---------- PCI configuration space (legacy ports 0xCF8 / 0xCFC) ---------- */

static u32 pci_read(u32 bus, u32 dev, u32 fn, u32 off)
{
    io_out32(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return io_in32(0xCFC);
}

static void pci_write(u32 bus, u32 dev, u32 fn, u32 off, u32 v)
{
    io_out32(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    io_out32(0xCFC, v);
}

/* ---------- controller state ---------- */

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

static Xhci CTRL[MAX_CTRL];
static int NCTRL;

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

/* A device we keep running: a hub, or something with a HID interface we use. */
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

static Dev DEV[MAX_DEV];

/* One interrupt-IN endpoint of a HID boot device (mouse or keyboard). */
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
static u8 RDESC[RDESC_MAX];

static Hid HID[MAX_HID];

static struct
{
    int dx, dy, wheel; /* movement not yet handed out */
    unsigned buttons;
} MS;

/* One USB mass-storage interface (bulk-only transport): a bulk IN and a bulk OUT endpoint, each with its own
 * ring, and one bounce buffer that every data stage goes through (the caller's buffer is not DMA memory). The
 * SCSI side lives in kernel/storage/usb_msc.c; msc.msc is its state and the block device's ctx. */
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

static Msc MSC[MAX_MSC];

/* Keyboard boot reports (8 bytes each) waiting for keyboard.c */
#define KQ_N 16
static u8 KQ[KQ_N][8];
static unsigned KQ_HEAD, KQ_TAIL;

static void kq_push(const volatile u8 *r, int len)
{
    if (KQ_TAIL - KQ_HEAD >= KQ_N)
        return; /* full: the next report carries the full key state anyway */
    u8 *d = KQ[KQ_TAIL++ % KQ_N];
    for (int i = 0; i < 8; i++)
        d[i] = i < len ? r[i] : 0;
}

static void kq_push_empty(void) /* "all keys released" */
{
    static const u8 zero[8] = {0};
    kq_push(zero, 8);
}

static volatile u8 *dma_page(Xhci *x, u64 *phys)
{
    u64 p = pmm_alloc_page_tag(PMM_TAG_USB);
    if (!p)
        return 0;
    if (!x->ac64 && p >= 0x100000000ULL)
    {
        pmm_free_page(p);
        return 0;
    }
    volatile u64 *w = (volatile u64 *)p;
    for (int i = 0; i < 512; i++)
        w[i] = 0;
    *phys = p;
    return (volatile u8 *)p;
}

/* ---------- rings ---------- */

static int ring_init(Xhci *x, Ring *r)
{
    volatile u8 *pg = dma_page(x, &r->phys);
    if (!pg)
        return -1;
    r->trb = (Trb *)pg;
    r->idx = 0;
    r->cycle = 1;
    r->last = 0;
    return 0;
}

static void ring_put(Ring *r, u64 param, u32 status, u32 control)
{
    Trb *t = &r->trb[r->idx];
    t->d0 = (u32)param;
    t->d1 = (u32)(param >> 32);
    t->d2 = status;
    mb();
    t->d3 = control | r->cycle; /* the cycle bit hands the TRB to the controller: write it last */
    r->last = r->phys + (u64)r->idx * 16;
    r->idx++;
    if (r->idx == RING_N - 1) /* reached the Link TRB: point back to the start and flip the cycle */
    {
        Trb *l = &r->trb[RING_N - 1];
        l->d0 = (u32)r->phys;
        l->d1 = (u32)(r->phys >> 32);
        l->d2 = 0;
        mb();
        l->d3 = (6u << 10) | 2u | (control & (1u << 4)) | r->cycle; /* type Link, toggle-cycle; chained when the TD goes on after it */
        r->idx = 0;
        r->cycle ^= 1;
    }
}

static int ev_next(Xhci *x, Trb *out)
{
    Trb *t = &x->ev[x->ev_idx];
    if ((t->d3 & 1u) != x->ev_cycle)
        return 0;
    mb();
    out->d0 = t->d0;
    out->d1 = t->d1;
    out->d2 = t->d2;
    out->d3 = t->d3;
    if (++x->ev_idx == EV_N)
    {
        x->ev_idx = 0;
        x->ev_cycle ^= 1;
    }
    wr64(x->rt, 0x38, (x->ev_phys + (u64)x->ev_idx * 16) | 8); /* ERDP, bit 3 clears "handler busy" */
    return 1;
}

static void hid_event(Xhci *x, const Trb *e);
static void port_event(Xhci *x, const Trb *e);

static void dispatch(Xhci *x, const Trb *e)
{
    u32 type = (e->d3 >> 10) & 63;
    if (type == 32)
        hid_event(x, e);
    else if (type == 34)
        port_event(x, e);
}

/* Ring the doorbell of the command ring, wait for the command's completion event. */
static int command(Xhci *x, u64 param, u32 control, u32 *slot_out)
{
    ring_put(&x->cmd, param, 0, control);
    u64 want = x->cmd.last;
    mb();
    wr32(x->db, 0, 0);
    Trb e;
    for (unsigned i = 0; i < 20000; i++) /* about 2 s */
    {
        if (!ev_next(x, &e))
        {
            udelay(100);
            continue;
        }
        if (((e.d3 >> 10) & 63) == 33 && (((u64)e.d1 << 32) | e.d0) == want)
        {
            if (slot_out)
                *slot_out = e.d3 >> 24;
            return (int)(e.d2 >> 24); /* completion code, 1 = success */
        }
        dispatch(x, &e);
    }
    return -1;
}

/* Control transfer on endpoint 0. Data (if any) goes through x->dma. Returns the completion
 * code (1 success, 13 short packet are both fine) or -1 on timeout. */
static int control(Xhci *x, int slot, Ring *r, u8 type, u8 req, u16 val, u16 idx, u16 len, int in)
{
    u64 setup = (u64)type | ((u64)req << 8) | ((u64)val << 16) | ((u64)idx << 32) | ((u64)len << 48);
    u32 trt = len ? (in ? 3u : 2u) : 0u;
    ring_put(r, setup, 8, (2u << 10) | (1u << 6) | (trt << 16)); /* Setup stage, data inside the TRB */
    if (len)
        ring_put(r, x->dma_phys, len, (3u << 10) | (in ? (1u << 16) : 0));
    ring_put(r, 0, 0, (4u << 10) | (1u << 5) | ((len && in) ? 0 : (1u << 16))); /* Status stage, interrupt on completion */
    u64 want = r->last;
    mb();
    wr32(x->db, (u32)slot * 4, 1);
    Trb e;
    for (unsigned i = 0; i < 20000; i++)
    {
        if (!ev_next(x, &e))
        {
            udelay(100);
            continue;
        }
        if (((e.d3 >> 10) & 63) == 32 && (((u64)e.d1 << 32) | e.d0) == want)
            return (int)(e.d2 >> 24);
        dispatch(x, &e);
    }
    return -1;
}

/* After a STALL the endpoint is halted: reset it and continue behind the dead TRBs. */
static void ep_recover(Xhci *x, int slot, int dci, Ring *r)
{
    command(x, 0, (14u << 10) | ((u32)dci << 16) | ((u32)slot << 24), 0);
    command(x, (r->phys + (u64)r->idx * 16) | r->cycle,
            (16u << 10) | ((u32)dci << 16) | ((u32)slot << 24), 0);
}

static int ctrl_ok(int code) { return code == 1 || code == 13; }

/* ---------- input context ---------- */

static volatile u32 *ctx_at(Xhci *x, int n) { return (volatile u32 *)(x->inctx + (u32)n * (u32)x->csz); }

static void inctx_clear(Xhci *x)
{
    volatile u64 *w = (volatile u64 *)x->inctx;
    for (int i = 0; i < 512; i++)
        w[i] = 0;
}

/* ---------- the mouse ---------- */

static void hid_queue(Hid *h)
{
    ring_put(&h->ring, h->buf_phys, (u32)h->rlen, (1u << 10) | (1u << 5)); /* Normal TRB, interrupt on completion */
    mb();
    wr32(h->x->db, (u32)h->slot * 4, (u32)h->dci);
}

static void hid_event(Xhci *x, const Trb *e)
{
    for (int i = 0; i < MAX_HID; i++)
    {
        Hid *h = &HID[i];
        if (!h->active || h->x != x || (int)(e->d3 >> 24) != h->slot || (int)((e->d3 >> 16) & 31) != h->dci)
            continue;
        int code = (int)(e->d2 >> 24);
        if (code == 1 || code == 13)
        {
            int resid = (int)(e->d2 & 0xFFFFFF);
            int len = h->rlen > resid ? h->rlen - resid : 0;
            if (h->kind == KIND_MOUSE && h->rp)
            {
                /* report protocol: fields are where the report descriptor says (see hid_desc.h) */
                u8 rep[64];
                if (len > (int)sizeof rep)
                    len = (int)sizeof rep;
                for (int k = 0; k < len; k++)
                    rep[k] = h->buf[k];
                const HidMouse *m = &h->hm;
                int at = m->uses_ids ? 1 : 0;
                if (len > at && (!m->uses_ids || rep[0] == m->report_id))
                {
                    const u8 *data = rep + at;
                    unsigned dlen = (unsigned)(len - at);
                    MS.buttons = (unsigned)hid_value(data, dlen, m->btn_off, m->btn_n > 3 ? 3 : m->btn_n, 0);
                    MS.dx += (int)hid_value(data, dlen, m->x_off, m->x_size, m->x_signed);
                    MS.dy += (int)hid_value(data, dlen, m->y_off, m->y_size, m->y_signed);
                    if (m->has_wheel)
                        MS.wheel -= (int)hid_value(data, dlen, m->w_off, m->w_size, m->w_signed); /* HID: + = up */
                }
            }
            else if (h->kind == KIND_MOUSE && len >= 3)
            {
                MS.buttons = h->buf[0] & 7u;
                MS.dx += (s8)h->buf[1];
                MS.dy += (s8)h->buf[2]; /* USB: positive = down, same as the screen */
                if (len >= 4)
                    MS.wheel -= (s8)h->buf[3]; /* USB: positive = up; the desktop wants + = down */
            }
            else if (h->kind == KIND_KBD && len >= 3)
                kq_push(h->buf, len);
            h->errs = 0;
        }
        else
        {
            if (++h->errs > 16) /* unplugged or broken: give up on it */
            {
                h->active = 0;
                DEV[h->dev].gone = 1;
                return;
            }
            if (code == 6)
                ep_recover(x, h->slot, h->dci, &h->ring);
        }
        hid_queue(h);
        return;
    }
}

static void port_event(Xhci *x, const Trb *e)
{
    int p = (int)(e->d0 >> 24) - 1;
    if (p < 0 || p >= x->nports)
        return;
    volatile u8 *pr = x->ports + 0x10 * p;
    u32 v = rd32(pr, 0);
    wr32(pr, 0, (v & PORT_KEEP) | (v & PORT_CHG));
    if (!(v & PORT_CCS))
    {
        x->handled[p] = 0;
        for (int i = 0; i < MAX_DEV; i++) /* removed later, from usb_poll(): we may be inside a command here */
            if (DEV[i].used && DEV[i].x == x && DEV[i].loc.parent < 0 && DEV[i].loc.pport == p)
                DEV[i].gone = 1;
    }
}

static u32 ep_interval(int speed, u32 bint)
{
    if (speed == 1 || speed == 2) /* full / low speed: milliseconds -> exponent of 125 us */
    {
        u32 v = bint * 8, n = 0;
        while ((1u << n) < v)
            n++;
        return n < 3 ? 3 : (n > 10 ? 10 : n);
    }
    if (bint < 1)
        bint = 1;
    if (bint > 16)
        bint = 16;
    return bint - 1;
}

/* ---------- enumeration ---------- */

static void drop_slot(Xhci *x, u32 slot)
{
    command(x, 0, (10u << 10) | (slot << 24), 0); /* Disable Slot */
    x->dcbaa[slot] = 0;
}

static int dev_alloc(void)
{
    for (int i = 0; i < MAX_DEV; i++)
        if (!DEV[i].used)
        {
            Dev *d = &DEV[i];
            d->gone = 0;
            d->is_hub = 0;
            d->nports = 0;
            for (int k = 0; k <= HUB_PORTS; k++)
                d->handled[k] = d->tries[k] = 0;
            return i;
        }
    return -1;
}

/* Stop a device and everything plugged into it, and give its slot back.
 * Never call this from inside an event handler: it sends commands. */
static void dev_remove(int di)
{
    Dev *d = &DEV[di];
    if (!d->used)
        return;
    for (int k = 0; k < MAX_DEV; k++)
        if (DEV[k].used && DEV[k].loc.parent == di)
            dev_remove(k);
    for (int i = 0; i < MAX_HID; i++)
    {
        Hid *h = &HID[i];
        if (!h->active || h->dev != di)
            continue;
        h->active = 0;
        if (h->kind == KIND_MOUSE)
            MS.buttons = 0;
        else
            kq_push_empty(); /* release whatever was held */
    }
    drop_slot(d->x, (u32)d->slot);
    for (int i = 0; i < MAX_MSC; i++)
    {
        Msc *m = &MSC[i];
        if (!m->active || m->dev != di)
            continue;
        m->active = 0;
        usb_msc_detach(&m->msc); /* its block device (and partitions) now fail; volume_sync() lets go of the drive */
        pmm_free_contiguous(m->buf_phys, MSC_BUF_PAGES); /* the slot is disabled: the controller no longer writes there */
        pmm_free_page(m->rin.phys);
        pmm_free_page(m->rout.phys);
    }
    d->used = 0;
}

/* The slot context both Address Device and Configure Endpoint need. */
static void fill_slot(volatile u32 *c, const Loc *L, int entries, int hub_ports, int think_time)
{
    c[0] = L->route | ((u32)L->speed << 20) | (hub_ports ? (1u << 26) : 0) | ((u32)entries << 27);
    c[1] = ((u32)(L->root + 1) << 16) | ((u32)hub_ports << 24);
    c[2] = (u32)L->tt_slot | ((u32)L->tt_port << 8) | ((u32)think_time << 16);
}

static int bring_up(Xhci *x, const Loc *L);
static void hub_scan(Xhci *x, int di);

/* ---------- hubs ---------- */

static int port_status(Xhci *x, Dev *d, int port, u32 *st, u32 *ch)
{
    int code = control(x, d->slot, &d->ep0, 0xA3, 0, 0, (u16)port, 4, 1); /* GET_STATUS, class, other */
    if (code == 6)
        ep_recover(x, d->slot, 1, &d->ep0);
    if (!ctrl_ok(code))
        return 0;
    *st = x->dma[0] | ((u32)x->dma[1] << 8);
    *ch = x->dma[2] | ((u32)x->dma[3] << 8);
    return 1;
}

static void port_feature(Xhci *x, Dev *d, int set, int feature, int port)
{
    int code = control(x, d->slot, &d->ep0, 0x23, set ? 3 : 1, (u16)feature, (u16)port, 0, 0);
    if (code == 6)
        ep_recover(x, d->slot, 1, &d->ep0);
}

#define HUB_PORT_RESET 4
#define HUB_PORT_POWER 8
#define HUB_C_CONNECTION 16
#define HUB_C_RESET 20

/* Reset a hub port and bring up what is on it. 0 = failed, worth another try. */
static int hub_enum_child(Xhci *x, int di, int port)
{
    Dev *d = &DEV[di];
    u32 st = 0, ch = 0;
    port_feature(x, d, 1, HUB_PORT_RESET, port);
    int done = 0;
    for (int i = 0; i < 60 && !done; i++)
    {
        delay_ms(10);
        if (port_status(x, d, port, &st, &ch) && (ch & (1u << 4)))
            done = 1;
    }
    if (!done)
        return 0;
    port_feature(x, d, 0, HUB_C_RESET, port);
    port_feature(x, d, 0, HUB_C_CONNECTION, port);
    if (!port_status(x, d, port, &st, &ch) || !(st & 1) || !(st & 2)) /* connected and enabled */
        return 0;
    delay_ms(20);

    Loc L;
    L.root = d->loc.root;
    L.route = d->loc.route | ((u32)port << (4 * d->loc.depth));
    L.depth = d->loc.depth + 1;
    L.speed = d->loc.speed == 4 ? 4 : ((st & (1u << 9)) ? 2 : ((st & (1u << 10)) ? 3 : 1));
    L.parent = di;
    L.pport = port;
    L.tt_slot = L.tt_port = 0;
    if (L.speed <= 2) /* low / full speed: needs the transaction translator of the nearest high speed hub */
    {
        if (d->loc.speed == 3)
        {
            L.tt_slot = d->slot;
            L.tt_port = port;
        }
        else
        {
            L.tt_slot = d->loc.tt_slot;
            L.tt_port = d->loc.tt_port;
        }
    }
    return bring_up(x, &L);
}

static void hub_remove_children(int di, int port)
{
    for (int k = 0; k < MAX_DEV; k++)
        if (DEV[k].used && DEV[k].loc.parent == di && DEV[k].loc.pport == port)
            dev_remove(k);
}

/* Look at every port of a hub: new devices get brought up, vanished ones removed. */
static void hub_scan(Xhci *x, int di)
{
    for (int port = 1; port <= DEV[di].nports; port++)
    {
        Dev *d = &DEV[di];
        if (!d->used || d->gone)
            return;
        u32 st, ch;
        if (!port_status(x, d, port, &st, &ch))
            continue;
        if (ch & 1) /* connection changed: whatever was there is gone */
        {
            port_feature(x, d, 0, HUB_C_CONNECTION, port);
            if (d->handled[port])
            {
                hub_remove_children(di, port);
                d->handled[port] = 0;
            }
        }
        if (!(st & 1))
        {
            if (d->handled[port])
            {
                hub_remove_children(di, port);
                d->handled[port] = 0;
            }
            d->tries[port] = 0;
            continue;
        }
        if (d->handled[port])
            continue;
        d->handled[port] = 1;
        if (!hub_enum_child(x, di, port) && ++d->tries[port] < 3)
            d->handled[port] = 0; /* try again at the next scan */
    }
}

/* The device in DEV[di] is a hub: configure it, switch its ports on and look at them. */
static int hub_setup(Xhci *x, int di, u32 cfg_value)
{
    Dev *d = &DEV[di];
    int slot = d->slot, ss = d->loc.speed == 4;
    if (d->loc.depth >= 5) /* a route string has room for five tiers only */
    {
        d->used = 0;
        drop_slot(x, (u32)slot);
        return 1;
    }
    int ok = ctrl_ok(control(x, slot, &d->ep0, 0x00, 9, (u16)cfg_value, 0, 0, 0)); /* SET_CONFIGURATION */
    if (ok && ss)
        control(x, slot, &d->ep0, 0x20, 12, (u16)d->loc.depth, 0, 0, 0); /* SET_HUB_DEPTH */
    if (ok)
        ok = ctrl_ok(control(x, slot, &d->ep0, 0xA0, 6, ss ? 0x2A00 : 0x2900, 0, 8, 1)); /* hub descriptor */
    if (!ok)
    {
        d->used = 0;
        drop_slot(x, (u32)slot);
        return 0;
    }
    int n = x->dma[2];
    u32 chars = x->dma[3] | ((u32)x->dma[4] << 8);
    u32 power_good = x->dma[5]; /* in units of 2 ms */
    if (n < 1)
    {
        d->used = 0;
        drop_slot(x, (u32)slot);
        return 1;
    }
    if (n > HUB_PORTS)
        n = HUB_PORTS;

    /* Tell the controller it is a hub (so it knows the ports and the transaction translator). */
    inctx_clear(x);
    volatile u32 *c = ctx_at(x, 0);
    c[1] = 1; /* add: slot context only */
    fill_slot(ctx_at(x, 1), &d->loc, 1, n, (int)((chars >> 5) & 3));
    if (command(x, x->inctx_phys, (12u << 10) | ((u32)slot << 24), 0) != 1)
        command(x, x->inctx_phys, (13u << 10) | ((u32)slot << 24), 0);

    d->is_hub = 1;
    d->nports = n;
    for (int p = 1; p <= n; p++)
        port_feature(x, d, 1, HUB_PORT_POWER, p);
    delay_ms(power_good * 2 > 100 ? power_good * 2 : 100);
    hub_scan(x, di);
    return 1;
}

/* ---------- mass storage ---------- */

static void pump(void);

static int msc_gone(const Msc *m) { return !DEV[m->dev].used || DEV[m->dev].gone; }

/* Wait for the transfer event of endpoint `dci` of this device. 0 on timeout or when the device is unplugged. */
static int msc_wait(Msc *m, int dci, unsigned timeout_ms, int *code, u64 *ptr, u32 *resid)
{
    int use_timer = timer_ready();
    unsigned long long t0 = use_timer ? timer_ms() : 0;
    for (unsigned i = 0;; i++)
    {
        Trb e;
        if (ev_next(m->x, &e))
        {
            if (((e.d3 >> 10) & 63) == 32 && (int)(e.d3 >> 24) == m->slot && (int)((e.d3 >> 16) & 31) == dci)
            {
                *code = (int)(e.d2 >> 24);
                *resid = e.d2 & 0xFFFFFF;
                *ptr = ((u64)e.d1 << 32) | e.d0;
                return 1;
            }
            dispatch(m->x, &e);
            continue;
        }
        if (msc_gone(m))
            return 0;
        udelay(100);
        if (use_timer ? timer_ms() - t0 >= timeout_ms : i >= timeout_ms * 10)
            return 0;
    }
}

/* A transfer that never finished: stop the endpoint and move its ring past the unfinished TRBs. */
static void ep_abort(Xhci *x, int slot, int dci, Ring *r)
{
    command(x, 0, (15u << 10) | ((u32)dci << 16) | ((u32)slot << 24), 0); /* Stop Endpoint */
    ep_recover(x, slot, dci, r);
}

/* Move `len` bytes between the bounce buffer and the device on the bulk IN / OUT endpoint. The data is cut into
 * TRBs that never cross a 64 KiB boundary (an xHCI rule); they are chained into one TD whose last TRB raises the
 * interrupt. TD Size tells the controller how many packets are still to come after each TRB. */
static int msc_xfer(Msc *m, int in, unsigned len, unsigned *got)
{
    Xhci *x = m->x;
    Ring *r = in ? &m->rin : &m->rout;
    int dci = in ? m->in_dci : m->out_dci;
    unsigned mps = in ? m->in_mps : m->out_mps;
    if (got)
        *got = 0;
    if (msc_gone(m) || len == 0 || len > MSC_BUF_PAGES * 4096u)
        return MSC_ERR;

    unsigned total_pk = (len + mps - 1) / mps;
    u64 addr = m->buf_phys, trb_at[3];
    unsigned chunk_len[3], nt = 0, left = len, queued = 0;
    while (left && nt < 3)
    {
        unsigned chunk = 0x10000u - (unsigned)(addr & 0xFFFF);
        if (chunk > left)
            chunk = left;
        left -= chunk;
        queued += chunk;
        unsigned td = left ? total_pk - (queued + mps - 1) / mps : 0;
        if (td > 31)
            td = 31;
        /* Normal TRB; interrupt on short packet; chain to the next TRB, or (last one) interrupt on completion */
        ring_put(r, addr, chunk | (td << 17), (1u << 10) | (1u << 2) | (left ? (1u << 4) : (1u << 5)));
        trb_at[nt] = r->last;
        chunk_len[nt++] = chunk;
        addr += chunk;
    }
    mb();
    wr32(x->db, (u32)m->slot * 4, (u32)dci);

    int code = 0;
    u64 ptr = 0;
    u32 resid = 0;
    if (!msc_wait(m, dci, 8000, &code, &ptr, &resid))
    {
        if (!msc_gone(m))
            ep_abort(x, m->slot, dci, r);
        if (++m->errs > 3)
            DEV[m->dev].gone = 1;
        return MSC_ERR;
    }
    if (code == 1 || code == 13) /* success, or a short packet */
    {
        unsigned done = 0, moved = len;
        for (unsigned k = 0; k < nt; k++) /* the event names the TRB it ended in */
        {
            if (ptr == trb_at[k])
            {
                moved = done + (chunk_len[k] > resid ? chunk_len[k] - resid : 0);
                break;
            }
            done += chunk_len[k];
        }
        if (got)
            *got = moved;
        m->errs = 0;
        return MSC_OK;
    }
    ep_recover(x, m->slot, dci, r); /* a STALL halts the endpoint; any other error: start again behind the dead TRBs */
    if (code == 6)
        return MSC_STALL;
    if (++m->errs > 3)
        DEV[m->dev].gone = 1;
    return MSC_ERR;
}

static int msc_bulk_out(void *ctx, const void *buf, unsigned len)
{
    Msc *m = (Msc *)ctx;
    if (len > MSC_BUF_PAGES * 4096u)
        return MSC_ERR;
    const volatile u8 *s = (const volatile u8 *)buf;
    for (unsigned i = 0; i < len; i++)
        m->buf[i] = s[i];
    return msc_xfer(m, 0, len, 0);
}

static int msc_bulk_in(void *ctx, void *buf, unsigned len, unsigned *got)
{
    Msc *m = (Msc *)ctx;
    unsigned n = 0;
    int r = msc_xfer(m, 1, len, &n);
    if (n > len)
        n = len;
    volatile u8 *d = (volatile u8 *)buf;
    for (unsigned i = 0; i < n; i++)
        d[i] = m->buf[i];
    *got = n;
    return r;
}

/* CLEAR_FEATURE(ENDPOINT_HALT) tells the device its endpoint works again (and resets its data toggle; the
 * controller's side was reset in msc_xfer). */
static int msc_clear_halt(void *ctx, int in)
{
    Msc *m = (Msc *)ctx;
    if (msc_gone(m))
        return -1;
    Dev *d = &DEV[m->dev];
    int code = control(m->x, m->slot, &d->ep0, 0x02, 1, 0, (u16)(in ? m->in_addr : m->out_addr), 0, 0);
    if (code == 6)
        ep_recover(m->x, m->slot, 1, &d->ep0);
    return ctrl_ok(code) ? 0 : -1;
}

/* Bulk-Only Mass Storage Reset (class request 0xFF on the interface), then both endpoints are cleared. */
static int msc_reset(void *ctx)
{
    Msc *m = (Msc *)ctx;
    if (msc_gone(m))
        return -1;
    Dev *d = &DEV[m->dev];
    int code = control(m->x, m->slot, &d->ep0, 0x21, 0xFF, 0, (u16)m->iface, 0, 0);
    if (code == 6)
        ep_recover(m->x, m->slot, 1, &d->ep0);
    msc_clear_halt(ctx, 1);
    msc_clear_halt(ctx, 0);
    return ctrl_ok(code) ? 0 : -1;
}

/* Waiting must not let the event ring fill up (a mouse reports 100+ times a second), so handle events meanwhile. */
static void msc_delay(void *ctx, unsigned ms)
{
    (void)ctx;
    for (unsigned i = 0; i < ms; i++)
    {
        udelay(1000);
        pump();
    }
}

static int msc_alive(void *ctx) { return !msc_gone((Msc *)ctx); }

/* The bulk endpoints of the first bulk-only mass-storage interface (alternate setting 0). */
typedef struct
{
    int iface, in_addr, out_addr;
    unsigned in_mps, out_mps, in_burst, out_burst;
} MscDesc;

/* Looks through the configuration descriptor in x->dma (`total` bytes): interface class 8 (mass storage),
 * subclass 6 (SCSI commands), protocol 0x50 (bulk-only). A SuperSpeed endpoint is followed by a companion
 * descriptor (type 0x30) that holds its burst size. UAS (protocol 0x62) is not supported. */
static int msc_find(Xhci *x, u32 total, MscDesc *md)
{
    int in_msc = 0;
    md->iface = -1;
    md->in_addr = md->out_addr = 0;
    for (u32 off = 0; off + 2 <= total && x->dma[off] >= 2; off += x->dma[off])
    {
        u8 len = x->dma[off], type = x->dma[off + 1];
        if (off + len > total)
            break;
        if (type == 4 && len >= 9)
        {
            if (md->iface >= 0 && md->in_addr && md->out_addr)
                break; /* the interface we want is complete */
            in_msc = x->dma[off + 3] == 0 && x->dma[off + 5] == 8 && x->dma[off + 6] == 6 && x->dma[off + 7] == 0x50;
            if (in_msc)
            {
                md->iface = x->dma[off + 2];
                md->in_addr = md->out_addr = 0;
            }
        }
        else if (type == 5 && len >= 7 && in_msc && (x->dma[off + 3] & 3) == 2 && (x->dma[off + 2] & 15)) /* bulk */
        {
            u8 addr = x->dma[off + 2];
            unsigned mps = (x->dma[off + 4] | (x->dma[off + 5] << 8)) & 0x7FF, burst = 0;
            u32 nx = off + len;
            if (nx + 3 <= total && x->dma[nx] >= 6 && x->dma[nx + 1] == 0x30)
                burst = x->dma[nx + 2];
            if (mps == 0)
                continue;
            if (addr & 0x80)
            {
                md->in_addr = addr;
                md->in_mps = mps;
                md->in_burst = burst;
            }
            else
            {
                md->out_addr = addr;
                md->out_mps = mps;
                md->out_burst = burst;
            }
        }
    }
    return md->iface >= 0 && md->in_addr && md->out_addr;
}

/* Configure the two bulk endpoints of the device in DEV[di], select its configuration and hand it to usb_msc.c.
 * Returns 1 if the device was handled (kept, or given up for good), 0 if it is worth another try. */
static int msc_setup(Xhci *x, int di, const Loc *L, u32 cfg_value, const MscDesc *md)
{
    Dev *d = &DEV[di];
    u32 slot = (u32)d->slot;
    Msc *m = 0;
    for (int i = 0; i < MAX_MSC && !m; i++)
        if (!MSC[i].active)
            m = &MSC[i];
    if (!m) /* too many drives: leave this one alone */
    {
        d->used = 0;
        drop_slot(x, slot);
        return 1;
    }
    m->x = x;
    m->slot = d->slot;
    m->dev = di;
    m->iface = md->iface;
    m->in_addr = md->in_addr;
    m->out_addr = md->out_addr;
    m->in_mps = md->in_mps;
    m->out_mps = md->out_mps;
    m->in_dci = (md->in_addr & 15) * 2 + 1;
    m->out_dci = (md->out_addr & 15) * 2;
    m->errs = 0;

    u64 bounce = pmm_alloc_contiguous_tag(MSC_BUF_PAGES, PMM_TAG_USB);
    if (bounce && !x->ac64 && bounce + (u64)MSC_BUF_PAGES * 4096 > 0x100000000ULL)
    {
        pmm_free_contiguous(bounce, MSC_BUF_PAGES);
        bounce = 0;
    }
    int ok = bounce != 0;
    int rings = 0;
    if (ok)
    {
        m->buf_phys = bounce;
        m->buf = (volatile u8 *)bounce;
        ok = ring_init(x, &m->rin) == 0;
        if (ok)
        {
            rings = 1;
            ok = ring_init(x, &m->rout) == 0;
        }
        if (ok)
            rings = 2;
    }

    /* Configure Endpoint: both bulk endpoints in one go (bulk IN = type 6, bulk OUT = type 2) */
    if (ok)
    {
        int maxdci = m->in_dci > m->out_dci ? m->in_dci : m->out_dci;
        inctx_clear(x);
        volatile u32 *c = ctx_at(x, 0);
        c[1] = 1u | (1u << m->in_dci) | (1u << m->out_dci);
        fill_slot(ctx_at(x, 1), L, maxdci, 0, 0);
        c = ctx_at(x, m->in_dci + 1);
        c[1] = (3u << 1) | (6u << 3) | (md->in_burst << 8) | (md->in_mps << 16);
        c[2] = (u32)m->rin.phys | 1;
        c[3] = (u32)(m->rin.phys >> 32);
        c[4] = md->in_mps;
        c = ctx_at(x, m->out_dci + 1);
        c[1] = (3u << 1) | (2u << 3) | (md->out_burst << 8) | (md->out_mps << 16);
        c[2] = (u32)m->rout.phys | 1;
        c[3] = (u32)(m->rout.phys >> 32);
        c[4] = md->out_mps;
        ok = command(x, x->inctx_phys, (12u << 10) | (slot << 24), 0) == 1 &&
             ctrl_ok(control(x, (int)slot, &d->ep0, 0x00, 9, (u16)cfg_value, 0, 0, 0)); /* SET_CONFIGURATION */
    }

    UsbMscIo io;
    io.bulk_out = msc_bulk_out;
    io.bulk_in = msc_bulk_in;
    io.clear_halt = msc_clear_halt;
    io.reset = msc_reset;
    io.delay_ms = msc_delay;
    io.alive = msc_alive;
    io.max_xfer = MSC_BUF_PAGES * 4096u;
    io.ctx = m;
    if (ok && usb_msc_attach(&m->msc, &io) >= 0)
    {
        m->active = 1; /* from here dev_remove() takes care of it */
        return 1;
    }

    if (rings >= 1)
        pmm_free_page(m->rin.phys);
    if (rings >= 2)
        pmm_free_page(m->rout.phys);
    if (bounce)
        pmm_free_contiguous(bounce, MSC_BUF_PAGES);
    d->used = 0;
    drop_slot(x, slot);
    return 0;
}

/* ---------- devices ---------- */

/* Brings up a device (already reset and enabled on its port) and, if it is a hub or has a
 * HID boot interface, keeps it. Returns 0 if it failed in a way worth retrying, 1 when the
 * device was handled (kept, or something we do not use). */
static int bring_up(Xhci *x, const Loc *L)
{
    int speed = L->speed;
    u32 mps = speed == 4 ? 512 : (speed == 3 ? 64 : 8);

    u32 slot = 0;
    if (command(x, 0, 9u << 10, &slot) != 1 || slot == 0 || (int)slot > x->nslots) /* Enable Slot */
        return 0;
    int di = dev_alloc();
    if (di < 0)
    {
        drop_slot(x, slot);
        return 1;
    }
    Dev *d = &DEV[di];
    d->used = 1;
    d->x = x;
    d->slot = (int)slot;
    d->loc = *L;
#define FAIL           \
    do                 \
    {                  \
        d->used = 0;   \
        drop_slot(x, slot); \
        return 0;      \
    } while (0)

    u64 dev_phys;
    volatile u8 *dev = dma_page(x, &dev_phys);
    if (!dev || ring_init(x, &d->ep0))
        FAIL;
    x->dcbaa[slot] = dev_phys;

    /* Address Device: slot context + endpoint 0 */
    inctx_clear(x);
    volatile u32 *c = ctx_at(x, 0);
    c[1] = 3; /* add: slot context, endpoint 0 */
    fill_slot(ctx_at(x, 1), L, 1, 0, 0);
    c = ctx_at(x, 2);
    c[1] = (3u << 1) | (4u << 3) | (mps << 16); /* error count 3, type control */
    c[2] = (u32)d->ep0.phys | 1;
    c[3] = (u32)(d->ep0.phys >> 32);
    c[4] = 8;
    if (command(x, x->inctx_phys, (11u << 10) | (slot << 24), 0) != 1)
        FAIL;
    delay_ms(10);

    /* First 8 bytes of the device descriptor tell the real packet size of endpoint 0. */
    if (!ctrl_ok(control(x, (int)slot, &d->ep0, 0x80, 6, 0x0100, 0, 8, 1)))
        FAIL;
    u32 dclass = x->dma[4];
    u32 real = x->dma[7];
    if (speed == 4)
        real = (real >= 9 && real <= 10) ? (1u << real) : 0;
    if (real && real != mps && (real == 8 || real == 16 || real == 32 || real == 64 || real == 512))
    {
        inctx_clear(x);
        c = ctx_at(x, 0);
        c[1] = 2; /* evaluate: endpoint 0 only */
        c = ctx_at(x, 2);
        c[1] = (3u << 1) | (4u << 3) | (real << 16);
        command(x, x->inctx_phys, (13u << 10) | (slot << 24), 0);
        mps = real;
    }

    /* Configuration descriptor: first its length, then all of it. */
    if (!ctrl_ok(control(x, (int)slot, &d->ep0, 0x80, 6, 0x0200, 0, 9, 1)))
        FAIL;
    u32 total = x->dma[2] | ((u32)x->dma[3] << 8);
    if (total > 2048)
        total = 2048;
    if (total < 9 || !ctrl_ok(control(x, (int)slot, &d->ep0, 0x80, 6, 0x0200, 0, (u16)total, 1)))
        FAIL;
    u32 cfg_value = x->dma[5];

    if (dclass == 9)
        return hub_setup(x, di, cfg_value);

    /* A pen drive or external disk (mass storage, bulk-only)? Then it is handed to usb_msc.c instead. */
    MscDesc md;
    if (msc_find(x, total, &md))
        return msc_setup(x, di, L, cfg_value, &md);

    /* Look for boot-protocol keyboard / mouse interfaces (at most one of each kind). */
    struct
    {
        int kind, iface, addr, mps, interval, rdlen;
    } F[2];
    int nf = 0, cur = -1, cur_kind = 0, cur_rdlen = 0;
    for (u32 off = 0; off + 2 <= total && x->dma[off] >= 2; off += x->dma[off])
    {
        u8 len = x->dma[off], type = x->dma[off + 1];
        if (off + len > total)
            break;
        if (type == 4 && len >= 9) /* interface: class 3 (HID), subclass 1 (boot), protocol 1 keyboard / 2 mouse */
        {
            cur = x->dma[off + 2];
            cur_kind = 0;
            cur_rdlen = 0;
            if (x->dma[off + 5] == 3 && x->dma[off + 6] == 1)
                cur_kind = x->dma[off + 7] == 1 ? KIND_KBD : (x->dma[off + 7] == 2 ? KIND_MOUSE : 0);
            for (int i = 0; i < nf; i++)
                if (F[i].kind == cur_kind)
                    cur_kind = 0;
        }
        else if (type == 0x21 && len >= 9 && cur_kind == KIND_MOUSE && x->dma[off + 6] == 0x22)
            cur_rdlen = x->dma[off + 7] | (x->dma[off + 8] << 8); /* HID descriptor: length of the report descriptor */
        else if (type == 5 && len >= 7 && cur_kind && nf < 2) /* first endpoint of that interface */
        {
            if ((x->dma[off + 2] & 0x80) && (x->dma[off + 3] & 3) == 3 && (x->dma[off + 2] & 15)) /* interrupt IN */
            {
                F[nf].kind = cur_kind;
                F[nf].iface = cur;
                F[nf].addr = x->dma[off + 2] & 15;
                F[nf].mps = (x->dma[off + 4] | (x->dma[off + 5] << 8)) & 0x7FF;
                F[nf].interval = x->dma[off + 6];
                F[nf].rdlen = cur_rdlen;
                if (F[nf].mps >= 3)
                    nf++;
                cur_kind = 0;
            }
        }
    }

    /* Reserve a HID entry for each one found (kind -1 = reserved while we set it up) */
    Hid *hid[2] = {0, 0};
    int nh = 0;
    for (int i = 0; i < nf; i++)
    {
        Hid *h = 0;
        for (int k = 0; k < MAX_HID && !h; k++)
            if (!HID[k].active && HID[k].kind >= 0)
                h = &HID[k];
        if (!h)
            break;
        h->kind = -1;
        h->buf = dma_page(x, &h->buf_phys);
        if (!h->buf || ring_init(x, &h->ring))
        {
            h->kind = 0;
            break;
        }
        hid[nh++] = h;
    }
    if (nh == 0)
    {
        d->used = 0;
        drop_slot(x, slot); /* storage, a webcam, ... : not ours */
        return 1;
    }

    /* Configure Endpoint: add every interrupt endpoint in one go */
    int maxdci = 1;
    inctx_clear(x);
    c = ctx_at(x, 0);
    c[1] = 1;
    for (int i = 0; i < nh; i++)
    {
        int dci = F[i].addr * 2 + 1;
        if (dci > maxdci)
            maxdci = dci;
        c[1] |= 1u << dci;
    }
    fill_slot(ctx_at(x, 1), L, maxdci, 0, 0);
    for (int i = 0; i < nh; i++)
    {
        int dci = F[i].addr * 2 + 1;
        c = ctx_at(x, dci + 1);
        c[0] = ep_interval(speed, (u32)F[i].interval) << 16;
        c[1] = (3u << 1) | (7u << 3) | ((u32)F[i].mps << 16); /* type interrupt IN */
        c[2] = (u32)hid[i]->ring.phys | 1;
        c[3] = (u32)(hid[i]->ring.phys >> 32);
        c[4] = (u32)F[i].mps | ((u32)F[i].mps << 16);
    }
    int ok = command(x, x->inctx_phys, (12u << 10) | (slot << 24), 0) == 1 &&
             ctrl_ok(control(x, (int)slot, &d->ep0, 0x00, 9, (u16)cfg_value, 0, 0, 0)); /* SET_CONFIGURATION */
    if (!ok)
    {
        for (int i = 0; i < nh; i++)
            hid[i]->kind = 0;
        FAIL;
    }
    for (int i = 0; i < nh; i++)
    {
        /* A mouse: try its report descriptor first. The boot report has only 3 bytes (buttons, X, Y), so a
         * scroll wheel needs the report protocol. If the descriptor cannot be read or understood we stay in
         * boot protocol exactly as before. Keyboards always use the boot protocol. */
        hid[i]->rp = 0;
        int rdlen = F[i].rdlen;
        if (F[i].kind == KIND_MOUSE && rdlen >= 16 && rdlen <= RDESC_MAX &&
            ctrl_ok(control(x, (int)slot, &d->ep0, 0x81, 6, 0x2200, (u16)F[i].iface, (u16)rdlen, 1))) /* GET_DESCRIPTOR: report */
        {
            for (int k = 0; k < rdlen; k++)
                RDESC[k] = x->dma[k];
            if (hid_find_mouse(RDESC, (unsigned)rdlen, &hid[i]->hm))
                hid[i]->rp = 1;
        }
        u16 proto = hid[i]->rp ? 1 : 0; /* SET_PROTOCOL: 1 = report, 0 = boot */
        if (control(x, (int)slot, &d->ep0, 0x21, 0x0B, proto, (u16)F[i].iface, 0, 0) == 6)
            ep_recover(x, (int)slot, 1, &d->ep0);
    }

    for (int i = 0; i < nh; i++)
    {
        Hid *h = hid[i];
        h->x = x;
        h->slot = (int)slot;
        h->dci = F[i].addr * 2 + 1;
        h->dev = di;
        h->rlen = F[i].mps > 64 ? 64 : F[i].mps;
        h->errs = 0;
        h->kind = F[i].kind;
        if (h->kind == KIND_MOUSE)
            MS.buttons = 0;
        h->active = 1;
        hid_queue(h);
    }
    return 1;
#undef FAIL
}

/* Brings up the device on root port p. 0 = failed, worth another try. */
static int enum_port(Xhci *x, int p)
{
    volatile u8 *pr = x->ports + 0x10 * p;
    u32 v = rd32(pr, 0);
    if (!(v & PORT_CCS))
        return 1;

    /* A USB 2 port must be reset to become enabled; a USB 3 port is usually enabled already. */
    if (!(v & PORT_PED))
    {
        wr32(pr, 0, (v & PORT_KEEP) | PORT_PR);
        if (!WAIT_MS(rd32(pr, 0) & PORT_PRC, 500))
            return 0;
        v = rd32(pr, 0);
        wr32(pr, 0, (v & PORT_KEEP) | (v & PORT_CHG));
        delay_ms(20);
    }
    v = rd32(pr, 0);
    if (!(v & PORT_CCS) || !(v & PORT_PED))
        return 0;
    int speed = (int)((v >> 10) & 15);
    if (speed < 1)
        return 0;

    Loc L;
    L.root = p;
    L.route = 0;
    L.depth = 0;
    L.speed = speed > 4 ? 4 : speed; /* SuperSpeed+ behaves like SuperSpeed for what we do */
    L.parent = -1;
    L.pport = p;
    L.tt_slot = L.tt_port = 0;
    return bring_up(x, &L);
}

static void rescan(Xhci *x)
{
    for (int p = 0; p < x->nports; p++)
    {
        volatile u8 *pr = x->ports + 0x10 * p;
        u32 v = rd32(pr, 0);
        if (v & PORT_CHG)
            wr32(pr, 0, (v & PORT_KEEP) | (v & PORT_CHG));
        if (!(v & PORT_CCS))
        {
            for (int k = 0; k < MAX_DEV; k++)
                if (DEV[k].used && DEV[k].x == x && DEV[k].loc.parent < 0 && DEV[k].loc.pport == p)
                    dev_remove(k);
            x->handled[p] = 0;
            x->tries[p] = 0;
            continue;
        }
        if (x->handled[p])
            continue;
        x->handled[p] = 1;
        if (!enum_port(x, p) && ++x->tries[p] < 3)
            x->handled[p] = 0; /* try again at the next scan */
    }
}

/* ---------- controller bring-up ---------- */

static int intel_with_port_routing(u32 id)
{
    u32 dev = id >> 16;
    return (id & 0xFFFF) == 0x8086 && (dev == 0x1E31 || dev == 0x8C31 || dev == 0x9C31 || dev == 0x9CB1);
}

static int xhci_setup(Xhci *x, u32 bus, u32 dev, u32 fn)
{
    u32 id = pci_read(bus, dev, fn, 0);
    u32 cmdreg = pci_read(bus, dev, fn, 4);
    pci_write(bus, dev, fn, 4, (cmdreg & 0xFFFF) | 0x6); /* memory space + bus master */

    u32 bar = pci_read(bus, dev, fn, 0x10);
    if (bar & 1)
        return -1;
    u64 base = bar & ~0xFULL;
    if (((bar >> 1) & 3) == 2)
        base |= (u64)pci_read(bus, dev, fn, 0x14) << 32;
    if (!base)
        return -1;

    /* Panther / Lynx Point chipsets: hand the USB ports over from EHCI to xHCI. */
    if (intel_with_port_routing(id))
    {
        pci_write(bus, dev, fn, 0xD8, pci_read(bus, dev, fn, 0xDC));
        pci_write(bus, dev, fn, 0xD0, pci_read(bus, dev, fn, 0xD4));
    }

    volatile u8 *cap = (volatile u8 *)base;
    u32 hcs1 = rd32(cap, 4), hcs2 = rd32(cap, 8), hcc1 = rd32(cap, 0x10);
    u32 caplen = cap[0];
    if (hcs1 == 0xFFFFFFFFu || caplen < 0x10 || caplen > 0x80)
        return -1;
    x->op = cap + caplen;
    x->rt = cap + (rd32(cap, 0x18) & ~0x1Fu);
    x->db = cap + (rd32(cap, 0x14) & ~3u);
    x->ports = x->op + 0x400;
    x->ac64 = (int)(hcc1 & 1);
    x->csz = (hcc1 & 4) ? 64 : 32;
    x->nports = (int)(hcs1 >> 24);
    x->nslots = (int)(hcs1 & 0xFF);
    if (x->nports > 255)
        x->nports = 255;
    if (x->nslots > 64)
        x->nslots = 64;
    if (x->nslots < 1 || x->nports < 1)
        return -1;

    /* The firmware may still own the controller (USB keyboard emulation): ask it to let go. */
    u32 xecp = (hcc1 >> 16) & 0xFFFF;
    for (u32 off = xecp * 4, guard = 0; xecp && guard < 64; guard++)
    {
        u32 h = rd32(cap, off);
        if ((h & 0xFF) == 1) /* USB Legacy Support */
        {
            if (h & (1u << 16))
            {
                wr32(cap, off, h | (1u << 24));
                WAIT_MS(!(rd32(cap, off) & (1u << 16)), 1000);
            }
            u32 s = rd32(cap, off + 4);
            s &= ~((3u << 1) | (0xFFu << 5) | (7u << 17)); /* no SMIs any more */
            s |= 7u << 29;                                 /* acknowledge pending ones */
            wr32(cap, off + 4, s);
            break;
        }
        u32 nxt = (h >> 8) & 0xFF;
        if (!nxt)
            break;
        off += nxt * 4;
    }

    /* Stop, then reset */
    if (!(rd32(x->op, 4) & 1))
    {
        wr32(x->op, 0, rd32(x->op, 0) & ~1u);
        WAIT_MS(rd32(x->op, 4) & 1, 200);
    }
    wr32(x->op, 0, 2);
    if (!WAIT_MS(!(rd32(x->op, 0) & 2) && !(rd32(x->op, 4) & (1u << 11)), 1000))
        return -1;
    delay_ms(5);

    wr32(x->op, 0x38, (u32)x->nslots); /* CONFIG: enabled slots */

    /* Device context base array (+ scratchpad buffers the controller wants for itself) */
    u64 dcbaa_phys;
    x->dcbaa = (volatile u64 *)dma_page(x, &dcbaa_phys);
    if (!x->dcbaa)
        return -1;
    u32 nscratch = ((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F);
    if (nscratch)
    {
        u64 arr_phys;
        volatile u64 *arr = (volatile u64 *)dma_page(x, &arr_phys);
        if (!arr || nscratch > 512)
            return -1;
        for (u32 i = 0; i < nscratch; i++)
        {
            u64 sp;
            if (!dma_page(x, &sp))
                return -1;
            arr[i] = sp;
        }
        x->dcbaa[0] = arr_phys;
    }
    wr64(x->op, 0x30, dcbaa_phys);

    /* Command ring */
    if (ring_init(x, &x->cmd))
        return -1;
    wr64(x->op, 0x18, x->cmd.phys | 1); /* CRCR: ring cycle state = 1 */

    /* Event ring: one segment */
    u64 erst_phys;
    volatile u8 *erst = dma_page(x, &erst_phys);
    volatile u8 *evpg = dma_page(x, &x->ev_phys);
    if (!erst || !evpg)
        return -1;
    x->ev = (Trb *)evpg;
    x->ev_idx = 0;
    x->ev_cycle = 1;
    *(volatile u64 *)erst = x->ev_phys;
    *(volatile u32 *)(erst + 8) = EV_N;
    wr32(x->rt, 0x28, 1);             /* ERSTSZ */
    wr64(x->rt, 0x38, x->ev_phys);    /* ERDP */
    wr64(x->rt, 0x30, erst_phys);     /* ERSTBA: last, this starts the ring */

    x->dma = dma_page(x, &x->dma_phys);
    x->inctx = dma_page(x, &x->inctx_phys);
    if (!x->dma || !x->inctx)
        return -1;

    wr32(x->op, 0, 1); /* run (interrupts stay off: we poll the event ring) */
    if (!WAIT_MS(!(rd32(x->op, 4) & 1), 200))
        return -1;

    /* Make sure every port has power, then give devices time to show up. */
    for (int p = 0; p < x->nports; p++)
    {
        volatile u8 *pr = x->ports + 0x10 * p;
        u32 v = rd32(pr, 0);
        if (!(v & PORT_PP))
            wr32(pr, 0, (v & PORT_KEEP) | PORT_PP);
    }
    delay_ms(200);
    return 0;
}

int usb_init(void)
{
    NCTRL = 0;
    for (u32 bus = 0; bus < 256 && NCTRL < MAX_CTRL; bus++)
        for (u32 dev = 0; dev < 32 && NCTRL < MAX_CTRL; dev++)
        {
            if ((pci_read(bus, dev, 0, 0) & 0xFFFF) == 0xFFFF)
                continue;
            u32 nfn = (pci_read(bus, dev, 0, 0x0C) & 0x800000) ? 8 : 1;
            for (u32 fn = 0; fn < nfn && NCTRL < MAX_CTRL; fn++)
            {
                if ((pci_read(bus, dev, fn, 0) & 0xFFFF) == 0xFFFF)
                    continue;
                if ((pci_read(bus, dev, fn, 8) >> 8) != 0x0C0330) /* serial bus / USB / xHCI */
                    continue;
                Xhci *x = &CTRL[NCTRL];
                if (xhci_setup(x, bus, dev, fn) == 0)
                {
                    NCTRL++;
                    rescan(x);
                }
            }
        }
    return NCTRL ? 0 : -1;
}

/* Handle whatever the controllers have reported so far. */
static void pump(void)
{
    for (int i = 0; i < NCTRL; i++)
    {
        Trb e;
        for (int n = 0; n < 32 && ev_next(&CTRL[i], &e); n++)
            dispatch(&CTRL[i], &e);
    }
}

int usb_kbd_pop(unsigned char report[8])
{
    pump();
    if (KQ_HEAD == KQ_TAIL)
        return 0;
    const u8 *r = KQ[KQ_HEAD++ % KQ_N];
    for (int i = 0; i < 8; i++)
        report[i] = r[i];
    return 1;
}

int usb_poll(int *dx, int *dy, int *wheel, unsigned *buttons)
{
    static unsigned long long next_scan;
    static unsigned spin;
    if (!NCTRL)
    {
        *buttons = 0;
        return 0;
    }
    pump();

    /* devices that went away or broke: stop them and give their slot back */
    for (int i = 0; i < MAX_DEV; i++)
        if (DEV[i].used && DEV[i].gone)
            dev_remove(i);

    /* look for newly plugged devices about once a second */
    int due;
    if (timer_ready())
    {
        unsigned long long t = timer_ms();
        due = t >= next_scan;
        if (due)
            next_scan = t + 1000;
    }
    else
        due = (++spin % 200000) == 0;
    if (due)
    {
        for (int i = 0; i < NCTRL; i++)
            rescan(&CTRL[i]);
        for (int i = 0; i < MAX_DEV; i++) /* hubs: a hub's ports are polled, there is no interrupt endpoint */
            if (DEV[i].used && DEV[i].is_hub && !DEV[i].gone)
                hub_scan(DEV[i].x, i);
    }

    int mouse = 0;
    for (int i = 0; i < MAX_HID; i++)
        if (HID[i].active && HID[i].kind == KIND_MOUSE)
            mouse = 1;
    *dx += MS.dx;
    *dy += MS.dy;
    *wheel += MS.wheel;
    MS.dx = MS.dy = MS.wheel = 0;
    *buttons = mouse ? MS.buttons : 0;
    return mouse;
}

void usb_hid_counts(int *mice, int *keyboards)
{
    *mice = *keyboards = 0;
    for (int i = 0; i < MAX_HID; i++)
    {
        if (!HID[i].active)
            continue;
        if (HID[i].kind == KIND_MOUSE)
            (*mice)++;
        else if (HID[i].kind == KIND_KBD)
            (*keyboards)++;
    }
}

int usb_mouse_info(int index, int *report_protocol, int *has_wheel)
{
    for (int i = 0; i < MAX_HID; i++)
    {
        if (!HID[i].active || HID[i].kind != KIND_MOUSE)
            continue;
        if (index-- > 0)
            continue;
        *report_protocol = HID[i].rp;
        *has_wheel = HID[i].rp && HID[i].hm.has_wheel;
        return 0;
    }
    return -1;
}

#else /* not x86-64: no PCI ports */

void usb_hid_counts(int *mice, int *keyboards) { *mice = *keyboards = 0; }
int usb_mouse_info(int index, int *report_protocol, int *has_wheel)
{
    (void)index; (void)report_protocol; (void)has_wheel;
    return -1;
}
int usb_init(void) { return -1; }
int usb_kbd_pop(unsigned char report[8])
{
    (void)report;
    return 0;
}
int usb_poll(int *dx, int *dy, int *wheel, unsigned *buttons)
{
    (void)dx; (void)dy; (void)wheel;
    *buttons = 0;
    return 0;
}

#endif
