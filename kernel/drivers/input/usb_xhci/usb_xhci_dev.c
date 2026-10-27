/* xHCI: bringing a device up, port scanning. */
#include "usb_xhci_internal.h"

#if defined(__x86_64__)

/* ---------- devices ---------- */

/* Brings up a device (already reset and enabled on its port) and, if it is a hub or has a
 * HID boot interface, keeps it. Returns 0 if it failed in a way worth retrying, 1 when the
 * device was handled (kept, or something we do not use). */
int bring_up(Xhci *x, const Loc *L)
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

void rescan(Xhci *x)
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

#endif
