/* HDA codec graph: reads the widgets of a codec's audio function group, finds output paths pin <- ... <- DAC, and
 * routes / unmutes them. No hardware access here: every verb goes through the HdaVerbFn the caller gives, so the
 * host test (tests/test_hda.c) runs it against a pretend codec. Also the pure helpers of hda.h (sine, stream format).
 *
 * A codec answers "Get Parameter" verbs about itself. Node 0 is the root; its sub-nodes are function groups; the
 * audio function group's sub-nodes are the widgets. Each widget with a connection list says which widgets feed it
 * (a pin's list = what it can play, a mixer's list = what it adds up). The board maker's "configuration default"
 * of a pin says what is behind it: a built-in speaker, a headphone jack, nothing at all, ... */
#include "hda.h"
#include "hda_internal.h"

static u32 verb12(unsigned verb, unsigned payload) { return (u32)(verb << 8) | (payload & 0xFFu); }
static u32 verb4(unsigned verb, unsigned payload) { return (u32)(verb << 16) | (payload & 0xFFFFu); }

const HdaNode *hda_codec_node(const HdaCodec *c, unsigned nid)
{
    if (nid < c->first || nid >= c->first + c->count)
        return (const HdaNode *)0;
    return &c->node[nid - c->first];
}

/* Reads the connection list of a widget. Short form: 4 entries of 8 bits per answer, long form: 2 of 16 bits.
 * An entry with the top bit set is a range: every node id from the previous entry up to this one. */
static int read_connections(HdaCodec *c, HdaNode *n, unsigned nid, HdaVerbFn fn, void *ctx)
{
    u32 len = fn(ctx, c->cad, nid, verb12(HDA_VERB_GET_PARAM, HDA_PAR_CONN_LEN));
    if (len == HDA_NO_RESPONSE)
        return HDA_ERR_SILENT;
    int longform = (len >> 7) & 1u;
    unsigned total = len & 0x7Fu, per = longform ? 2u : 4u, bits = longform ? 16u : 8u;
    u32 mask = longform ? 0x7FFFu : 0x7Fu, range = longform ? 0x8000u : 0x80u;
    unsigned prev = 0;
    for (unsigned i = 0; i < total; i += per)
    {
        u32 r = fn(ctx, c->cad, nid, verb12(HDA_VERB_GET_CONN_LIST, i));
        if (r == HDA_NO_RESPONSE)
            return HDA_ERR_SILENT;
        for (unsigned j = 0; j < per && i + j < total; j++)
        {
            u32 e = (r >> (bits * j)) & (longform ? 0xFFFFu : 0xFFu);
            unsigned id = e & mask;
            unsigned from = (e & range) && prev && prev < id ? prev + 1 : id;
            for (unsigned k = from; k <= id && n->nconn < HDA_MAX_CONN; k++)
                n->conn[n->nconn++] = (u8)(k > 0xFF ? 0 : k); /* node ids above 255 do not occur in practice */
            prev = id;
        }
    }
    return 0;
}

