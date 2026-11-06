/* xHCI: Bluetooth controller (USB transport of HCI, see include/bluetooth.h for the HCI side).
 *
 * The USB Bluetooth class (class 0xE0, subclass 1, protocol 1) maps the HCI packets onto USB like this:
 *   HCI command  -> control transfer on endpoint 0: bmRequestType 0x20 (class, to the device), bRequest 0, wValue 0, wIndex 0
 *   HCI event    -> interrupt IN endpoint (a long event is cut into packets of the endpoint's size; the length byte in the
 *                   event header tells where it ends, so we put the pieces together)
 *   ACL data     -> bulk IN / bulk OUT endpoints (configured here, used by a later stage)
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
    return bd->iface >= 0 && bd->intr_addr && bd->in_addr && bd->out_addr && bd->intr_mps <= 512;
}

/* One transfer of exactly one packet on the interrupt IN endpoint. The size must be the packet size: a bigger transfer would not
 * finish until it is full or a short packet comes, and a full-size packet is not "short". */
static void bt_queue(Bt *b)
{
    ring_put(&b->rintr, b->ibuf_phys, b->intr_mps, (1u << 10) | (1u << 5)); /* Normal TRB, interrupt on completion */
    mb();
    wr32(b->x->db, (u32)b->slot * 4, (u32)b->intr_dci);
}

/* A transfer event of this slot / endpoint: hand the packet on and queue the next one. */
void bt_event(Xhci *x, const Trb *e)
{
    Bt *b = &BT;
    if (!b->active || b->x != x || (int)(e->d3 >> 24) != b->slot || (int)((e->d3 >> 16) & 31) != b->intr_dci)
        return;
    int code = (int)(e->d2 >> 24);
    if (code == 1 || code == 13)
    {
        unsigned resid = e->d2 & 0xFFFFFF;
        unsigned n = b->intr_mps > resid ? b->intr_mps - resid : 0;
        if (b->ev_have + n > BT_EV_BUF) /* cannot be a real event: start again with the next packet */
            b->ev_have = 0;
        for (unsigned i = 0; i < n && b->ev_have < BT_EV_BUF; i++)
            b->ev[b->ev_have++] = b->ibuf[i];
        while (b->ev_have >= 2 && b->ev_have >= 2u + b->ev[1]) /* header (code, length) + parameters complete */
        {
            unsigned need = 2u + b->ev[1];
            bt_rx_event(b->ev, need);
            b->ev_have -= need;
            for (unsigned i = 0; i < b->ev_have; i++)
                b->ev[i] = b->ev[need + i];
        }
        b->errs = 0;
    }
    else
    {
        b->ev_have = 0;
        if (++b->errs > 16) /* unplugged or broken: give up, usb_poll() removes it */
        {
            DEV[b->dev].gone = 1;
            return;
        }
        if (code == 6)
            ep_recover(x, b->slot, b->intr_dci, &b->rintr);
    }
    bt_queue(b);
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
    if (code == 6)
        ep_recover(x, b->slot, 1, &d->ep0);
    return ctrl_ok(code) ? 0 : -1;
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
    b->ev_have = 0;
    b->errs = 0;
    b->vid = b->pid = 0;

    int rings = 0;
    b->ibuf = dma_page(x, &b->ibuf_phys);
    int ok = b->ibuf != 0;
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
        if (b->ibuf)
            pmm_free_page(b->ibuf_phys);
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
    bt_queue(b);

    BtTransport t;
    t.send_cmd = bt_send_cmd;
    t.wait_step = bt_wait_step;
    t.alive = bt_alive;
    t.ctx = b;
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
}

#endif
