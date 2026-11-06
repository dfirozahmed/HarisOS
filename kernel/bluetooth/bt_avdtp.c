/* Bluetooth AVDTP (Audio/Video Distribution Transport Protocol): the control side of A2DP music streaming (stage 7).
 *
 * To play music a source (us) and a sink (the headset) first agree on how: over an L2CAP channel to PSM 0x19 (the "signalling channel")
 * the source asks the sink which "stream endpoints" it has (Discover), what each can do (Get Capabilities), picks one setting of the SBC
 * codec the sink allows (Set Configuration) and opens the stream (Open). Then a second L2CAP channel to the same PSM, the "media channel",
 * is made, and Start tells the sink that audio follows on it. Stopping is Close. AVDTP 1.3 / A2DP 1.3, checked against BlueZ
 * profiles/audio/avdtp.c, avdtp.h and a2dp-codecs.h.
 *
 * Signalling message: byte 0 = transaction label (4 bits) | packet type (2 bits, 0 = single) | message type (2 bits: 0 command, 1 general
 * reject, 2 accept, 3 reject); byte 1 = signal id (6 bits); then the parameters.
 *
 * This file has two parts: pure functions that build and read the parameters (host-tested), and the session that runs the sequence on top of
 * the L2CAP channels of bt_link.c. */
#include "bluetooth.h"

#define AVDTP_PSM 0x0019

#define SIG_DISCOVER 0x01
#define SIG_GET_CAPABILITIES 0x02
#define SIG_SET_CONFIGURATION 0x03
#define SIG_OPEN 0x06
#define SIG_START 0x07
#define SIG_CLOSE 0x08
#define SIG_SUSPEND 0x09
#define SIG_GET_ALL_CAPABILITIES 0x0C

#define MSG_COMMAND 0
#define MSG_GEN_REJECT 1
#define MSG_ACCEPT 2
#define MSG_REJECT 3

#define CAP_MEDIA_TRANSPORT 0x01
#define CAP_MEDIA_CODEC 0x07
#define CAP_DELAY_REPORTING 0x08

#define OUR_SEID 1 /* the number of our own stream endpoint (a source), named in Set Configuration */

#define CMD_LIMIT_MS 6000
#define CHAN_WAIT_MS 12000
#define CLOSE_LIMIT_MS 3000

/* ------------------------------------------------------------------------------------------------------------- the messages */

/* Discover answer: for each endpoint 2 bytes: ACP SEID (6 bits) | in use (1 bit) | 0 ; then 3 reserved bits | type (1 bit: 0 source, 1 sink) |
 * media type (4 bits: 0 = audio) in the high nibble. Returns how many, or -1 if the length is odd. */
int bt_avdtp_parse_discover(const unsigned char *p, unsigned n, BtAvSep *out, unsigned max)
{
    if (n & 1)
        return -1;
    unsigned k = 0;
    for (unsigned i = 0; i + 1 < n && k < max; i += 2)
    {
        out[k].seid = (unsigned char)(p[i] >> 2);
        out[k].in_use = (p[i] >> 1) & 1;
        out[k].media_type = p[i + 1] >> 4;
        out[k].sink = (p[i + 1] >> 3) & 1;
        k++;
    }
    return (int)k;
}

/* Get Capabilities answer: a list of (category, length, data). The Media Codec capability of an SBC endpoint is 6 bytes: media type in the
 * high nibble, codec type 0 = SBC, then the 4 bytes of SBC information:
 *   byte 0: sampling frequency in the high nibble (1 = 48 kHz, 2 = 44.1, 4 = 32, 8 = 16), channel mode in the low (1 joint stereo, 2 stereo, 4 dual, 8 mono)
 *   byte 1: block length in the high nibble (1 = 16, 2 = 12, 4 = 8, 8 = 4), subbands in bits 3..2 (1 = 8, 2 = 4), allocation in bits 1..0 (1 loudness, 2 SNR)
 *   byte 2, 3: smallest and largest bitpool (how many bits per sample: quality and bit rate).
 * Returns the number of capabilities read, or -1 if the list is damaged. */
