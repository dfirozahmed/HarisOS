/* Intel High Definition Audio controller (PCI class 04 subclass 03): reset, the CORB / RIRB command rings, one output
 * stream. Polled, no interrupts. See include/hda.h; register numbers from the HDA specification rev 1.0a, section 3.3.
 *
 * Command rings: the CORB is a ring of 32-bit verbs in RAM; we write a verb after the write pointer (CORBWP) and move
 * the pointer; the controller sends it on the link and moves its read pointer. Each answer is put in the RIRB (8 bytes:
 * the answer, then the codec address in bits 3:0 and "unsolicited" in bit 4) and RIRBWP moves. Both pointers start at
 * 0 after a reset and the first entry used is 1. If the rings do not work, the "immediate command" registers (one verb
 * at a time) are tried instead, as Linux does.
 *
 * Stream: an output stream descriptor reads a Buffer Descriptor List (16-byte entries: 64-bit address, length, IOC)
 * round and round, and sends the samples tagged with its stream number (1..15). LPIB tells how far it has read. */
#include "hda.h"
#include "hda_internal.h"
#include "io.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "timer.h"

#define R_GCAP 0x00      /* 16: output streams 15:12, input streams 11:8, bidirectional 7:3, 64-bit 0 */
#define R_VMIN 0x02
#define R_VMAJ 0x03
#define R_GCTL 0x08
#define R_STATESTS 0x0E  /* 16: bit n = a codec answered at address n after the reset */
#define R_INTCTL 0x20
#define R_CORBLBASE 0x40
#define R_CORBUBASE 0x44
#define R_CORBWP 0x48
#define R_CORBRP 0x4A    /* bit 15: reset the read pointer */
#define R_CORBCTL 0x4C   /* 8: bit 1 run */
#define R_CORBSIZE 0x4E  /* 8: size 1:0 (0 = 2, 1 = 16, 2 = 256 entries), sizes supported 7:4 */
#define R_RIRBLBASE 0x50
#define R_RIRBUBASE 0x54
#define R_RIRBWP 0x58    /* bit 15: reset the write pointer */
#define R_RINTCNT 0x5A
#define R_RIRBCTL 0x5C   /* 8: bit 0 response interrupt, bit 1 DMA run */
#define R_RIRBSTS 0x5D   /* 8: bit 0 response, bit 2 overrun; write 1 to clear */
#define R_RIRBSIZE 0x5E
#define R_ICOI 0x60      /* immediate command out */
#define R_ICII 0x64      /* immediate response in */
#define R_ICIS 0x68      /* 16: bit 0 busy, bit 1 response valid (write 1 to clear) */
#define R_DPLBASE 0x70   /* DMA position buffer, bit 0 = enable */
#define R_DPUBASE 0x74
#define R_SD0 0x80       /* stream descriptors, 0x20 bytes each: inputs first, then outputs, then bidirectional */

#define SD_CTL 0x00   /* 24 bits: 0 reset, 1 run, 2 IOC interrupt; stream number 23:20 */
#define SD_STS 0x03   /* 8: 2 buffer complete, 3 FIFO error, 4 descriptor error; write 1 to clear */
#define SD_LPIB 0x04  /* link position in buffer */
#define SD_CBL 0x08   /* cyclic buffer length */
#define SD_LVI 0x0C   /* 16: last valid index of the BDL */
#define SD_FMT 0x12   /* 16: stream format */
#define SD_BDPL 0x18
#define SD_BDPU 0x1C

#define GCTL_CRST (1u << 0) /* 0 = the controller and the link are held in reset */
#define CTL_SRST 0x01u
#define CTL_RUN 0x02u

/* DMA page of a controller: CORB (256 x 4 bytes) at 0, RIRB (256 x 8) at 0x400, position buffer at 0xC00, BDL at 0xE00
 * (every part 128-byte aligned, as the spec asks) */
