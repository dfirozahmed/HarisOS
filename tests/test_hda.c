/* Host test for hda_codec.c against pretend codecs: a table of widgets answers the Get Parameter / configuration /
 * connection list verbs, and every Set verb is recorded so the test can check what the driver would write.
 * The "laptop" graph is made up in the shape of a Realtek laptop codec (two DACs, two mixers, a built-in speaker and
 * a headphone jack, a microphone, an S/PDIF output and an unconnected pin); it is not copied from a data sheet.
 * This proves the scan, the path search and the routing verbs; whether a real codec agrees needs real hardware. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/drivers/audio/hda/hda_codec.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- the pretend codec ---- */
typedef struct
{
    u32 caps, pincaps, config, in_amp, out_amp;
    u32 fg_type, nodes; /* root and function groups only */
    unsigned nconn;
    u16 conn[16]; /* raw entries (short form: top bit 0x80 = range) */
    int longform;
} FNode;

static FNode N[256];
static unsigned CAD = 2;     /* the address the codec answers at */
static int SILENT_AFTER = -1; /* stop answering after this many verbs (-1 = never) */
static int VERBS;
static u32 LOG[512];         /* Set verbs: nid << 20 | verb */
static int NLOG;

static u32 codec(void *ctx, unsigned cad, unsigned nid, u32 v)
{
    (void)ctx;
    if (cad != CAD || nid > 255 || (SILENT_AFTER >= 0 && VERBS >= SILENT_AFTER))
        return HDA_NO_RESPONSE;
    VERBS++;
    unsigned top = (v >> 16) & 0xFu;
    const FNode *n = &N[nid];
    if (top == 0xF && ((v >> 8) & 0xFFFu) == HDA_VERB_GET_PARAM)
    {
        switch (v & 0xFFu)
        {
        case HDA_PAR_VENDOR: return 0x10EC0999u;
        case HDA_PAR_NODES: return n->nodes;
        case HDA_PAR_FG_TYPE: return n->fg_type;
        case HDA_PAR_WCAPS: return n->caps;
        case HDA_PAR_PINCAPS: return n->pincaps;
        case HDA_PAR_IN_AMP: return n->in_amp;
        case HDA_PAR_OUT_AMP: return n->out_amp;
        case HDA_PAR_CONN_LEN: return n->nconn | (n->longform ? 0x80u : 0);
        default: return 0;
        }
    }
    if (top == 0xF && ((v >> 8) & 0xFFFu) == HDA_VERB_GET_CONFIG)
        return n->config;
    if (top == 0xF && ((v >> 8) & 0xFFFu) == HDA_VERB_GET_CONN_LIST)
    {
        unsigned i = v & 0xFFu, per = n->longform ? 2 : 4, bits = n->longform ? 16 : 8;
        u32 r = 0;
        for (unsigned j = 0; j < per && i + j < n->nconn; j++)
            r |= (u32)n->conn[i + j] << (bits * j);
        return r;
    }
    if (NLOG < 512)
        LOG[NLOG++] = (u32)nid << 20 | (v & 0xFFFFFu);
    return 0;
}

static int sent(unsigned nid, u32 v)
{
    for (int i = 0; i < NLOG; i++)
        if (LOG[i] == ((u32)nid << 20 | v))
            return 1;
    return 0;
}

static int sent_to(unsigned nid)
{
    int k = 0;
    for (int i = 0; i < NLOG; i++)
        k += (LOG[i] >> 20) == nid;
    return k;
}

#define W(type) ((u32)(type) << 20)
#define DAC_AMP 0x00035757u /* step size 3, 0x57 steps, 0 dB at step 0x57 */
#define MIX_AMP 0x80000000u /* mute only */

static void conn(unsigned nid, unsigned n, const u16 *c)
{
    N[nid].nconn = n;
    for (unsigned i = 0; i < n; i++)
        N[nid].conn[i] = c[i];
}

