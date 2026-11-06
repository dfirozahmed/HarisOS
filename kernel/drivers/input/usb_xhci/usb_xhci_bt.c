/* xHCI: Bluetooth controller (USB transport of HCI, see include/bluetooth.h for the HCI side).
 *
 * The USB Bluetooth class (class 0xE0, subclass 1, protocol 1) maps the HCI packets onto USB like this:
 *   HCI command  -> control transfer on endpoint 0: bmRequestType 0x20 (class, to the device), bRequest 0, wValue 0, wIndex 0
 *   HCI event    -> interrupt IN endpoint (a long event is cut into packets of the endpoint's size; the length byte in the
 *                   event header tells where it ends, so bt_rx_event_chunk() puts the pieces together)
 *   ACL data     -> bulk IN endpoint (same: bt_rx_acl_chunk()) and bulk OUT endpoint (one transfer per HCI ACL packet)
 * Interface 1 (isochronous, for voice) is left alone: it needs alternate settings we do not use. */
#include "usb_xhci_internal.h"
#include "bluetooth.h"

#if defined(__x86_64__)

Bt BT;

static int bt_gone(void) { return !BT.active || !DEV[BT.dev].used || DEV[BT.dev].gone; }

/* Looks through the configuration descriptor in x->dma (`total` bytes) for the Bluetooth interface (alternate setting 0) and its
 * three endpoints. */
int bt_find(Xhci *x, u32 total, BtDesc *bd)
{
    int in_bt = 0;
    bd->iface = -1;
    bd->intr_addr = bd->in_addr = bd->out_addr = 0;
    bd->intr_mps = bd->in_mps = bd->out_mps = bd->intr_interval = 0;
    for (u32 off = 0; off + 2 <= total && x->dma[off] >= 2; off += x->dma[off])
    {
        u8 len = x->dma[off], type = x->dma[off + 1];
        if (off + len > total)
            break;
        if (type == 4 && len >= 9)
        {
            if (bd->iface >= 0)
                break; /* the next interface (voice, alternate settings): the data interface is complete */
            in_bt = x->dma[off + 3] == 0 && x->dma[off + 5] == 0xE0 && x->dma[off + 6] == 1 && x->dma[off + 7] == 1;
            if (in_bt)
                bd->iface = x->dma[off + 2];
        }
        else if (type == 5 && len >= 7 && in_bt && (x->dma[off + 2] & 15))
        {
            u8 addr = x->dma[off + 2], attr = x->dma[off + 3] & 3;
            unsigned mps = (x->dma[off + 4] | (x->dma[off + 5] << 8)) & 0x7FF;
            if (mps == 0)
                continue;
            if (attr == 3 && (addr & 0x80)) /* interrupt IN */
            {
                bd->intr_addr = addr;
                bd->intr_mps = mps;
                bd->intr_interval = x->dma[off + 6];
            }
            else if (attr == 2 && (addr & 0x80)) /* bulk IN */
            {
                bd->in_addr = addr;
                bd->in_mps = mps;
            }
            else if (attr == 2) /* bulk OUT */
            {
                bd->out_addr = addr;
                bd->out_mps = mps;
            }
        }
    }
    return bd->iface >= 0 && bd->intr_addr && bd->in_addr && bd->out_addr && bd->intr_mps <= 512 && bd->in_mps <= 512 && bd->out_mps <= 512;
}

/* Bulk IN: several transfers wait at the controller at once, each into a buffer of its own. With one only, the chip's data (a file arrives as a
 * stream of small packets) came out at one packet per pass of the main loop: the next transfer was queued only after the event of the last was
 * handled. With BT_IN_SLOTS waiting, a pass takes everything that has arrived. A page holds 8 buffers of 512 bytes (the largest bulk packet). */
#define BT_IN_SLOTS 8
#define BT_IN_STRIDE 512

static void bt_queue_in(Bt *b, unsigned slot)
{
    ring_put(&b->rin, b->abuf_phys + (u64)slot * BT_IN_STRIDE, b->in_mps, (1u << 10) | (1u << 5)); /* Normal TRB, interrupt on completion */
}

