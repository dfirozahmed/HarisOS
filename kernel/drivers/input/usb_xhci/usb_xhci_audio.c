/* xHCI: USB audio (USB Audio Class 1.0) playback over an isochronous OUT endpoint.
 *
 * What a USB sound device looks like: besides the AudioControl interface (volume, mute: not used here, devices start unmuted at a
 * sensible level) it has an AudioStreaming interface with several "alternate settings". Alternate 0 has no endpoint (the device is
 * idle and takes no bus time); every other alternate has an isochronous OUT endpoint and a class specific Format Type descriptor that
 * says how the samples must look: channels, bytes per sample, bit depth and the sample rates. We look for the alternate that takes
 * 2 channels of 16-bit samples at 48 kHz, select it (SET_INTERFACE) and then send one packet per millisecond: 48 frames * 4 bytes.
 *
 * Isochronous means "on time or never": the controller sends whatever packet is queued for each 1 ms interval and there is no retry,
 * so the driver has to keep the ring filled a little ahead of the clock. Each packet is one TD of one Isoch TRB with SIA ("start
 * isochronous ASAP": no frame number to compute). play_pcm() paces itself by timer_ms(): it keeps about 24 ms queued, which is
 * enough to ride out a late loop pass and small enough for the 40 packet buffer; if the loop is ever more than a moment late the
 * clock is moved back, so the gap is silence and no queued packet is overwritten. */
#include "usb_xhci_internal.h"

#if defined(__x86_64__)

Aud AUD;

#define AHEAD_MS 24 /* how far ahead of the clock packets are queued */

/* Scan the configuration descriptor in x->dma for a streaming interface whose alternate setting plays 48 kHz 16-bit stereo. */
int aud_find(Xhci *x, u32 total, AudDesc *ad)
{
    int in_as = 0, ok_fmt = 0, iface = -1, alt = 0;
    int have = 0;
    for (u32 off = 0; off + 2 <= total && x->dma[off] >= 2; off += x->dma[off])
    {
        u8 len = x->dma[off], type = x->dma[off + 1];
        if (off + len > total)
            break;
        if (type == 4 && len >= 9) /* interface: class 1 (audio), subclass 2 (streaming) */
        {
            in_as = x->dma[off + 5] == 1 && x->dma[off + 6] == 2;
            ok_fmt = 0;
            iface = x->dma[off + 2];
            alt = x->dma[off + 3];
        }
        else if (type == 0x24 && len >= 8 && in_as && x->dma[off + 2] == 2 && x->dma[off + 3] == 1) /* FORMAT_TYPE, type I (PCM) */
        {
            unsigned ch = x->dma[off + 4], sub = x->dma[off + 5], bits = x->dma[off + 6], nfreq = x->dma[off + 7];
            ok_fmt = 0;
            if (ch == 2 && sub == 2 && bits == 16 && nfreq >= 1 && off + 8 + 3u * nfreq <= off + len)
                for (unsigned k = 0; k < nfreq; k++)
                {
                    u32 f = x->dma[off + 8 + 3 * k] | (u32)x->dma[off + 9 + 3 * k] << 8 | (u32)x->dma[off + 10 + 3 * k] << 16;
                    if (f == 48000)
                        ok_fmt = 1;
                }
        }
        else if (type == 5 && len >= 7 && in_as && ok_fmt && !have) /* the endpoint of that alternate setting */
        {
            u8 addr = x->dma[off + 2];
            if (!(addr & 0x80) && (addr & 15) && (x->dma[off + 3] & 3) == 1) /* isochronous OUT */
            {
                unsigned mps = (x->dma[off + 4] | (x->dma[off + 5] << 8)) & 0x7FF;
                if (mps >= AUD_PKT_BYTES && alt > 0)
                {
                    ad->iface = iface;
                    ad->alt = alt;
                    ad->addr = addr;
                    ad->mps = mps;
                    ad->interval = x->dma[off + 6];
                    have = 1;
                }
            }
        }
    }
    return have;
}

/* Configure the isochronous endpoint of the device in DEV[di], select the configuration and the playing alternate setting.
 * Returns 1 if the device was handled (kept, or given up for good), 0 if it is worth another try. */