int bt_avdtp_parse_caps(const unsigned char *p, unsigned n, BtSbcCaps *out)
{
    unsigned char *z = (unsigned char *)out;
    for (unsigned i = 0; i < sizeof *out; i++)
        z[i] = 0;
    int count = 0;
    while (n)
    {
        if (n < 2 || 2u + p[1] > n)
            return -1;
        unsigned cat = p[0], len = p[1];
        const unsigned char *d = p + 2;
        if (cat == CAP_MEDIA_TRANSPORT)
            out->has_transport = 1;
        else if (cat == CAP_DELAY_REPORTING)
            out->has_delay = 1;
        else if (cat == CAP_MEDIA_CODEC && len == 6 && (d[0] >> 4) == 0 && d[1] == 0) /* audio, SBC */
        {
            out->has_sbc = 1;
            out->freq = d[2] >> 4;
            out->mode = d[2] & 15;
            out->blocks = d[3] >> 4;
            out->bands = (d[3] >> 2) & 3;
            out->alloc = d[3] & 3;
            out->min_bitpool = d[4];
            out->max_bitpool = d[5];
        }
        p += 2 + len;
        n -= 2 + len;
        count++;
    }
    return count;
}

/* Our choice out of what the sink allows: the best quality our encoder makes. 48 kHz first (the rate of everything else in this system),
 * joint stereo, 16 blocks, 8 subbands, loudness allocation, and as high a bitpool as is usual (51 at 48 kHz, 53 at 44.1: 328 kbit/s) but not above
 * the sink's limit. Returns 0, or a negative number that says what could not be matched. */
int bt_avdtp_choose(const BtSbcCaps *c, BtSbcConfig *cfg)
{
    if (!c->has_sbc)
        return BT_AV_NO_SBC;
    if (c->freq & 1)
        cfg->freq = 1;
    else if (c->freq & 2)
        cfg->freq = 2;
    else
        return BT_AV_NO_FREQ;
    if (c->mode & 1)
        cfg->mode = 1;
    else if (c->mode & 2)
        cfg->mode = 2;
    else
        return BT_AV_NO_MODE;
    if (c->blocks & 1)
        cfg->blocks = 1;
    else if (c->blocks & 2)
        cfg->blocks = 2;
    else if (c->blocks & 4)
        cfg->blocks = 4;
    else if (c->blocks & 8)
        cfg->blocks = 8;
    else
        return BT_AV_NO_BLOCKS;
    if (c->bands & 1)
        cfg->bands = 1;
    else if (c->bands & 2)
        cfg->bands = 2;
    else
        return BT_AV_NO_BANDS;
    if (c->alloc & 1)
        cfg->alloc = 1;
    else if (c->alloc & 2)
        cfg->alloc = 2;
    else
        return BT_AV_NO_ALLOC;
    if (c->min_bitpool > c->max_bitpool)
        return BT_AV_NO_BITPOOL;
    unsigned want = cfg->freq == 1 ? 51 : 53;
    if (want > c->max_bitpool)
        want = c->max_bitpool;
    if (want < c->min_bitpool)
        return BT_AV_NO_BITPOOL;
    cfg->bitpool = want;
    cfg->min_bitpool = c->min_bitpool;
    return 0;
}

/* The parameters of Set Configuration: the sink's endpoint, ours, then Media Transport (nothing in it) and the Media Codec capability with
 * exactly one choice in every field. Returns the length, or 0 if `cap` is too small. */
unsigned bt_avdtp_set_config(unsigned char *out, unsigned cap, unsigned acp_seid, unsigned int_seid, const BtSbcConfig *cfg)
{
    if (cap < 12)
        return 0;
    unsigned n = 0;
    out[n++] = (unsigned char)(acp_seid << 2);
    out[n++] = (unsigned char)(int_seid << 2);
    out[n++] = CAP_MEDIA_TRANSPORT;
    out[n++] = 0;
    out[n++] = CAP_MEDIA_CODEC;
    out[n++] = 6;
    out[n++] = 0x00; /* audio */
    out[n++] = 0x00; /* SBC */
    out[n++] = (unsigned char)((cfg->freq << 4) | cfg->mode);
    out[n++] = (unsigned char)((cfg->blocks << 4) | (cfg->bands << 2) | cfg->alloc);
    out[n++] = (unsigned char)cfg->min_bitpool;
    out[n++] = (unsigned char)cfg->bitpool;
    return n;
}

/* ------------------------------------------------------------------------------------------------------------------ the session */

enum
{
    AV_IDLE,
    AV_SIGCHAN,    /* opening the signalling channel */
    AV_DISCOVER,
    AV_CAPS,
    AV_SETCONF,
    AV_OPENCMD,
    AV_MEDIACHAN,  /* opening the media channel */
    AV_STARTING,
    AV_STREAMING,
    AV_READY,      /* "keep" mode: the stream is open and its media channel too, but the sink was not told to expect audio (Start) */
    AV_SUSPENDING, /* Suspend sent: audio stops, the stream stays open */
    AV_CLOSING,
    AV_CLOSED,
    AV_FAILED
};