/* all slots, after the endpoint was reset (its queue is empty then) or at the start */
static void bt_queue_in_all(Bt *b)
{
    for (unsigned i = 0; i < BT_IN_SLOTS; i++)
        bt_queue_in(b, i);
    b->a_next = 0;
    mb();
    wr32(b->x->db, (u32)b->slot * 4, (u32)b->in_dci);
}

/* One transfer of exactly one packet on the interrupt IN / bulk IN endpoint. The size must be the packet size: a bigger transfer would
 * not finish until it is full or a short packet comes, and a full-size packet is not "short". */
static void bt_queue(Bt *b, int bulk)
{
    if (bulk) /* (the bulk endpoint is queued by bt_queue_in_all / bt_event) */
        return;
    ring_put(&b->rintr, b->ibuf_phys, b->intr_mps, (1u << 10) | (1u << 5)); /* Normal TRB, interrupt on completion */
    mb();
    wr32(b->x->db, (u32)b->slot * 4, (u32)b->intr_dci);
}

/* A transfer event of this slot on the event or ACL endpoint: hand the packet on and queue the next one. (The bulk OUT endpoint's events
 * are taken by bt_send_acl() itself.) */
void bt_event(Xhci *x, const Trb *e)
{
    Bt *b = &BT;
    if (!b->active || b->x != x || (int)(e->d3 >> 24) != b->slot)
        return;
    int dci = (int)((e->d3 >> 16) & 31);
    int bulk = dci == b->in_dci;
    if (!bulk && dci != b->intr_dci)
        return;
    int code = (int)(e->d2 >> 24);
    if (code == 1 || code == 13)
    {
        unsigned mps = bulk ? b->in_mps : b->intr_mps;
        unsigned resid = e->d2 & 0xFFFFFF;
        unsigned n = mps > resid ? mps - resid : 0;
        u8 tmp[512];
        unsigned slot = b->a_next % BT_IN_SLOTS;
        volatile u8 *src = bulk ? b->abuf + slot * BT_IN_STRIDE : b->ibuf;
        for (unsigned i = 0; i < n && i < sizeof tmp; i++)
            tmp[i] = src[i];
        if (bulk)
            bt_rx_acl_chunk(tmp, n);
        else
            bt_rx_event_chunk(tmp, n);
        b->errs = 0;
    }
    else
    {
        if (++b->errs > 16) /* unplugged or broken: give up, usb_poll() removes it */
        {
            DEV[b->dev].gone = 1;
            return;
        }
        if (code == 6)
        {
            ep_recover(x, b->slot, dci, bulk ? &b->rin : &b->rintr);
            if (bulk) /* the reset threw away every transfer that waited: queue them all again */
            {
                bt_queue_in_all(b);
                return;
            }
        }
    }
    if (bulk)
    {
        bt_queue_in(b, b->a_next % BT_IN_SLOTS); /* the buffer just emptied goes to the back of the queue */
        b->a_next++;
        mb();
        wr32(b->x->db, (u32)b->slot * 4, (u32)b->in_dci);
    }
    else
        bt_queue(b, 0);
}

/* ---- the transport handed to the HCI core ---- */

static int bt_send_cmd(void *ctx, const unsigned char *pkt, unsigned len)
{
    Bt *b = (Bt *)ctx;
    if (bt_gone() || len < 3 || len > 3 + 255)
        return -1;
    Xhci *x = b->x;
    Dev *d = &DEV[b->dev];
    for (unsigned i = 0; i < len; i++)
        x->dma[i] = pkt[i];
    int code = control(x, b->slot, &d->ep0, 0x20, 0, 0, 0, (u16)len, 0);
    if (code == 6) /* STALL: the chip did not take the command, so sending it again is safe (a time-out would not be: it may have taken it) */
    {
        bt_note_cmd_error(code);
        ep_recover(x, b->slot, 1, &d->ep0);
        delay_ms(2);
        for (unsigned i = 0; i < len; i++)
            x->dma[i] = pkt[i];
        code = control(x, b->slot, &d->ep0, 0x20, 0, 0, 0, (u16)len, 0);
        if (code == 6)
            ep_recover(x, b->slot, 1, &d->ep0);
    }
    if (!ctrl_ok(code))
    {
        bt_note_cmd_error(code);
        return -1;
    }
    return 0;
}