int aud_setup(Xhci *x, int di, const Loc *L, u32 cfg_value, const AudDesc *ad)
{
    Dev *d = &DEV[di];
    u32 slot = (u32)d->slot;
    Aud *a = &AUD;
    a->x = x;
    a->slot = d->slot;
    a->dev = di;
    a->dci = (ad->addr & 15) * 2; /* OUT endpoint: 2 * number */
    a->busy = 0;
    int pages = 0;
    int ok = ring_init(x, &a->ring) == 0;
    for (int i = 0; ok && i < 2; i++)
    {
        a->buf_phys[i] = pmm_alloc_contiguous_tag(1, PMM_TAG_USB);
        if (a->buf_phys[i] && !x->ac64 && a->buf_phys[i] + 4096 > 0x100000000ULL)
        {
            pmm_free_contiguous(a->buf_phys[i], 1);
            a->buf_phys[i] = 0;
        }
        if (!a->buf_phys[i])
            ok = 0;
        else
            pages++;
    }
    if (ok)
    {
        inctx_clear(x);
        volatile u32 *c = ctx_at(x, 0);
        c[1] = 1u | (1u << a->dci);
        fill_slot(ctx_at(x, 1), L, a->dci, 0, 0);
        c = ctx_at(x, a->dci + 1);
        c[0] = ep_interval(L->speed, ad->interval) << 16;
        c[1] = (3u << 1) | (1u << 3) | (ad->mps << 16); /* error count 3, type isochronous OUT */
        c[2] = (u32)a->ring.phys | 1;
        c[3] = (u32)(a->ring.phys >> 32);
        c[4] = AUD_PKT_BYTES | (ad->mps << 16); /* average TRB length; max ESIT payload (bytes per service interval) */
        ok = command(x, x->inctx_phys, (12u << 10) | (slot << 24), 0) == 1 &&
             ctrl_ok(control(x, (int)slot, &d->ep0, 0x00, 9, (u16)cfg_value, 0, 0, 0)) &&   /* SET_CONFIGURATION */
             ctrl_ok(control(x, (int)slot, &d->ep0, 0x01, 11, (u16)ad->alt, (u16)ad->iface, 0, 0)); /* SET_INTERFACE: start streaming */
    }
    if (ok)
    {
        a->active = 1; /* from here dev_remove() takes care of it */
        return 1;
    }
    if (a->ring.trb)
        pmm_free_page(a->ring.phys);
    for (int i = 0; i < pages; i++)
        pmm_free_contiguous(a->buf_phys[i], 1);
    d->used = 0;
    drop_slot(x, slot);
    return 0;
}

void aud_release(void)
{
    Aud *a = &AUD;
    a->active = 0; /* a play loop notices and stops */
    pmm_free_page(a->ring.phys); /* the slot is disabled: the controller no longer reads these */
    pmm_free_contiguous(a->buf_phys[0], 1);
    pmm_free_contiguous(a->buf_phys[1], 1);
}

int usb_audio_ready(void) { return AUD.active; }

/* Copy packet number `n` into its slot of the buffer and queue it. */
static void aud_queue(Aud *a, const short *pcm, unsigned long frames, unsigned long n, int last)
{
    unsigned slot = (unsigned)(n % AUD_SLOTS);
    u64 phys = a->buf_phys[slot / AUD_SLOTS_PER_PAGE] + (u64)(slot % AUD_SLOTS_PER_PAGE) * AUD_PKT_BYTES;
    volatile short *dst = (volatile short *)phys;
    unsigned long first = n * AUD_PKT_FRAMES;
    for (unsigned i = 0; i < AUD_PKT_FRAMES * 2; i++) /* the end of the sound is padded with silence */
        dst[i] = (first + i / 2 < frames) ? pcm[first * 2 + i] : 0;
    /* Isochronous TRB (type 5): SIA (bit 31) = start as soon as possible; IOC on the last one only */
    ring_put(&a->ring, phys, AUD_PKT_BYTES, (5u << 10) | (1u << 31) | (last ? (1u << 5) : 0));
}

int usb_audio_play_pcm(const short *pcm, unsigned long frames)
{
    Aud *a = &AUD;
    if (!a->active || !timer_ready())
        return -1;
    if (a->busy)
        return -2;
    if (frames == 0)
        return 0;
    a->busy = 1;
    unsigned long total = (frames + AUD_PKT_FRAMES - 1) / AUD_PKT_FRAMES;
    unsigned long queued = 0;
    unsigned long long start = timer_ms();
    int gone = 0;
    for (;;) /* queue everything just in time, then let the last packets play out */
    {
        pump(); /* transfer events and errors of the endpoint are not needed, but the event ring must not fill up */
        if (!a->active || DEV[a->dev].gone)
        {
            gone = 1;
            break;
        }
        unsigned long long el = timer_ms() - start;
        if (el > queued) /* we are late: the controller played silence; do not rush to catch up (the buffer is small) */
        {
            start += el - queued;
            el = queued;
        }
        unsigned long target = (unsigned long)el + AHEAD_MS;
        if (target > total)
            target = total;
        int pushed = 0;
        while (queued < target)
        {
            aud_queue(a, pcm, frames, queued, queued + 1 == total);
            queued++;
            pushed = 1;
        }
        if (pushed)
        {
            mb();
            wr32(a->x->db, (u32)a->slot * 4, (u32)a->dci);
        }
        if (queued >= total && timer_ms() - start >= total + 8) /* the last packet has been sent */
            break;
        udelay(200);
    }
    a->busy = 0;
    return gone ? -3 : 0;
}

#endif