#define MAX_SEPS 8

static struct
{
    int state;
    const char *why;
    int code;
    int sig, media;            /* channel slots (bt_link.c), -1 = none */
    unsigned char label;
    int pending;               /* a command of ours waits for its answer */
    unsigned pend_sig;
    unsigned long long t0;
    BtAvSep seps[MAX_SEPS];
    int nseps;
    unsigned acp_seid;
    int have_caps;
    BtSbcCaps caps;
    int configured;
    BtSbcConfig cfg;
    int tried_all;             /* Get All Capabilities was used after Get Capabilities was refused */
    int keep;                  /* stay ready after Open instead of starting at once: the stream is started when audio comes, suspended when it stops */
} A;

static const char *sig_name(unsigned sig)
{
    switch (sig)
    {
    case SIG_DISCOVER: return "Discover";
    case SIG_GET_CAPABILITIES: return "Get Capabilities";
    case SIG_GET_ALL_CAPABILITIES: return "Get All Capabilities";
    case SIG_SET_CONFIGURATION: return "Set Configuration";
    case SIG_OPEN: return "Open";
    case SIG_START: return "Start";
    case SIG_CLOSE: return "Close";
    case SIG_SUSPEND: return "Suspend";
    default: return "a command";
    }
}

static int active(void) { return A.state >= AV_SIGCHAN && A.state <= AV_CLOSING; }

static void fail(const char *why, int code)
{
    A.state = AV_FAILED;
    A.why = why;
    A.code = code;
    A.pending = 0;
    if (A.media >= 0)
        bt_chan_close(A.media);
    if (A.sig >= 0)
        bt_chan_close(A.sig);
    A.media = A.sig = -1;
}

/* A command on the signalling channel. */
static void command(unsigned sig, const unsigned char *par, unsigned n)
{
    unsigned char pdu[24];
    A.label = (unsigned char)((A.label + 1) & 15);
    pdu[0] = (unsigned char)((A.label << 4) | (0 << 2) | MSG_COMMAND);
    pdu[1] = (unsigned char)sig;
    for (unsigned i = 0; i < n && i < sizeof pdu - 2; i++)
        pdu[2 + i] = par[i];
    if (bt_chan_queue(A.sig, pdu, 2 + n) < 0)
    {
        fail("could not queue a command", 0);
        return;
    }
    A.pending = 1;
    A.pend_sig = sig;
    A.t0 = bt_now();
}

static void send_seid_command(unsigned sig)
{
    unsigned char s = (unsigned char)(A.acp_seid << 2);
    command(sig, &s, 1);
}

static void accepted(unsigned sig, const unsigned char *p, unsigned n)
{
    switch (sig)
    {
    case SIG_DISCOVER:
    {
        A.nseps = bt_avdtp_parse_discover(p, n, A.seps, MAX_SEPS);
        if (A.nseps < 0)
        {
            A.nseps = 0;
            fail("the Discover answer is damaged", 0);
            return;
        }
        for (int i = 0; i < A.nseps; i++)
            if (A.seps[i].media_type == 0 && A.seps[i].sink && !A.seps[i].in_use)
            {
                A.acp_seid = A.seps[i].seid;
                A.state = AV_CAPS;
                send_seid_command(SIG_GET_CAPABILITIES);
                return;
            }
        fail("the device has no free audio sink endpoint", 0);
        return;
    }
    case SIG_GET_CAPABILITIES:
    case SIG_GET_ALL_CAPABILITIES:
    {
        if (bt_avdtp_parse_caps(p, n, &A.caps) < 0)
        {
            fail("the capabilities are damaged", 0);
            return;
        }
        A.have_caps = 1;
        int r = bt_avdtp_choose(&A.caps, &A.cfg);
        if (r != 0)
        {
            fail(r == BT_AV_NO_SBC ? "the endpoint has no SBC codec"
                 : r == BT_AV_NO_FREQ ? "no sampling rate in common (48 or 44.1 kHz)"
                 : r == BT_AV_NO_MODE ? "no channel mode in common (joint stereo or stereo)"
                 : r == BT_AV_NO_BITPOOL ? "no usable bitpool"
                                         : "no SBC setting in common",
                 0);
            return;
        }
        unsigned char par[16];
        unsigned k = bt_avdtp_set_config(par, sizeof par, A.acp_seid, OUR_SEID, &A.cfg);
        A.state = AV_SETCONF;
        command(SIG_SET_CONFIGURATION, par, k);
        return;
    }
    case SIG_SET_CONFIGURATION:
        A.configured = 1;
        A.state = AV_OPENCMD;
        send_seid_command(SIG_OPEN);
        return;
    case SIG_OPEN:
        A.media = bt_chan_open(AVDTP_PSM, BT_USER_AVDTP_MEDIA);
        if (A.media < 0)
        {
            fail("no channel left for the media transport", 0);
            return;
        }
        A.state = AV_MEDIACHAN;
        A.t0 = bt_now();
        return;
    case SIG_START:
        A.state = AV_STREAMING;
        return;
    case SIG_SUSPEND:
        A.state = AV_READY;
        return;
    case SIG_CLOSE:
        if (A.media >= 0)
            bt_chan_close(A.media);
        if (A.sig >= 0)
            bt_chan_close(A.sig);
        A.media = A.sig = -1;
        A.state = AV_CLOSED;
        return;
    default:
        return;
    }
}