#define PG_RIRB 0x400
#define PG_DPOS 0xC00
#define PG_BDL 0xE00

#define STREAM_TAG 1u
#define RATE 48000u

typedef struct
{
    u8 bus, dev, fn;
    volatile u8 *regs;
    u64 page; /* physical = virtual (identity mapped) */
    unsigned corb_n, rirb_n, rirb_rp;
    int ring_ok;   /* the CORB / RIRB have answered at least once */
    int immediate; /* 1 = the CORB / RIRB did not answer: immediate command registers */
    unsigned iss, oss;
    unsigned codecs; /* STATESTS bits */
    int has_codec;   /* codec below holds the one with outputs */
    HdaCodec codec;
    int working;
    char status[96];
} HdaCtrl;

static HdaCtrl CTRL[HDA_MAX_CTRL];
static int NCTRL;
static int PRIMARY = -1; /* the controller that plays */

static inline u8 rd8(HdaCtrl *h, u32 o) { return *(volatile u8 *)(h->regs + o); }
static inline u16 rd16(HdaCtrl *h, u32 o) { return *(volatile u16 *)(h->regs + o); }
static inline u32 rd32(HdaCtrl *h, u32 o) { return *(volatile u32 *)(h->regs + o); }
static inline void wr8(HdaCtrl *h, u32 o, u8 v) { *(volatile u8 *)(h->regs + o) = v; }
static inline void wr16(HdaCtrl *h, u32 o, u16 v) { *(volatile u16 *)(h->regs + o) = v; }
static inline void wr32(HdaCtrl *h, u32 o, u32 v) { *(volatile u32 *)(h->regs + o) = v; }
static inline void fence(void) { __asm__ volatile("mfence" ::: "memory"); }

/* a read of port 0x80 takes about 1 us: a delay that needs no timer */
static void udelay(unsigned us)
{
    for (unsigned i = 0; i < us; i++)
        io_in8(0x80);
}

/* Wait up to `ms` milliseconds for `cond` (by the timer once it runs, else by counting delays). 1 if it came true. */
#define HDA_WAIT(cond, ms)                                                              \
    ({                                                                                  \
        int _ok = 0;                                                                    \
        unsigned long long _end = timer_ready() ? timer_ms() + (ms) : 0;                \
        for (unsigned _n = 0;; _n++)                                                    \
        {                                                                               \
            if (cond)                                                                   \
            {                                                                           \
                _ok = 1;                                                                \
                break;                                                                  \
            }                                                                           \
            if (_end ? timer_ms() > _end : _n > (unsigned)(ms) * 20u)                   \
                break;                                                                  \
            udelay(50);                                                                 \
        }                                                                               \
        _ok;                                                                            \
    })

static void zero(void *p, unsigned long n)
{
    volatile u8 *b = (volatile u8 *)p;
    for (unsigned long i = 0; i < n; i++)
        b[i] = 0;
}

/* ---- status line (bounded, no libc) ---- */
static void st_str(HdaCtrl *h, const char *s)
{
    unsigned n = 0;
    while (h->status[n])
        n++;
    while (*s && n < sizeof h->status - 1)
        h->status[n++] = *s++;
    h->status[n] = 0;
}

static void st_hex(HdaCtrl *h, unsigned v, int digits)
{
    char t[9];
    for (int i = 0; i < digits; i++)
        t[i] = "0123456789ABCDEF"[(v >> (4 * (digits - 1 - i))) & 0xFu];
    t[digits] = 0;
    st_str(h, t);
}

static void log_hex(const char *label, unsigned v)
{
    char t[9];
    for (int i = 0; i < 8; i++)
        t[i] = "0123456789ABCDEF"[(v >> (4 * (7 - i))) & 0xFu];
    t[8] = 0;
    serial_puts(label);
    serial_puts(t);
}

/* ---- commands ---- */

