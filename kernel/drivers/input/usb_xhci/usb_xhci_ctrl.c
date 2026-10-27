/* xHCI: controller bring-up and the public usb_* functions. */
#include "usb_xhci_internal.h"

#if defined(__x86_64__)

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
void pump(void)
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

#endif