void bt_avdtp_rx(int user, const unsigned char *d, unsigned n)
{
    if (user != BT_USER_AVDTP_SIG || n < 2) /* the media channel carries nothing we need to read */
        return;
    unsigned label = d[0] >> 4, ptype = (d[0] >> 2) & 3, mtype = d[0] & 3, sig = d[1] & 0x3F;
    const unsigned char *p = d + 2;
    unsigned pn = n - 2;
    if (mtype == MSG_COMMAND)
    {
        /* the device asks us something (we are only a source that never serves requests): general reject, as the specification says for
         * a command that is not understood */
        unsigned char rej[2] = {(unsigned char)((label << 4) | (0 << 2) | MSG_GEN_REJECT), (unsigned char)sig};
        if (A.sig >= 0)
            bt_chan_queue(A.sig, rej, 2);
        return;
    }
    if (!A.pending || label != A.label || sig != A.pend_sig)
        return;
    if (ptype != 0) /* an answer in several pieces: nothing we ask for is that long */
    {
        fail("the answer comes in pieces (not supported)", 0);
        return;
    }
    A.pending = 0;
    if (mtype == MSG_ACCEPT)
        accepted(sig, p, pn);
    else if (mtype == MSG_GEN_REJECT)
        fail("the device does not know this command", (int)sig);
    else /* reject: the last parameter is the error code (Set Configuration puts the service category before it) */
    {
        int code = pn ? p[pn - 1] : 0;
        if (sig == SIG_GET_CAPABILITIES && !A.tried_all) /* the other form of the question, for devices that want it */
        {
            A.tried_all = 1;
            send_seid_command(SIG_GET_ALL_CAPABILITIES);
            return;
        }
        static char why[48];
        const char *nm = sig_name(sig);
        unsigned k = 0;
        for (; nm[k] && k < 30; k++)
            why[k] = nm[k];
        static const char tail[] = " was refused";
        for (unsigned i = 0; i < sizeof tail; i++)
            why[k++] = tail[i];
        fail(why, code);
    }
}

static int start_common(int keep)
{
    if (bt_link_state() != BT_LINK_UP)
        return BT_E_NOT_UP;
    if (active())
        return BT_E_LINK_BUSY;
    A.why = 0;
    A.code = 0;
    A.nseps = 0;
    A.have_caps = A.configured = 0;
    A.tried_all = 0;
    A.pending = 0;
    A.media = -1;
    A.sig = bt_chan_open(AVDTP_PSM, BT_USER_AVDTP_SIG);
    if (A.sig < 0)
        return BT_E_LINK_BUSY;
    A.keep = keep;
    A.state = AV_SIGCHAN;
    A.t0 = bt_now();
    return 0;
}

int bt_avdtp_start(void) { return start_common(0); }
int bt_avdtp_start_keep(void) { return start_common(1); }

int bt_avdtp_stop(void)
{
    bt_a2dp_stop_audio(); /* the sound stops before the stream is closed */
    if (A.state == AV_STREAMING || A.state == AV_STARTING || A.state == AV_READY || A.state == AV_SUSPENDING)
    {
        A.state = AV_CLOSING;
        send_seid_command(SIG_CLOSE);
        A.t0 = bt_now();
    }
    else if (active())
    {
        A.state = AV_CLOSED;
        if (A.media >= 0)
            bt_chan_close(A.media);
        if (A.sig >= 0)
            bt_chan_close(A.sig);
        A.media = A.sig = -1;
        A.pending = 0;
    }
    return 0;
}