static u32 cmd_immediate(HdaCtrl *h, u32 cmd)
{
    if (!HDA_WAIT(!(rd16(h, R_ICIS) & 1u), 5))
        return HDA_NO_RESPONSE;
    wr16(h, R_ICIS, (u16)(rd16(h, R_ICIS) | 2u)); /* clear an old "response valid" */
    wr32(h, R_ICOI, cmd);
    wr16(h, R_ICIS, (u16)(rd16(h, R_ICIS) | 1u)); /* send */
    if (!HDA_WAIT((rd16(h, R_ICIS) & 3u) == 2u, 5))
        return HDA_NO_RESPONSE;
    return rd32(h, R_ICII);
}

static u32 cmd_ring(HdaCtrl *h, u32 cmd, unsigned cad)
{
    volatile u32 *corb = (volatile u32 *)h->page;
    volatile u32 *rirb = (volatile u32 *)(h->page + PG_RIRB);
    unsigned wp = (rd16(h, R_CORBWP) + 1u) % h->corb_n;
    corb[wp] = cmd;
    fence();
    wr16(h, R_CORBWP, (u16)wp);
    for (;;)
    {
        if (!HDA_WAIT((rd16(h, R_RIRBWP) & 0xFFu) != h->rirb_rp, 20))
            return HDA_NO_RESPONSE;
        h->rirb_rp = (h->rirb_rp + 1u) % h->rirb_n;
        u32 res = rirb[2 * h->rirb_rp], ex = rirb[2 * h->rirb_rp + 1];
        wr8(h, R_RIRBSTS, 0x05); /* QEMU sends the next answers only after the response bit was cleared */
        if (!(ex & 0x10u) && (ex & 0xFu) == cad)
            return res;
    }
}

static void rings_stop(HdaCtrl *h)
{
    wr8(h, R_CORBCTL, 0);
    wr8(h, R_RIRBCTL, 0);
    HDA_WAIT(!(rd8(h, R_CORBCTL) & 2u) && !(rd8(h, R_RIRBCTL) & 2u), 10);
}

static u32 verb(void *ctx, unsigned cad, unsigned nid, u32 v)
{
    HdaCtrl *h = (HdaCtrl *)ctx;
    u32 cmd = (u32)(cad & 0xFu) << 28 | (u32)(nid & 0xFFu) << 20 | (v & 0xFFFFFu);
    if (!h->immediate)
    {
        u32 r = cmd_ring(h, cmd, cad);
        if (r != HDA_NO_RESPONSE)
        {
            h->ring_ok = 1;
            return r;
        }
        /* a codec that stops answering is the codec's problem once the rings have worked; if they never did,
         * switch to immediate commands for good */
        if (h->ring_ok)
            return r;
        serial_puts("HDA: no answer through the CORB / RIRB, trying immediate commands\n");
        rings_stop(h);
        h->immediate = 1;
    }
    return cmd_immediate(h, cmd);
}

static unsigned ring_size(HdaCtrl *h, u32 reg)
{
    u8 cap = (u8)(rd8(h, reg) >> 4);
    unsigned code = (cap & 4u) || !cap ? 2u : (cap & 2u) ? 1u : 0u; /* nothing listed: 256, like Linux */
    wr8(h, reg, (u8)((rd8(h, reg) & ~3u) | code));
    return code == 2 ? 256u : code == 1 ? 16u : 2u;
}

