/* xHCI: mass storage (bulk-only transport over the controller). */
#include "usb_xhci_internal.h"

#if defined(__x86_64__)

/* ---------- mass storage ---------- */

void pump(void);

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


/* Looks through the configuration descriptor in x->dma (`total` bytes): interface class 8 (mass storage),
 * subclass 6 (SCSI commands), protocol 0x50 (bulk-only). A SuperSpeed endpoint is followed by a companion
 * descriptor (type 0x30) that holds its burst size. UAS (protocol 0x62) is not supported. */
int msc_find(Xhci *x, u32 total, MscDesc *md)
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
int msc_setup(Xhci *x, int di, const Loc *L, u32 cfg_value, const MscDesc *md)
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

#endif
