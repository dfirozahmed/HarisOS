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
#include "io.h"
#include "pmm.h"
#include "timer.h"

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

/* One interrupt-IN endpoint of a HID boot device (mouse or keyboard). */
#define MAX_HID 4
#define KIND_KBD 1
#define KIND_MOUSE 2
typedef struct
{
    int active, drop, kind;
    Xhci *x;
    int slot, dci, port, rlen, errs;
    Ring ring;
    volatile u8 *buf;
    u64 buf_phys;
} Hid;

static Hid HID[MAX_HID];

static struct
{
    int dx, dy, wheel; /* movement not yet handed out */
    unsigned buttons;
} MS;

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
    u64 p = pmm_alloc_page();
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
        l->d3 = (6u << 10) | 2u | r->cycle; /* type Link, toggle-cycle */
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
            if (h->kind == KIND_MOUSE && len >= 3)
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
                h->drop = 1;
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
        for (int i = 0; i < MAX_HID; i++)
        {
            Hid *h = &HID[i];
            if (!h->active || h->x != x || h->port != p)
                continue;
            h->active = 0;
            h->drop = 1;
            if (h->kind == KIND_MOUSE)
                MS.buttons = 0;
            else
                kq_push_empty(); /* release whatever was held */
        }
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

/* Brings up the device on root port p. Returns 0 if it failed in a way worth retrying,
 * 1 when the device was handled (HID device started, or something we do not use). */
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
    if (speed > 4)
        speed = 4; /* SuperSpeed+ behaves like SuperSpeed for what we do */
    u32 mps = speed == 4 ? 512 : (speed == 3 ? 64 : 8);

    u32 slot = 0;
    if (command(x, 0, 9u << 10, &slot) != 1 || slot == 0 || (int)slot > x->nslots) /* Enable Slot */
        return 0;

    u64 dev_phys;
    volatile u8 *dev = dma_page(x, &dev_phys);
    Ring ep0;
    if (!dev || ring_init(x, &ep0))
    {
        drop_slot(x, slot);
        return 0;
    }
    x->dcbaa[slot] = dev_phys;

    /* Address Device: slot context + endpoint 0 */
    inctx_clear(x);
    volatile u32 *c = ctx_at(x, 0);
    c[1] = 3; /* add: slot context, endpoint 0 */
    c = ctx_at(x, 1);
    c[0] = (1u << 27) | ((u32)speed << 20);
    c[1] = (u32)(p + 1) << 16;
    c = ctx_at(x, 2);
    c[1] = (3u << 1) | (4u << 3) | (mps << 16); /* error count 3, type control */
    c[2] = (u32)ep0.phys | 1;
    c[3] = (u32)(ep0.phys >> 32);
    c[4] = 8;
    if (command(x, x->inctx_phys, (11u << 10) | (slot << 24), 0) != 1)
    {
        drop_slot(x, slot);
        return 0;
    }
    delay_ms(10);

    /* First 8 bytes of the device descriptor tell the real packet size of endpoint 0. */
    if (!ctrl_ok(control(x, (int)slot, &ep0, 0x80, 6, 0x0100, 0, 8, 1)))
    {
        drop_slot(x, slot);
        return 0;
    }
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
    if (!ctrl_ok(control(x, (int)slot, &ep0, 0x80, 6, 0x0200, 0, 9, 1)))
    {
        drop_slot(x, slot);
        return 0;
    }
    u32 total = x->dma[2] | ((u32)x->dma[3] << 8);
    if (total > 2048)
        total = 2048;
    if (total < 9 || !ctrl_ok(control(x, (int)slot, &ep0, 0x80, 6, 0x0200, 0, (u16)total, 1)))
    {
        drop_slot(x, slot);
        return 0;
    }

    /* Look for boot-protocol keyboard / mouse interfaces (at most one of each kind). */
    u32 cfg_value = x->dma[5];
    struct
    {
        int kind, iface, addr, mps, interval;
    } F[2];
    int nf = 0, cur = -1, cur_kind = 0;
    for (u32 off = 0; off + 2 <= total && x->dma[off] >= 2; off += x->dma[off])
    {
        u8 len = x->dma[off], type = x->dma[off + 1];
        if (off + len > total)
            break;
        if (type == 4 && len >= 9) /* interface: class 3 (HID), subclass 1 (boot), protocol 1 keyboard / 2 mouse */
        {
            cur = x->dma[off + 2];
            cur_kind = 0;
            if (x->dma[off + 5] == 3 && x->dma[off + 6] == 1)
                cur_kind = x->dma[off + 7] == 1 ? KIND_KBD : (x->dma[off + 7] == 2 ? KIND_MOUSE : 0);
            for (int i = 0; i < nf; i++)
                if (F[i].kind == cur_kind)
                    cur_kind = 0;
        }
        else if (type == 5 && len >= 7 && cur_kind && nf < 2) /* first endpoint of that interface */
        {
            if ((x->dma[off + 2] & 0x80) && (x->dma[off + 3] & 3) == 3 && (x->dma[off + 2] & 15)) /* interrupt IN */
            {
                F[nf].kind = cur_kind;
                F[nf].iface = cur;
                F[nf].addr = x->dma[off + 2] & 15;
                F[nf].mps = (x->dma[off + 4] | (x->dma[off + 5] << 8)) & 0x7FF;
                F[nf].interval = x->dma[off + 6];
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
            if (!HID[k].active && !HID[k].drop && HID[k].kind >= 0)
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
        drop_slot(x, slot); /* a hub, storage, ... : not ours */
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
    c = ctx_at(x, 1);
    c[0] = ((u32)maxdci << 27) | ((u32)speed << 20);
    c[1] = (u32)(p + 1) << 16;
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
             ctrl_ok(control(x, (int)slot, &ep0, 0x00, 9, (u16)cfg_value, 0, 0, 0)); /* SET_CONFIGURATION */
    if (!ok)
    {
        for (int i = 0; i < nh; i++)
            hid[i]->kind = 0;
        drop_slot(x, slot);
        return 0;
    }
    for (int i = 0; i < nh; i++)
        if (control(x, (int)slot, &ep0, 0x21, 0x0B, 0, (u16)F[i].iface, 0, 0) == 6) /* SET_PROTOCOL: boot */
            ep_recover(x, (int)slot, 1, &ep0);

    for (int i = 0; i < nh; i++)
    {
        Hid *h = hid[i];
        h->x = x;
        h->slot = (int)slot;
        h->dci = F[i].addr * 2 + 1;
        h->port = p;
        h->rlen = F[i].mps > 64 ? 64 : F[i].mps;
        h->errs = 0;
        h->drop = 0;
        h->kind = F[i].kind;
        if (h->kind == KIND_MOUSE)
            MS.buttons = 0;
        h->active = 1;
        hid_queue(h);
    }
    return 1;
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

    /* devices that went away: give their slot back (unless another interface of it still runs) */
    for (int i = 0; i < MAX_HID; i++)
    {
        Hid *h = &HID[i];
        if (!h->drop)
            continue;
        h->drop = 0;
        int shared = 0;
        for (int k = 0; k < MAX_HID; k++)
            if (k != i && HID[k].kind > 0 && HID[k].x == h->x && HID[k].slot == h->slot && (HID[k].active || HID[k].drop))
                shared = 1;
        if (!shared)
        {
            drop_slot(h->x, (u32)h->slot);
            h->x->handled[h->port] = 0;
        }
    }

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
        for (int i = 0; i < NCTRL; i++)
            rescan(&CTRL[i]);

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

#else /* not x86-64: no PCI ports */

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