/* One HCI ACL packet on the bulk OUT endpoint: a single transfer. The device takes the length from the packet's own header, so there is
 * no extra empty packet at the end. Waits for the transfer to finish (other events are handled meanwhile). */
static int bt_send_acl(void *ctx, const unsigned char *pkt, unsigned len)
{
    Bt *b = (Bt *)ctx;
    if (bt_gone() || len < 4 || len > 4096)
        return -1;
    Xhci *x = b->x;
    for (unsigned i = 0; i < len; i++)
        b->obuf[i] = pkt[i];
    ring_put(&b->rout, b->obuf_phys, len, (1u << 10) | (1u << 5));
    mb();
    wr32(x->db, (u32)b->slot * 4, (u32)b->out_dci);
    for (unsigned i = 0; i < 20000; i++) /* about 2 s */
    {
        Trb e;
        if (ev_next(x, &e))
        {
            if (((e.d3 >> 10) & 63) == 32 && (int)(e.d3 >> 24) == b->slot && (int)((e.d3 >> 16) & 31) == b->out_dci)
            {
                int code = (int)(e.d2 >> 24);
                if (code == 1 || code == 13)
                    return 0;
                if (code == 6)
                    ep_recover(x, b->slot, b->out_dci, &b->rout);
                return -1;
            }
            dispatch(x, &e);
            continue;
        }
        if (bt_gone())
            return -1;
        udelay(100);
    }
    /* never finished: stop the endpoint and move the ring past the unfinished TRB */
    command(x, 0, (15u << 10) | ((u32)b->out_dci << 16) | ((u32)b->slot << 24), 0);
    ep_recover(x, b->slot, b->out_dci, &b->rout);
    return -1;
}

/* 100 us of waiting; the events the controller reported meanwhile reach bt_event() through pump(). */
static void bt_wait_step(void *ctx)
{
    (void)ctx;
    udelay(100);
    pump();
}

static int bt_alive(void *ctx)
{
    (void)ctx;
    return !bt_gone();
}

static unsigned long long bt_clock(void *ctx)
{
    (void)ctx;
    return timer_ready() ? timer_ms() : 0;
}

/* Configure the three endpoints of the device in DEV[di], select its configuration, read the USB ids and start the HCI side.
 * Returns 1 if the device was handled (kept, or given up for good), 0 if it is worth another try. */