int hda_codec_scan(HdaCodec *c, unsigned cad, HdaVerbFn fn, void *ctx)
{
    volatile u8 *z = (volatile u8 *)c; /* no memset in the kernel */
    for (unsigned long i = 0; i < sizeof *c; i++)
        z[i] = 0;
    c->cad = cad;

    c->vendor = fn(ctx, cad, 0, verb12(HDA_VERB_GET_PARAM, HDA_PAR_VENDOR));
    u32 nodes = fn(ctx, cad, 0, verb12(HDA_VERB_GET_PARAM, HDA_PAR_NODES));
    if (c->vendor == HDA_NO_RESPONSE || nodes == HDA_NO_RESPONSE)
        return HDA_ERR_SILENT;
    unsigned fg = (nodes >> 16) & 0xFFu, fgs = nodes & 0xFFu;
    for (unsigned i = 0; i < fgs && !c->afg; i++)
    {
        u32 t = fn(ctx, cad, fg + i, verb12(HDA_VERB_GET_PARAM, HDA_PAR_FG_TYPE));
        if (t != HDA_NO_RESPONSE && (t & 0xFFu) == 0x01u)
            c->afg = fg + i;
    }
    if (!c->afg)
        return HDA_ERR_NO_AFG;

    u32 w = fn(ctx, cad, c->afg, verb12(HDA_VERB_GET_PARAM, HDA_PAR_NODES));
    u32 fg_in = fn(ctx, cad, c->afg, verb12(HDA_VERB_GET_PARAM, HDA_PAR_IN_AMP));
    u32 fg_amp = fn(ctx, cad, c->afg, verb12(HDA_VERB_GET_PARAM, HDA_PAR_OUT_AMP));
    if (w == HDA_NO_RESPONSE || fg_in == HDA_NO_RESPONSE || fg_amp == HDA_NO_RESPONSE)
        return HDA_ERR_SILENT;
    c->first = (w >> 16) & 0xFFu;
    c->count = w & 0xFFu;
    if (c->count > HDA_MAX_NODES)
        return HDA_ERR_TOO_BIG;

    for (unsigned i = 0; i < c->count; i++)
    {
        unsigned nid = c->first + i;
        HdaNode *n = &c->node[i];
        n->caps = fn(ctx, cad, nid, verb12(HDA_VERB_GET_PARAM, HDA_PAR_WCAPS));
        if (n->caps == HDA_NO_RESPONSE)
            return HDA_ERR_SILENT;
        n->in_amp = fg_in;
        n->out_amp = fg_amp;
        if ((n->caps & HDA_WCAP_IN_AMP) && (n->caps & HDA_WCAP_AMP_OVRD))
            n->in_amp = fn(ctx, cad, nid, verb12(HDA_VERB_GET_PARAM, HDA_PAR_IN_AMP));
        if ((n->caps & HDA_WCAP_OUT_AMP) && (n->caps & HDA_WCAP_AMP_OVRD))
            n->out_amp = fn(ctx, cad, nid, verb12(HDA_VERB_GET_PARAM, HDA_PAR_OUT_AMP));
        if (HDA_WCAP_TYPE(n->caps) == HDA_W_PIN)
        {
            n->pincaps = fn(ctx, cad, nid, verb12(HDA_VERB_GET_PARAM, HDA_PAR_PINCAPS));
            n->config = fn(ctx, cad, nid, verb12(HDA_VERB_GET_CONFIG, 0));
            if (n->pincaps == HDA_NO_RESPONSE || n->config == HDA_NO_RESPONSE)
                return HDA_ERR_SILENT;
        }
        if (n->caps & HDA_WCAP_CONN_LIST)
        {
            int e = read_connections(c, n, nid, fn, ctx);
            if (e)
                return e;
        }
    }
    return 0;
}

static int dac_used(const HdaCodec *c, unsigned nid)
{
    for (int i = 0; i < c->nouts; i++)
        if (c->out[i].nid[c->out[i].len - 1] == nid)
            return 1;
    return 0;
}

/* Depth-first search from p->nid[depth] towards an analog DAC, through mixers and selectors only. */
static int walk(const HdaCodec *c, HdaPath *p, unsigned nid, unsigned depth, int fresh_dac)
{
    const HdaNode *n = hda_codec_node(c, nid);
    if (!n || depth >= HDA_MAX_PATH)
        return 0;
    for (unsigned i = 0; i < depth; i++)
        if (p->nid[i] == nid) /* a loop in the graph */
            return 0;
    p->nid[depth] = (u8)nid;
    unsigned type = HDA_WCAP_TYPE(n->caps);
    if (depth > 0 && type == HDA_W_OUTPUT)
    {
        if ((n->caps & HDA_WCAP_DIGITAL) || (fresh_dac && dac_used(c, nid)))
            return 0;
        p->len = (u8)(depth + 1);
        return 1;
    }
    if (depth > 0 && type != HDA_W_MIXER && type != HDA_W_SELECTOR)
        return 0;
    if (!(n->caps & HDA_WCAP_CONN_LIST))
        return 0;
    for (unsigned i = 0; i < n->nconn; i++)
    {
        p->sel[depth] = (u8)i;
        if (walk(c, p, n->conn[i], depth + 1, fresh_dac))
            return 1;
    }
    return 0;
}