void bt_avdtp_reset(void)
{
    if (active())
        fail("link ended", 0);
    if (A.state == AV_IDLE)
    {
        A.sig = A.media = -1;
    }
}

void bt_avdtp_step(unsigned long long now)
{
    if (!active())
        return;
    if (A.state == AV_SIGCHAN)
    {
        int st = bt_chan_state(A.sig);
        if (st == BT_CHAN_OPEN)
        {
            A.state = AV_DISCOVER;
            command(SIG_DISCOVER, 0, 0);
        }
        else if (st == BT_CHAN_FREE)
            fail("the signalling channel was refused", bt_chan_result(A.sig));
        else if (now && now - A.t0 > CHAN_WAIT_MS)
            fail("the signalling channel did not open", 0);
        return;
    }
    if (A.state == AV_CLOSING)
    {
        if (now && now - A.t0 > CLOSE_LIMIT_MS) /* no answer to Close: end it anyway */
        {
            if (A.media >= 0)
                bt_chan_close(A.media);
            if (A.sig >= 0)
                bt_chan_close(A.sig);
            A.media = A.sig = -1;
            A.pending = 0;
            A.state = AV_CLOSED;
        }
        return;
    }
    if (A.sig >= 0 && bt_chan_state(A.sig) != BT_CHAN_OPEN)
    {
        fail("the device closed the signalling channel", 0);
        return;
    }
    if (A.state == AV_MEDIACHAN)
    {
        int st = bt_chan_state(A.media);
        if (st == BT_CHAN_OPEN)
        {
            if (A.keep)
                A.state = AV_READY; /* open and waiting: bt_avdtp_stream_start() tells the sink when audio comes */
            else
            {
                A.state = AV_STARTING; /* the stream is open and its channel is there: tell the sink that audio follows */
                send_seid_command(SIG_START);
            }
        }
        else if (st == BT_CHAN_FREE)
            fail("the media channel was refused", bt_chan_result(A.media));
        else if (now && now - A.t0 > CHAN_WAIT_MS)
            fail("the media channel did not open", 0);
        return;
    }
    if ((A.state == AV_STREAMING || A.state == AV_READY || A.state == AV_SUSPENDING) && A.media >= 0 && bt_chan_state(A.media) != BT_CHAN_OPEN)
    {
        fail("the device closed the media channel", 0);
        return;
    }
    if (A.pending && now && now - A.t0 > CMD_LIMIT_MS)
    {
        static char why[48];
        const char *nm = sig_name(A.pend_sig);
        unsigned k = 0;
        for (; nm[k] && k < 28; k++)
            why[k] = nm[k];
        static const char tail[] = " got no answer";
        for (unsigned i = 0; i < sizeof tail; i++)
            why[k++] = tail[i];
        fail(why, 0);
    }
}

/* ------------------------------------------------------------------------------------------------------------------------ text */

typedef struct
{
    char *p;
    unsigned cap, n;
    int cut;
} Out;

static void putc_(Out *o, char c)
{
    if (o->n + 1 < o->cap)
        o->p[o->n++] = c;
    else
        o->cut = 1;
}

static void puts_(Out *o, const char *s)
{
    while (*s)
        putc_(o, *s++);
}

static void putx(Out *o, unsigned v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        putc_(o, "0123456789abcdef"[(v >> (4 * i)) & 15]);
}

static void putd(Out *o, int v)
{
    char t[11];
    int k = 0;
    unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0)
        putc_(o, '-');
    do
    {
        t[k++] = (char)('0' + u % 10);
        u /= 10;
    } while (u && k < 10);
    while (k)
        putc_(o, t[--k]);
}

/* a list of the entries of `names` whose bit is set in `mask`, separated by "/" */
static void put_set(Out *o, unsigned mask, const unsigned *bits, const char *const *names, int n)
{
    int first = 1;
    for (int i = 0; i < n; i++)
        if (mask & bits[i])
        {
            if (!first)
                putc_(o, '/');
            puts_(o, names[i]);
            first = 0;
        }
    if (first)
        puts_(o, "-");
}

