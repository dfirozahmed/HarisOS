/* xHCI: the mouse and keyboard (HID reports), port events. */
#include "usb_xhci_internal.h"

#if defined(__x86_64__)

/* ---------- the mouse ---------- */

void hid_queue(Hid *h)
{
    ring_put(&h->ring, h->buf_phys, (u32)h->rlen, (1u << 10) | (1u << 5)); /* Normal TRB, interrupt on completion */
    mb();
    wr32(h->x->db, (u32)h->slot * 4, (u32)h->dci);
}

void hid_event(Xhci *x, const Trb *e)
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

void port_event(Xhci *x, const Trb *e)
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

u32 ep_interval(int speed, u32 bint)
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

#endif
