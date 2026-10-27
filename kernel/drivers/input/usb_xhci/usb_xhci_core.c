/* xHCI: PCI configuration space, controller state, rings, commands and control transfers. */
#include "usb_xhci_internal.h"

#if defined(__x86_64__)

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
/* ---------- PCI configuration space (legacy ports 0xCF8 / 0xCFC) ---------- */

u32 pci_read(u32 bus, u32 dev, u32 fn, u32 off)
{
    io_out32(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return io_in32(0xCFC);
}

void pci_write(u32 bus, u32 dev, u32 fn, u32 off, u32 v)
{
    io_out32(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    io_out32(0xCFC, v);
}

/* ---------- controller state ---------- */




Xhci CTRL[MAX_CTRL];
int NCTRL;


/* A device we keep running: a hub, or something with a HID interface we use. */

Dev DEV[MAX_DEV];

/* One interrupt-IN endpoint of a HID boot device (mouse or keyboard). */

u8 RDESC[RDESC_MAX];

Hid HID[MAX_HID];

Anon_MS MS;

/* One USB mass-storage interface (bulk-only transport): a bulk IN and a bulk OUT endpoint, each with its own
 * ring, and one bounce buffer that every data stage goes through (the caller's buffer is not DMA memory). The
 * SCSI side lives in kernel/storage/usb_msc.c; msc.msc is its state and the block device's ctx. */

Msc MSC[MAX_MSC];

/* Keyboard boot reports (8 bytes each) waiting for keyboard.c */
u8 KQ[KQ_N][8];
unsigned KQ_HEAD, KQ_TAIL;

void kq_push(const volatile u8 *r, int len)
{
    if (KQ_TAIL - KQ_HEAD >= KQ_N)
        return; /* full: the next report carries the full key state anyway */
    u8 *d = KQ[KQ_TAIL++ % KQ_N];
    for (int i = 0; i < 8; i++)
        d[i] = i < len ? r[i] : 0;
}

void kq_push_empty(void) /* "all keys released" */
{
    static const u8 zero[8] = {0};
    kq_push(zero, 8);
}

volatile u8 *dma_page(Xhci *x, u64 *phys)
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

int ring_init(Xhci *x, Ring *r)
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

void ring_put(Ring *r, u64 param, u32 status, u32 control)
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

int ev_next(Xhci *x, Trb *out)
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

void hid_event(Xhci *x, const Trb *e);
void port_event(Xhci *x, const Trb *e);

void dispatch(Xhci *x, const Trb *e)
{
    u32 type = (e->d3 >> 10) & 63;
    if (type == 32)
        hid_event(x, e);
    else if (type == 34)
        port_event(x, e);
}

/* Ring the doorbell of the command ring, wait for the command's completion event. */
int command(Xhci *x, u64 param, u32 control, u32 *slot_out)
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
int control(Xhci *x, int slot, Ring *r, u8 type, u8 req, u16 val, u16 idx, u16 len, int in)
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
void ep_recover(Xhci *x, int slot, int dci, Ring *r)
{
    command(x, 0, (14u << 10) | ((u32)dci << 16) | ((u32)slot << 24), 0);
    command(x, (r->phys + (u64)r->idx * 16) | r->cycle,
            (16u << 10) | ((u32)dci << 16) | ((u32)slot << 24), 0);
}

int ctrl_ok(int code) { return code == 1 || code == 13; }

/* ---------- input context ---------- */

volatile u32 *ctx_at(Xhci *x, int n) { return (volatile u32 *)(x->inctx + (u32)n * (u32)x->csz); }

void inctx_clear(Xhci *x)
{
    volatile u64 *w = (volatile u64 *)x->inctx;
    for (int i = 0; i < 512; i++)
        w[i] = 0;
}

#endif