static void rings_start(HdaCtrl *h)
{
    rings_stop(h);
    u64 corb = h->page, rirb = h->page + PG_RIRB;
    wr32(h, R_CORBLBASE, (u32)corb);
    wr32(h, R_CORBUBASE, (u32)(corb >> 32));
    h->corb_n = ring_size(h, R_CORBSIZE);
    wr16(h, R_CORBWP, 0);
    wr16(h, R_CORBRP, 0x8000);
    HDA_WAIT(rd16(h, R_CORBRP) & 0x8000u, 5); /* some controllers never show the bit: go on anyway, like Linux */
    wr16(h, R_CORBRP, 0);
    HDA_WAIT(!(rd16(h, R_CORBRP) & 0x8000u), 5);
    wr8(h, R_CORBCTL, 2); /* run */

    wr32(h, R_RIRBLBASE, (u32)rirb);
    wr32(h, R_RIRBUBASE, (u32)(rirb >> 32));
    h->rirb_n = ring_size(h, R_RIRBSIZE);
    wr16(h, R_RIRBWP, 0x8000);
    wr16(h, R_RINTCNT, 1);
    h->rirb_rp = 0;
    wr8(h, R_RIRBSTS, 0x05);
    /* run + "response interrupt": no interrupt reaches the CPU (INTCTL = 0), but QEMU needs the status bit */
    wr8(h, R_RIRBCTL, 3);
}

/* ---- bring-up ---- */

static int controller_reset(HdaCtrl *h)
{
    wr32(h, R_INTCTL, 0);
    for (unsigned i = 0; i < h->iss + h->oss; i++)
        wr8(h, R_SD0 + 0x20 * i + SD_CTL, 0); /* every stream stopped */
    rings_stop(h);
    wr16(h, R_STATESTS, 0x7FFF);

    wr32(h, R_GCTL, rd32(h, R_GCTL) & ~GCTL_CRST);
    if (!HDA_WAIT(!(rd32(h, R_GCTL) & GCTL_CRST), 100))
        return 0;
    udelay(500); /* the spec asks for at least 100 us in reset */
    wr32(h, R_GCTL, rd32(h, R_GCTL) | GCTL_CRST);
    if (!HDA_WAIT(rd32(h, R_GCTL) & GCTL_CRST, 100))
        return 0;
    udelay(1000); /* codecs say they are there within 521 us (25 frames) of the reset ending */
    h->codecs = rd16(h, R_STATESTS) & 0x7FFFu;
    return 1;
}

static const char *device_name(unsigned d)
{
    return d == HDA_DEV_SPEAKER ? "Speaker" : d == HDA_DEV_HP_OUT ? "Headphone" : d == HDA_DEV_LINE_OUT ? "Line out" : "Out";
}

static void log_codec(const HdaCodec *c)
{
    serial_puts("HDA: codec ");
    serial_putd(c->cad);
    log_hex(" vendor ", c->vendor);
    serial_puts(", widgets ");
    serial_putd(c->count);
    serial_puts(", outputs ");
    serial_putd(c->nouts);
    serial_puts("\n");
    for (int o = 0; o < c->nouts; o++)
    {
        serial_puts("HDA:   ");
        serial_puts(device_name(c->out[o].device));
        serial_puts(":");
        for (unsigned i = 0; i < c->out[o].len; i++)
        {
            serial_puts(" ");
            serial_putd(c->out[o].nid[i]);
        }
        serial_puts("\n");
    }
}