/* An analog pin that can drive an output and is connected to something. */
static int output_pin(const HdaNode *n)
{
    return HDA_WCAP_TYPE(n->caps) == HDA_W_PIN && !(n->caps & HDA_WCAP_DIGITAL) && (n->pincaps & HDA_PINCAP_OUT) &&
           HDA_CFG_CONN(n->config) != 1u;
}

static void add_pin(HdaCodec *c, unsigned nid)
{
    for (int i = 0; i < c->nouts; i++)
        if (c->out[i].nid[0] == nid)
            return;
    HdaPath p;
    volatile u8 *z = (volatile u8 *)&p;
    for (unsigned long i = 0; i < sizeof p; i++)
        z[i] = 0;
    /* a DAC of its own if there is one (speaker and headphone on separate DACs, like Linux), else share one */
    if (!walk(c, &p, nid, 0, 1) && !walk(c, &p, nid, 0, 0))
        return;
    p.device = (u8)HDA_CFG_DEVICE(hda_codec_node(c, nid)->config);
    c->out[c->nouts++] = p;
}

int hda_codec_find_outputs(HdaCodec *c)
{
    static const unsigned order[3] = {HDA_DEV_SPEAKER, HDA_DEV_HP_OUT, HDA_DEV_LINE_OUT};
    c->nouts = 0;
    for (int k = 0; k < 3; k++)
        for (unsigned i = 0; i < c->count && c->nouts < HDA_MAX_OUTS; i++)
            if (output_pin(&c->node[i]) && HDA_CFG_DEVICE(c->node[i].config) == order[k])
                add_pin(c, c->first + i);
    /* no pin is described as one of those (wrong or empty configuration): take any output pin */
    for (unsigned i = 0; i < c->count && c->nouts == 0; i++)
        if (output_pin(&c->node[i]))
            add_pin(c, c->first + i);
    return c->nouts;
}

/* Amp gain/mute payload (spec 7.3.3.7): output 15, input 14, left 13, right 12, index 11:8, mute 7, gain 6:0.
 * Unmuted at the amp's 0 dB step (the "offset" of its capabilities). */
static u32 amp_unmute(int output, unsigned index, u32 ampcaps)
{
    return (output ? 1u << 15 : 1u << 14) | (3u << 12) | ((index & 0xFu) << 8) | (ampcaps & 0x7Fu);
}