static void laptop(void)
{
    memset(N, 0, sizeof N);
    CAD = 2;
    SILENT_AFTER = -1;
    N[0].nodes = (1u << 16) | 1u;
    N[1].fg_type = 0x01;
    N[1].nodes = (2u << 16) | 0x20u; /* widgets 0x02 .. 0x21 */
    N[1].in_amp = 0x00270300u;
    N[1].out_amp = 0x00012020u;
    for (unsigned i = 2; i < 0x22; i++)
        N[i].caps = W(0xF); /* vendor-defined widgets: ignored */
    N[0x02].caps = N[0x03].caps = W(HDA_W_OUTPUT) | HDA_WCAP_OUT_AMP | HDA_WCAP_AMP_OVRD | HDA_WCAP_POWER | 1u;
    N[0x02].out_amp = N[0x03].out_amp = DAC_AMP;
    N[0x06].caps = W(HDA_W_OUTPUT) | HDA_WCAP_DIGITAL; /* S/PDIF converter */
    N[0x09].caps = W(HDA_W_INPUT) | HDA_WCAP_CONN_LIST;  /* ADC */
    conn(0x09, 1, (const u16[]){0x12});
    N[0x0B].caps = W(HDA_W_MIXER) | HDA_WCAP_IN_AMP | HDA_WCAP_CONN_LIST; /* the input mixer: never on an output path */
    conn(0x0B, 1, (const u16[]){0x12});
    N[0x0C].caps = N[0x0D].caps = W(HDA_W_MIXER) | HDA_WCAP_IN_AMP | HDA_WCAP_AMP_OVRD | HDA_WCAP_CONN_LIST;
    N[0x0C].in_amp = N[0x0D].in_amp = MIX_AMP;
    conn(0x0C, 2, (const u16[]){0x02, 0x0B});
    conn(0x0D, 2, (const u16[]){0x03, 0x0B});
    N[0x12].caps = W(HDA_W_PIN);
    N[0x12].pincaps = 1u << 5;          /* input only: the built-in microphone */
    N[0x12].config = 0x90A60130u;
    N[0x14].caps = W(HDA_W_PIN) | HDA_WCAP_OUT_AMP | HDA_WCAP_CONN_LIST;
    N[0x14].pincaps = HDA_PINCAP_OUT | HDA_PINCAP_EAPD;
    N[0x14].config = 0x90170110u;       /* fixed, speaker */
    conn(0x14, 2, (const u16[]){0x0C, 0x0D});
    N[0x1B].caps = W(HDA_W_PIN) | HDA_WCAP_CONN_LIST;
    N[0x1B].pincaps = HDA_PINCAP_OUT;
    N[0x1B].config = 0x411111F0u;       /* nothing connected */
    conn(0x1B, 2, (const u16[]){0x0C, 0x0D});
    N[0x1E].caps = W(HDA_W_PIN) | HDA_WCAP_DIGITAL | HDA_WCAP_CONN_LIST;
    N[0x1E].pincaps = HDA_PINCAP_OUT;
    N[0x1E].config = 0x01441130u;       /* S/PDIF out jack */
    conn(0x1E, 1, (const u16[]){0x06});
    N[0x21].caps = W(HDA_W_PIN) | HDA_WCAP_OUT_AMP | HDA_WCAP_CONN_LIST;
    N[0x21].pincaps = HDA_PINCAP_OUT | HDA_PINCAP_HP_DRIVE | HDA_PINCAP_EAPD;
    N[0x21].config = 0x02211020u;       /* jack, headphone */
    conn(0x21, 2, (const u16[]){0x0C, 0x0D});
}

/* QEMU's hda-duplex shape: a DAC and a line out pin behind it */
static void small(u32 pin_config)
{
    memset(N, 0, sizeof N);
    CAD = 0;
    SILENT_AFTER = -1;
    N[0].nodes = (1u << 16) | 1u;
    N[1].fg_type = 0x01;
    N[1].nodes = (2u << 16) | 2u;
    N[1].out_amp = 0x80004A4Au;
    N[2].caps = W(HDA_W_OUTPUT) | HDA_WCAP_OUT_AMP | 1u;
    N[3].caps = W(HDA_W_PIN) | HDA_WCAP_CONN_LIST;
    N[3].pincaps = HDA_PINCAP_OUT;
    N[3].config = pin_config;
    conn(3, 1, (const u16[]){2});
}

static HdaCodec C;