static void bring_up(HdaCtrl *h, const PciDevice *pd)
{
    h->bus = pd->bus;
    h->dev = pd->dev;
    h->fn = pd->fn;
    u32 cmd = pci_cfg_read(pd->bus, pd->dev, pd->fn, 4);
    pci_cfg_write(pd->bus, pd->dev, pd->fn, 4, (cmd & 0xFFFFu) | 0x6u); /* memory space + bus master */
    u32 bar0 = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x10);
    u64 base = bar0 & ~0xFULL;
    if (((bar0 >> 1) & 3u) == 2u)
        base |= (u64)pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x14) << 32;
    if ((bar0 & 1u) || !base)
    {
        st_str(h, "No register window (BAR 0)");
        return;
    }
    h->regs = (volatile u8 *)base;
    u16 gcap = rd16(h, R_GCAP);
    h->oss = (gcap >> 12) & 0xFu;
    h->iss = (gcap >> 8) & 0xFu;
    serial_puts("HDA: controller at ");
    serial_putd(pd->bus);
    serial_puts(":");
    serial_putd(pd->dev);
    serial_puts(".");
    serial_putd(pd->fn);
    serial_puts(", version ");
    serial_putd(rd8(h, R_VMAJ));
    serial_puts(".");
    serial_putd(rd8(h, R_VMIN));
    serial_puts(", streams in ");
    serial_putd(h->iss);
    serial_puts(" out ");
    serial_putd(h->oss);
    serial_puts("\n");

    h->page = pmm_alloc_contiguous_tag(1, PMM_TAG_OTHER);
    if (!h->page)
    {
        st_str(h, "Out of memory");
        return;
    }
    zero((void *)h->page, PMM_PAGE_SIZE);

    if (!controller_reset(h))
    {
        st_str(h, "Controller reset did not finish");
        return;
    }
    log_hex("HDA: codecs answered (STATESTS) ", h->codecs);
    serial_puts("\n");
    if (!h->codecs)
    {
        st_str(h, "No codec answered after the reset");
        return;
    }
    rings_start(h);

    /* every codec is listed in the status; the first one with an analog output plays */
    static HdaCodec scratch;
    for (unsigned cad = 0; cad < 15; cad++)
    {
        if (!(h->codecs & (1u << cad)))
            continue;
        int e = hda_codec_scan(&scratch, cad, verb, h);
        if (e == 0)
            hda_codec_find_outputs(&scratch);
        log_codec(&scratch);
        if (h->status[0])
            st_str(h, "; ");
        st_str(h, "Codec ");
        st_hex(h, scratch.vendor >> 16, 4);
        st_str(h, ":");
        st_hex(h, scratch.vendor & 0xFFFFu, 4);
        if (e == HDA_ERR_SILENT)
            st_str(h, " stopped answering");
        else if (e == HDA_ERR_NO_AFG)
            st_str(h, " is not an audio codec");
        else if (e == HDA_ERR_TOO_BIG)
            st_str(h, " has too many widgets");
        else if (scratch.nouts == 0)
            st_str(h, ": no analog output (HDMI?)");
        else
        {
            st_str(h, ":");
            for (int o = 0; o < scratch.nouts; o++)
            {
                st_str(h, " ");
                st_str(h, device_name(scratch.out[o].device));
            }
            if (!h->has_codec)
            {
                h->codec = scratch;
                h->has_codec = 1;
            }
        }
    }
    h->working = h->has_codec && h->oss > 0;
}

void hda_init(void)
{
    NCTRL = 0;
    PRIMARY = -1;
    for (int i = 0; i < pci_count() && NCTRL < HDA_MAX_CTRL; i++)
    {
        const PciDevice *pd = pci_get(i);
        if (pd->class_code != 0x04 || pd->subclass != 0x03)
            continue;
        HdaCtrl *h = &CTRL[NCTRL++];
        zero(h, sizeof *h);
        bring_up(h, pd);
        if (h->working && PRIMARY < 0)
            PRIMARY = NCTRL - 1;
    }
    if (NCTRL == 0)
        serial_puts("HDA: no HD Audio controller\n");
}

int hda_ready(void) { return PRIMARY >= 0; }
int hda_count(void) { return NCTRL; }

int hda_status(int index, unsigned char loc[3], int *working, char *buf, int cap)
{
    if (index < 0 || index >= NCTRL)
        return -1;
    HdaCtrl *h = &CTRL[index];
    loc[0] = h->bus;
    loc[1] = h->dev;
    loc[2] = h->fn;
    *working = h->working;
    int n = 0;
    for (; n < cap - 1 && h->status[n]; n++)
        buf[n] = h->status[n];
    if (cap > 0)
        buf[n] = 0;
    return 0;
}

/* ---- playback ---- */