void hda_codec_setup_outputs(const HdaCodec *c, unsigned tag, u16 fmt, HdaVerbFn fn, void *ctx)
{
    fn(ctx, c->cad, c->afg, verb12(HDA_VERB_SET_POWER, 0)); /* D0 = fully on */
    for (int o = 0; o < c->nouts; o++)
    {
        const HdaPath *p = &c->out[o];
        for (unsigned i = 0; i < p->len; i++)
        {
            unsigned nid = p->nid[i];
            const HdaNode *n = hda_codec_node(c, nid);
            unsigned type = HDA_WCAP_TYPE(n->caps);
            if (n->caps & HDA_WCAP_POWER)
                fn(ctx, c->cad, nid, verb12(HDA_VERB_SET_POWER, 0));
            if (type == HDA_W_OUTPUT)
            {
                fn(ctx, c->cad, nid, verb4(HDA_VERB4_SET_FORMAT, fmt));
                fn(ctx, c->cad, nid, verb12(HDA_VERB_SET_STREAM, (tag & 0xFu) << 4)); /* channels 0 and 1 of the stream */
            }
            else if (type == HDA_W_MIXER)
            {
                /* a mixer's input amps are one per connection: unmute the one on the path */
                if (n->caps & HDA_WCAP_IN_AMP)
                    fn(ctx, c->cad, nid, verb4(HDA_VERB4_SET_AMP, amp_unmute(0, p->sel[i], n->in_amp)));
            }
            else if (n->nconn > 1)
                fn(ctx, c->cad, nid, verb12(HDA_VERB_SET_CONN_SEL, p->sel[i])); /* pin or selector: pick the input */
            if (n->caps & HDA_WCAP_OUT_AMP)
                fn(ctx, c->cad, nid, verb4(HDA_VERB4_SET_AMP, amp_unmute(1, 0, n->out_amp)));
            if (type == HDA_W_PIN)
            {
                u32 ctl = HDA_PINCTL_OUT_EN;
                if (p->device == HDA_DEV_HP_OUT && (n->pincaps & HDA_PINCAP_HP_DRIVE))
                    ctl |= HDA_PINCTL_HP_EN;
                fn(ctx, c->cad, nid, verb12(HDA_VERB_SET_PIN_CTL, ctl));
                if (n->pincaps & HDA_PINCAP_EAPD) /* external amplifier power down: bit 1 = the amplifier ON */
                    fn(ctx, c->cad, nid, verb12(HDA_VERB_SET_EAPD, 0x02));
            }
        }
    }
}

void hda_codec_stop_outputs(const HdaCodec *c, HdaVerbFn fn, void *ctx)
{
    for (int o = 0; o < c->nouts; o++)
        fn(ctx, c->cad, c->out[o].nid[c->out[o].len - 1], verb12(HDA_VERB_SET_STREAM, 0));
}

/* ---- pure helpers ---- */

/* Taylor series to x^7 on [-pi/2, pi/2] (worst error 1.6e-4 at the ends), in Q30 fixed point. */
int hda_sine_q15(unsigned phase)
{
    long long s = (int)phase; /* the angle as -pi .. pi: 2^31 = pi */
    if (s > (1LL << 30))
        s = (1LL << 31) - s; /* sin(pi - x) = sin(x) */
    else if (s < -(1LL << 30))
        s = -(1LL << 31) - s;
    const long long one = 1LL << 30, pi_q30 = 3373259426LL; /* pi * 2^30 */
    long long x = s * pi_q30 >> 31;                         /* radians, Q30 */
    long long x2 = x * x >> 30;
    long long t = one - x2 / 42;
    t = one - (x2 * t >> 30) / 20;
    t = one - (x2 * t >> 30) / 6;
    long long r = (x * t >> 30) >> 15;
    if (r > 32767)
        r = 32767;
    if (r < -32767)
        r = -32767;
    return (int)r;
}

unsigned short hda_stream_format(unsigned rate, unsigned bits, unsigned channels)
{
    unsigned b;
    switch (bits)
    {
    case 8: b = 0; break;
    case 16: b = 1; break;
    case 20: b = 2; break;
    case 24: b = 3; break;
    case 32: b = 4; break;
    default: return 0;
    }
    if (channels < 1 || channels > 16)
        return 0;
    /* rate = base (48 kHz, or 44.1 kHz with bit 14) * mult (bits 13:11, 1..4) / div (bits 10:8, 1..8) */
    for (unsigned base = 0; base < 2; base++)
        for (unsigned mult = 1; mult <= 4; mult++)
            for (unsigned div = 1; div <= 8; div++)
            {
                unsigned hz = base ? 44100u : 48000u;
                if ((hz * mult) % div == 0 && hz * mult / div == rate)
                    return (unsigned short)(base << 14 | (mult - 1) << 11 | (div - 1) << 8 | b << 4 | (channels - 1));
            }
    return 0;
}