int bt_setup(Xhci *x, int di, const Loc *L, u32 cfg_value, const BtDesc *bd)
{
    Dev *d = &DEV[di];
    u32 slot = (u32)d->slot;
    Bt *b = &BT;
    b->x = x;
    b->slot = d->slot;
    b->dev = di;
    b->intr_dci = (bd->intr_addr & 15) * 2 + 1;
    b->in_dci = (bd->in_addr & 15) * 2 + 1;
    b->out_dci = (bd->out_addr & 15) * 2;
    b->intr_mps = bd->intr_mps;
    b->in_mps = bd->in_mps;
    b->out_mps = bd->out_mps;
    b->errs = 0;
    b->vid = b->pid = 0;

    /* three buffer pages (events, ACL in, ACL out) and three rings */
    int pages = 0, rings = 0;
    int ok = 1;
    b->ibuf = b->abuf = b->obuf = 0;
    b->ibuf = dma_page(x, &b->ibuf_phys);
    ok = b->ibuf != 0;
    if (ok)
    {
        pages = 1;
        b->abuf = dma_page(x, &b->abuf_phys);
        ok = b->abuf != 0;
    }
    if (ok)
    {
        pages = 2;
        b->obuf = dma_page(x, &b->obuf_phys);
        ok = b->obuf != 0;
    }
    if (ok)
        pages = 3;
    Ring *all[3] = {&b->rintr, &b->rin, &b->rout};
    for (int i = 0; ok && i < 3; i++)
    {
        ok = ring_init(x, all[i]) == 0;
        if (ok)
            rings++;
    }

    if (ok)
    {
        int maxdci = b->intr_dci;
        if (b->in_dci > maxdci)
            maxdci = b->in_dci;
        if (b->out_dci > maxdci)
            maxdci = b->out_dci;
        inctx_clear(x);
        volatile u32 *c = ctx_at(x, 0);
        c[1] = 1u | (1u << b->intr_dci) | (1u << b->in_dci) | (1u << b->out_dci);
        fill_slot(ctx_at(x, 1), L, maxdci, 0, 0);
        c = ctx_at(x, b->intr_dci + 1); /* interrupt IN: type 7, as for the HID devices */
        c[0] = ep_interval(L->speed, bd->intr_interval) << 16;
        c[1] = (3u << 1) | (7u << 3) | (bd->intr_mps << 16);
        c[2] = (u32)b->rintr.phys | 1;
        c[3] = (u32)(b->rintr.phys >> 32);
        c[4] = bd->intr_mps | (bd->intr_mps << 16);
        c = ctx_at(x, b->in_dci + 1); /* bulk IN: type 6 */
        c[1] = (3u << 1) | (6u << 3) | (bd->in_mps << 16);
        c[2] = (u32)b->rin.phys | 1;
        c[3] = (u32)(b->rin.phys >> 32);
        c[4] = bd->in_mps;
        c = ctx_at(x, b->out_dci + 1); /* bulk OUT: type 2 */
        c[1] = (3u << 1) | (2u << 3) | (bd->out_mps << 16);
        c[2] = (u32)b->rout.phys | 1;
        c[3] = (u32)(b->rout.phys >> 32);
        c[4] = bd->out_mps;
        ok = command(x, x->inctx_phys, (12u << 10) | (slot << 24), 0) == 1 &&
             ctrl_ok(control(x, (int)slot, &d->ep0, 0x00, 9, (u16)cfg_value, 0, 0, 0)); /* SET_CONFIGURATION */
    }
    if (!ok)
    {
        for (int i = 0; i < rings; i++)
            pmm_free_page(all[i]->phys);
        if (pages >= 1)
            pmm_free_page(b->ibuf_phys);
        if (pages >= 2)
            pmm_free_page(b->abuf_phys);
        if (pages >= 3)
            pmm_free_page(b->obuf_phys);
        d->used = 0;
        drop_slot(x, slot);
        return 0;
    }

    /* The USB ids (device descriptor bytes 8..11) name the chip; the HCI side uses them to know what it is talking to. */
    if (ctrl_ok(control(x, (int)slot, &d->ep0, 0x80, 6, 0x0100, 0, 18, 1)))
    {
        b->vid = (unsigned short)(x->dma[8] | (x->dma[9] << 8));
        b->pid = (unsigned short)(x->dma[10] | (x->dma[11] << 8));
    }

    b->active = 1; /* from here dev_remove() takes care of it; bt_event() needs it to be set while the chip starts */
    bt_queue(b, 0);
    bt_queue_in_all(b);

    BtTransport t;
    t.send_cmd = bt_send_cmd;
    t.wait_step = bt_wait_step;
    t.alive = bt_alive;
    t.ctx = b;
    t.now_ms = bt_clock;
    t.send_acl = bt_send_acl;
    bt_attach(&t, b->vid, b->pid); /* reset + version: even if the chip does not answer, the device stays and the status says why */
    return 1;
}

/* The device was unplugged (its slot is already disabled). */
void bt_release(void)
{
    Bt *b = &BT;
    b->active = 0;
    bt_detach();
    pmm_free_page(b->rintr.phys);
    pmm_free_page(b->rin.phys);
    pmm_free_page(b->rout.phys);
    pmm_free_page(b->ibuf_phys);
    pmm_free_page(b->abuf_phys);
    pmm_free_page(b->obuf_phys);
}

#endif