int hda_play_tone(unsigned hz, unsigned ms)
{
    if (PRIMARY < 0 || hz == 0 || hz >= RATE / 2 || ms == 0 || ms > 10000)
        return -1;
    HdaCtrl *h = &CTRL[PRIMARY];
    u32 sd = R_SD0 + 0x20 * h->iss; /* the first output stream */

    /* one buffer: the tone, then 100 ms of silence, in two halves (the BDL needs at least two entries) */
    u32 tone = RATE / 1000u * ms * 4u, total = tone + RATE / 10u * 4u;
    u32 half = (total / 2u + 127u) & ~127u;
    total = 2u * half;
    unsigned pages = (total + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE;
    u64 buf = pmm_alloc_contiguous_tag(pages, PMM_TAG_OTHER);
    if (!buf)
        return -1;
    short *s = (short *)buf;
    unsigned phase = 0, step = (unsigned)(((u64)hz << 32) / RATE);
    for (u32 i = 0; i < total / 4u; i++)
    {
        int v = 0;
        if (i * 4u < tone)
        {
            v = hda_sine_q15(phase) / 4; /* a quarter of full scale: -12 dB */
            phase += step;
        }
        s[2 * i] = s[2 * i + 1] = (short)v;
    }
    volatile u32 *bdl = (volatile u32 *)(h->page + PG_BDL);
    for (int e = 0; e < 2; e++)
    {
        u64 a = buf + (u64)e * half;
        bdl[4 * e] = (u32)a;
        bdl[4 * e + 1] = (u32)(a >> 32);
        bdl[4 * e + 2] = half;
        bdl[4 * e + 3] = 0; /* no interrupt on completion */
    }
    fence();

    /* stream reset, then program it */
    wr8(h, sd + SD_CTL, CTL_SRST);
    HDA_WAIT(rd8(h, sd + SD_CTL) & CTL_SRST, 10);
    wr8(h, sd + SD_CTL, 0);
    HDA_WAIT(!(rd8(h, sd + SD_CTL) & CTL_SRST), 10);
    wr8(h, sd + SD_STS, 0x1C);
    wr8(h, sd + SD_CTL + 2, (u8)(STREAM_TAG << 4));
    wr32(h, sd + SD_CBL, total);
    wr16(h, sd + SD_LVI, 1);
    u16 fmt = hda_stream_format(RATE, 16, 2);
    wr16(h, sd + SD_FMT, fmt);
    wr32(h, sd + SD_BDPL, (u32)(h->page + PG_BDL));
    wr32(h, sd + SD_BDPU, (u32)((h->page + PG_BDL) >> 32));
    wr32(h, R_DPLBASE, (u32)(h->page + PG_DPOS) | 1u);
    wr32(h, R_DPUBASE, (u32)((h->page + PG_DPOS) >> 32));

    hda_codec_setup_outputs(&h->codec, STREAM_TAG, fmt, verb, h);
    wr8(h, sd + SD_CTL, CTL_RUN);

    /* follow LPIB until the stream is inside the silence; give up if it stops moving for half a second */
    u32 last = rd32(h, sd + SD_LPIB), now = last, played = 0;
    int ok = 1;
    while (played < tone + RATE / 20u * 4u)
    {
        if (!HDA_WAIT((now = rd32(h, sd + SD_LPIB)) != last, 500))
        {
            ok = 0;
            break;
        }
        played += (now + total - last) % total;
        last = now;
    }
    u8 sts = rd8(h, sd + SD_STS);
    wr8(h, sd + SD_CTL, 0);
    HDA_WAIT(!(rd8(h, sd + SD_CTL) & CTL_RUN), 10);
    hda_codec_stop_outputs(&h->codec, verb, h);
    pmm_free_contiguous(buf, pages);

    serial_puts(ok ? "HDA: tone played, " : "HDA: the stream stopped moving, ");
    serial_putd(played);
    serial_puts(" bytes");
    log_hex(", stream status ", sts);
    serial_puts("\n");
    st_str(h, ok ? "; test tone played" : "; stream did not move");
    return ok ? 0 : -1;
}