int bt_avdtp_line(unsigned index, char *buf, unsigned cap)
{
    if (cap < 2 || index > 2)
        return -1;
    Out o = {buf, cap, 0, 0};
    if (index == 0)
    {
        puts_(&o, "a2dp: ");
        switch (A.state)
        {
        case AV_IDLE: puts_(&o, "-"); break;
        case AV_SIGCHAN: puts_(&o, "opening the signalling channel"); break;
        case AV_DISCOVER: puts_(&o, "asking for the stream endpoints"); break;
        case AV_CAPS: puts_(&o, "reading what the endpoint can do"); break;
        case AV_SETCONF: puts_(&o, "setting the codec"); break;
        case AV_OPENCMD: puts_(&o, "opening the stream"); break;
        case AV_MEDIACHAN: puts_(&o, "opening the media channel"); break;
        case AV_STARTING: puts_(&o, "starting the stream"); break;
        case AV_STREAMING: puts_(&o, "streaming (the sink accepted Start)"); break;
        case AV_READY: puts_(&o, "ready (stream open, no audio yet)"); break;
        case AV_SUSPENDING: puts_(&o, "suspending the stream"); break;
        case AV_CLOSING: puts_(&o, "closing the stream"); break;
        case AV_CLOSED: puts_(&o, "closed"); break;
        default:
            puts_(&o, "failed: ");
            puts_(&o, A.why ? A.why : "?");
            if (A.code)
            {
                puts_(&o, " (");
                if (A.code < 0)
                    putd(&o, A.code);
                else
                {
                    puts_(&o, "0x");
                    putx(&o, (unsigned)A.code, 2);
                }
                puts_(&o, ")");
            }
        }
    }
    else if (index == 1)
    {
        static const unsigned fb[4] = {1, 2, 4, 8}, mb[4] = {1, 2, 4, 8};
        static const char *const fn[4] = {"48", "44.1", "32", "16"}, *const mn[4] = {"joint", "stereo", "dual", "mono"};
        if (!A.have_caps)
            puts_(&o, "a2dp: sink capabilities -");
        else
        {
            puts_(&o, "a2dp: sink endpoint ");
            putd(&o, (int)A.acp_seid);
            puts_(&o, " offers SBC ");
            put_set(&o, A.caps.freq, fb, fn, 4);
            puts_(&o, " kHz ");
            put_set(&o, A.caps.mode, mb, mn, 4);
            puts_(&o, ", bitpool ");
            putd(&o, (int)A.caps.min_bitpool);
            putc_(&o, '-');
            putd(&o, (int)A.caps.max_bitpool);
        }
    }
    else
    {
        if (!A.configured)
            puts_(&o, "a2dp: configuration -");
        else
        {
            puts_(&o, "a2dp: configured ");
            puts_(&o, A.cfg.freq == 1 ? "48" : "44.1");
            puts_(&o, " kHz ");
            puts_(&o, A.cfg.mode == 1 ? "joint stereo" : "stereo");
            puts_(&o, ", ");
            putd(&o, A.cfg.blocks == 1 ? 16 : A.cfg.blocks == 2 ? 12 : A.cfg.blocks == 4 ? 8 : 4);
            puts_(&o, " blocks, ");
            putd(&o, A.cfg.bands == 1 ? 8 : 4);
            puts_(&o, " subbands, ");
            puts_(&o, A.cfg.alloc == 1 ? "loudness" : "SNR");
            puts_(&o, ", bitpool ");
            putd(&o, (int)A.cfg.bitpool);
        }
    }
    buf[o.n] = 0;
    if (o.cut)
        return -1;
    return (int)o.n;
}

int bt_avdtp_streaming(void) { return A.state == AV_STREAMING; }

/* the stream is open and its media channel usable (started or not) */
int bt_avdtp_ready(void) { return (A.state == AV_READY || A.state == AV_STARTING || A.state == AV_STREAMING || A.state == AV_SUSPENDING) && A.media >= 0; }

/* Tell the sink that audio comes (Start). 1 = the command went out, 0 = not now (not ready, or another command is waiting for its answer) */
int bt_avdtp_stream_start(void)
{
    if (A.state != AV_READY || A.pending)
        return 0;
    A.state = AV_STARTING;
    send_seid_command(SIG_START);
    return A.state == AV_STARTING;
}

/* Audio stopped: Suspend (the stream stays open, the sink may go quiet or save power). */
int bt_avdtp_stream_suspend(void)
{
    if (A.state != AV_STREAMING || A.pending)
        return 0;
    A.state = AV_SUSPENDING;
    send_seid_command(SIG_SUSPEND);
    return A.state == AV_SUSPENDING;
}

int bt_avdtp_media(int *slot, BtSbcConfig *cfg)
{
    if (!bt_avdtp_ready())
        return 0;
    if (slot)
        *slot = A.media;
    if (cfg)
        *cfg = A.cfg;
    return 1;
}