int main(void)
{
    /* ---- laptop: scan ---- */
    laptop();
    CHECK(hda_codec_scan(&C, 2, codec, 0) == 0);
    CHECK(C.vendor == 0x10EC0999u && C.afg == 1 && C.first == 2 && C.count == 0x20);
    CHECK(hda_codec_node(&C, 1) == NULL && hda_codec_node(&C, 0x22) == NULL);
    const HdaNode *dac = hda_codec_node(&C, 0x02);
    CHECK(dac && dac->out_amp == DAC_AMP);                     /* its own amp caps (override bit) */
    CHECK(hda_codec_node(&C, 0x14)->out_amp == 0x00012020u);   /* the function group's */
    CHECK(hda_codec_node(&C, 0x0C)->in_amp == MIX_AMP && hda_codec_node(&C, 0x0B)->in_amp == 0x00270300u);
    CHECK(hda_codec_node(&C, 0x21)->nconn == 2 && hda_codec_node(&C, 0x21)->conn[1] == 0x0D);
    CHECK(hda_codec_node(&C, 0x14)->config == 0x90170110u);

    /* ---- laptop: paths. Speaker first; the headphone gets the other DAC; unconnected / digital pins are skipped ---- */
    CHECK(hda_codec_find_outputs(&C) == 2);
    CHECK(C.out[0].device == HDA_DEV_SPEAKER && C.out[0].len == 3);
    CHECK(C.out[0].nid[0] == 0x14 && C.out[0].nid[1] == 0x0C && C.out[0].nid[2] == 0x02);
    CHECK(C.out[0].sel[0] == 0 && C.out[0].sel[1] == 0);
    CHECK(C.out[1].device == HDA_DEV_HP_OUT && C.out[1].len == 3);
    CHECK(C.out[1].nid[0] == 0x21 && C.out[1].nid[1] == 0x0D && C.out[1].nid[2] == 0x03);
    CHECK(C.out[1].sel[0] == 1 && C.out[1].sel[1] == 0);

    /* ---- laptop: setup ---- */
    NLOG = 0;
    hda_codec_setup_outputs(&C, 1, 0x0011, codec, 0);
    CHECK(sent(0x01, 0x70500));                    /* function group to D0 */
    CHECK(sent(0x02, 0x70500) && sent(0x03, 0x70500)); /* DACs have power control */
    CHECK(sent(0x02, 0x20011) && sent(0x03, 0x20011)); /* format 48 kHz 16-bit stereo */
    CHECK(sent(0x02, 0x70610) && sent(0x03, 0x70610)); /* stream 1, channel 0 */
    CHECK(sent(0x02, 0x3B057) && sent(0x03, 0x3B057)); /* output amp, both sides, unmuted at 0 dB (0x57) */
    CHECK(sent(0x0C, 0x37000) && sent(0x0D, 0x37000)); /* mixer input 0 (the DAC) unmuted */
    CHECK(!sent(0x0C, 0x70100));                   /* a mixer has no connection select */
    CHECK(sent(0x14, 0x70100) && sent(0x21, 0x70101)); /* pins pick their mixer */
    CHECK(sent(0x14, 0x70740) && sent(0x21, 0x707C0)); /* out enable; headphone drive on the jack */
    CHECK(sent(0x14, 0x70C02) && sent(0x21, 0x70C02)); /* external amplifier on */
    CHECK(sent(0x14, 0x3B020));                    /* pin output amp at the function group's 0 dB */
    CHECK(sent_to(0x1B) == 0 && sent_to(0x1E) == 0 && sent_to(0x06) == 0 && sent_to(0x0B) == 0);
    NLOG = 0;
    hda_codec_stop_outputs(&C, codec, 0);
    CHECK(NLOG == 2 && sent(0x02, 0x70600) && sent(0x03, 0x70600));

    /* ---- one DAC only: the headphone shares it ---- */
    laptop();
    N[0x0D].nconn = 1;
    N[0x0D].conn[0] = 0x02;
    N[0x03].caps = W(0xF);
    CHECK(hda_codec_scan(&C, 2, codec, 0) == 0 && hda_codec_find_outputs(&C) == 2);
    CHECK(C.out[1].nid[0] == 0x21 && C.out[1].nid[C.out[1].len - 1] == 0x02);

    /* ---- a selector between pin and DAC, its list given as a range (0x02 .. 0x05): expanded ---- */
    laptop();
    N[0x14].nconn = 1;
    N[0x14].conn[0] = 0x0F;
    N[0x0F].caps = W(HDA_W_SELECTOR) | HDA_WCAP_CONN_LIST;
    conn(0x0F, 2, (const u16[]){0x04, 0x80 | 0x07}); /* 4, then range up to 7: 4 5 6 7 */
    N[0x07].caps = W(HDA_W_OUTPUT);
    CHECK(hda_codec_scan(&C, 2, codec, 0) == 0);
    CHECK(hda_codec_node(&C, 0x0F)->nconn == 4 && hda_codec_node(&C, 0x0F)->conn[3] == 0x07);
    CHECK(hda_codec_find_outputs(&C) == 2);
    CHECK(C.out[0].len == 3 && C.out[0].nid[1] == 0x0F && C.out[0].nid[2] == 0x07 && C.out[0].sel[1] == 3);
    NLOG = 0;
    hda_codec_setup_outputs(&C, 1, 0x0011, codec, 0);
    CHECK(sent(0x0F, 0x70103) && !sent(0x14, 0x70100)); /* the selector picks entry 3; the pin has one input */

    /* ---- long-form connection list ---- */
    laptop();
    N[0x21].longform = 1;
    conn(0x21, 3, (const u16[]){0x0C, 0x0D, 0x0B});
    CHECK(hda_codec_scan(&C, 2, codec, 0) == 0);
    CHECK(hda_codec_node(&C, 0x21)->nconn == 3 && hda_codec_node(&C, 0x21)->conn[2] == 0x0B);

    /* ---- a loop in the graph does not hang the search ---- */
    laptop();
    conn(0x0C, 1, (const u16[]){0x0D});
    conn(0x0D, 1, (const u16[]){0x0C});
    CHECK(hda_codec_scan(&C, 2, codec, 0) == 0 && hda_codec_find_outputs(&C) == 0);

    /* ---- QEMU shape: line out jack ---- */
    small(0x01014010u); /* jack, line out */
    CHECK(hda_codec_scan(&C, 0, codec, 0) == 0 && hda_codec_find_outputs(&C) == 1);
    CHECK(C.out[0].device == HDA_DEV_LINE_OUT && C.out[0].len == 2 && C.out[0].nid[0] == 3 && C.out[0].nid[1] == 2);
    NLOG = 0;
    hda_codec_setup_outputs(&C, 1, 0x0011, codec, 0);
    CHECK(sent(2, 0x3B04A) && sent(3, 0x70740) && !sent(3, 0x70C02) && !sent(3, 0x70100));
    /* a pin described as "other": taken by the fallback */
    small(0x01F14010u);
    CHECK(hda_codec_scan(&C, 0, codec, 0) == 0 && hda_codec_find_outputs(&C) == 1 && C.out[0].device == 0xF);
    /* ... but not when it says nothing is connected */
    small(0x41F14010u);
    CHECK(hda_codec_scan(&C, 0, codec, 0) == 0 && hda_codec_find_outputs(&C) == 0);

    /* ---- errors ---- */
    laptop();
    CHECK(hda_codec_scan(&C, 3, codec, 0) == HDA_ERR_SILENT); /* nothing at that address */
    SILENT_AFTER = 20;
    VERBS = 0;
    CHECK(hda_codec_scan(&C, 2, codec, 0) == HDA_ERR_SILENT);  /* stops answering half way */
    laptop();
    N[1].fg_type = 0x02; /* a modem function group */
    CHECK(hda_codec_scan(&C, 2, codec, 0) == HDA_ERR_NO_AFG);
    laptop();
    N[1].nodes = (2u << 16) | 200u;
    CHECK(hda_codec_scan(&C, 2, codec, 0) == HDA_ERR_TOO_BIG);

    /* ---- sine ---- */
    int worst = 0;
    for (unsigned k = 0; k < 1u << 16; k++)
    {
        unsigned ph = k << 16 | (k * 2654435761u >> 16);
        double want = sin(2.0 * M_PI * (double)ph / 4294967296.0) * 32768.0;
        int e = (int)fabs(hda_sine_q15(ph) - want);
        if (e > worst)
            worst = e;
    }
    CHECK(worst < 8);
    CHECK(hda_sine_q15(0) == 0 && hda_sine_q15(1u << 30) >= 32760 && hda_sine_q15(3u << 30) <= -32760);

    /* ---- volume: the gain step for a percentage ---- */
    int mute;
    CHECK(hda_amp_gain(DAC_AMP, 100, &mute) == 0x57 && !mute);              /* 100 % = the 0 dB step */
    CHECK(hda_amp_gain(DAC_AMP, 50, &mute) == 44 && !mute);                 /* 0x57 * 0.5 = 43.5, rounded */
    CHECK(hda_amp_gain(DAC_AMP, 1, &mute) == 1 && !mute);
    CHECK(hda_amp_gain(DAC_AMP, 0, &mute) == 0 && !mute);                   /* it cannot mute: the lowest step */
    CHECK(hda_amp_gain(0x80004A4Au, 0, &mute) == 0 && mute);                /* QEMU's amp: mute capable */
    CHECK(hda_amp_gain(0x80004A4Au, 100, &mute) == 0x4A && !mute);
    CHECK(hda_amp_gain(0x00000F00u, 100, &mute) == 15 && !mute);            /* no 0 dB offset: the last step */
    CHECK(hda_amp_gain(0x00000F20u, 100, &mute) == 15);                     /* an offset above the last step is clamped */
    CHECK(hda_amp_gain(DAC_AMP, 500, &mute) == 0x57);                       /* clamped */

    /* ---- volume on the laptop graph: the DACs' amps, not the pins' ---- */
    laptop();
    CHECK(hda_codec_scan(&C, 2, codec, 0) == 0 && hda_codec_find_outputs(&C) == 2);
    NLOG = 0;
    CHECK(hda_codec_set_volume(&C, 50, codec, 0) == 2);
    CHECK(NLOG == 2);                                           /* one verb per output path, nothing else */
    CHECK(sent(0x02, 0x3B000 | 44) && sent(0x03, 0x3B000 | 44)); /* output + left + right, gain 44 */
    NLOG = 0;
    CHECK(hda_codec_set_volume(&C, 100, codec, 0) == 2 && sent(0x02, 0x3B057));
    /* a codec whose DAC has no amp: the pin's amp (the nearest one along the path) is used */
    small(0x01014010u);
    N[2].caps = W(HDA_W_OUTPUT) | 1u;                    /* DAC without an amp */
    N[3].caps |= HDA_WCAP_OUT_AMP | HDA_WCAP_AMP_OVRD;   /* the pin has one */
    N[3].out_amp = 0x80002020u;
    CHECK(hda_codec_scan(&C, 0, codec, 0) == 0 && hda_codec_find_outputs(&C) == 1);
    NLOG = 0;
    CHECK(hda_codec_set_volume(&C, 0, codec, 0) == 1 && sent(3, 0x3B080)); /* muted: bit 7, gain 0 */
    /* no amp at all (and amps with no steps): nothing to set */
    N[3].caps &= ~(u32)HDA_WCAP_OUT_AMP;
    CHECK(hda_codec_scan(&C, 0, codec, 0) == 0 && hda_codec_find_outputs(&C) == 1);
    CHECK(hda_codec_set_volume(&C, 50, codec, 0) == 0);

    /* ---- which controllers need the snoop bit ---- */
    CHECK(hda_needs_ati_snoop(0x1022, 0x15E3) && hda_needs_ati_snoop(0x1022, 0x1487) && hda_needs_ati_snoop(0x1002, 0x4383));
    CHECK(!hda_needs_ati_snoop(0x1002, 0xAA01));  /* a GPU's HDMI audio */
    CHECK(!hda_needs_ati_snoop(0x8086, 0x293E) && !hda_needs_ati_snoop(0x1022, 0x1234));

    /* ---- stream format ---- */
    CHECK(hda_stream_format(48000, 16, 2) == 0x0011);
    CHECK(hda_stream_format(44100, 16, 2) == 0x4011);
    CHECK(hda_stream_format(96000, 24, 2) == 0x0831);
    CHECK(hda_stream_format(8000, 16, 1) == 0x0510);
    CHECK(hda_stream_format(192000, 32, 8) == 0x1847);
    CHECK(hda_stream_format(22050, 16, 2) == 0x4111);
    CHECK(hda_stream_format(12345, 16, 2) == 0);
    CHECK(hda_stream_format(48000, 12, 2) == 0 && hda_stream_format(48000, 16, 0) == 0 && hda_stream_format(48000, 16, 17) == 0);

    if (!fails)
        printf("hda tests passed\n");
    return fails ? 1 : 0;
}
