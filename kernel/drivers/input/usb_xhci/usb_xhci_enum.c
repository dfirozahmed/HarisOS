/* xHCI: enumeration and hubs. */
#include "usb_xhci_internal.h"

#if defined(__x86_64__)

/* ---------- enumeration ---------- */

void drop_slot(Xhci *x, u32 slot)
{
    command(x, 0, (10u << 10) | (slot << 24), 0); /* Disable Slot */
    x->dcbaa[slot] = 0;
}

int dev_alloc(void)
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
void dev_remove(int di)
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
void fill_slot(volatile u32 *c, const Loc *L, int entries, int hub_ports, int think_time)
{
    c[0] = L->route | ((u32)L->speed << 20) | (hub_ports ? (1u << 26) : 0) | ((u32)entries << 27);
    c[1] = ((u32)(L->root + 1) << 16) | ((u32)hub_ports << 24);
    c[2] = (u32)L->tt_slot | ((u32)L->tt_port << 8) | ((u32)think_time << 16);
}

int bring_up(Xhci *x, const Loc *L);
void hub_scan(Xhci *x, int di);

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
void hub_scan(Xhci *x, int di)
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
int hub_setup(Xhci *x, int di, u32 cfg_value)
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

#endif
