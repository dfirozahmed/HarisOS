/* A2DP media sender: turns audio into SBC frames, puts them into RTP packets and sends them on the media channel at the speed of the music.
 * See bt_avdtp.c for how the stream is set up and started.
 *
 * A media packet is one L2CAP PDU on the media channel: a 12 byte RTP header (RFC 3550: version 2, payload type 96 = "dynamic", a sequence
 * number counting packets, a time stamp counting samples, a fixed source id), one byte SBC payload header (the number of frames, 1 .. 15, in
 * the low four bits) and that many SBC frames. As many frames as the channel's MTU takes go into a packet (BlueZ and PulseAudio do the same).
 *
 * The sink plays at its own clock, so the packets must arrive at the rate of the music: not much faster (its buffer would overflow) nor
 * slower (it would run dry). The sender keeps about LEAD_MS of audio sent ahead of real time, measured with the system clock.
 *
 * Two sources: a test tune made here (bt_a2dp_tone, needs a started stream) and the audio of programs. That comes through a ring buffer: the
 * program's side (a system call, bt_a2dp_ring_write) only puts samples in; this file's bt_a2dp_step, called from the main loop, is the only
 * code that touches the Bluetooth state: it starts the stream when samples arrive, sends the packets, and suspends the stream when the
 * samples stop for a while. So the two sides share nothing but the ring and a few flags. */
#include "bluetooth.h"
#include "bt_sbc.h"

#define LEAD_MS 60        /* how far ahead of real time the packets are sent */
#define BEHIND_MS 250     /* more than this behind: the audio is cut (counted as an underrun) and the clock starts again */
#define IDLE_MS 3000      /* no audio for this long: the stream is suspended */
#define PACKETS_PER_STEP 4
#define RTP_BYTES 12
#define PAYLOAD_TYPE 96
#define SSRC 0x48415249u  /* "HARI": any number will do, it only has to stay the same */
#define RING_FRAMES 16384 /* 341 ms at 48 kHz; a power of two */
#define MIN_FRAMES 32     /* the smallest SBC frame (4 blocks of 8 samples) */

/* one period of a sine wave, 256 points, amplitude 32767 */
static const short SINE[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804,
};

enum { SRC_NONE, SRC_TONE, SRC_RING };

static struct
{
    int active;          /* the test tune plays */
    int source;          /* the source of the packets sent last: SRC_* */
    BtSbcEnc enc;
    int enc_ok;          /* enc, spf, fb, rate, nf, slot belong to the stream that is open now */
    unsigned spf, fb;    /* samples per frame, bytes per frame */
    unsigned rate;       /* Hz */
    int slot;            /* the media channel */
    unsigned mtu;
    unsigned nf;         /* frames in a packet */
    unsigned long long t0;       /* the clock of the current burst of packets */
    unsigned long long sent;     /* sample frames sent since t0 */
    unsigned long long total;    /* the length of the tune in sample frames */
    unsigned seq, ts;
    unsigned packets, frames, underruns, errors;
    int state;           /* the tune: 0 nothing yet, 1 playing, 2 finished, 3 failed */
    const char *why;
    int was_ready;       /* the stream was open at the last step (to see a new one) */
    int dry;             /* the ring had nothing to send at the last step */
    unsigned long long idle_since;
    /* the tune */
    unsigned phase, note;
    unsigned long long note_pos;
    unsigned char pkt[RTP_BYTES + 1 + 15 * BT_SBC_MAX_FRAME];
} P;

/* the ring of samples (left, right) from programs */
static short RING[RING_FRAMES * 2];
static volatile unsigned RD, WR;      /* free running counters of sample frames: RD is moved by the sender, WR by the program */
static volatile int PROD_BUSY;        /* a program is putting samples in */
static volatile int FLUSH_REQ;        /* drop what is in the ring (the program was stopped) */
static volatile int VOLUME_Q8 = 256;  /* software volume: 256 = unchanged */

/* three notes (C5, E5, G5 in hundredths of Hz) of 300 ms each, over and over, with a short fade at the ends of a note */
static const unsigned NOTES[3] = {52325, 65926, 78399};

static int sine_at(unsigned phase)
{
    unsigned i = phase >> 24, f = (phase >> 8) & 0xFFFF;
    int a = SINE[i], b = SINE[(i + 1) & 255];
    return a + (int)(((long long)(b - a) * (long long)f) >> 16);
}

/* the next `n` sample frames of the tune (left = right) into pcm */
static void tune(short *pcm, unsigned n)
{
    unsigned long long note_len = (unsigned long long)P.rate * 300 / 1000;
    unsigned fade = P.rate / 200; /* 5 ms */
    for (unsigned i = 0; i < n; i++)
    {
        unsigned inc = (unsigned)((unsigned long long)NOTES[P.note] * 4294967296ULL / ((unsigned long long)P.rate * 100));
        int s = sine_at(P.phase);
        P.phase += inc;
        unsigned long long left = note_len - P.note_pos;
        unsigned env = (unsigned)(P.note_pos < fade ? P.note_pos : left < fade ? left : fade);
        int v = (int)(((long long)s * 8192 * env) / ((long long)32767 * fade)); /* a quarter of full scale */
        pcm[2 * i] = pcm[2 * i + 1] = (short)v;
        if (++P.note_pos >= note_len)
        {
            P.note_pos = 0;
            P.note = (P.note + 1) % 3;
            P.phase = 0;
        }
    }
}

/* the next `n` sample frames from the ring, with the volume applied */
static void from_ring(short *pcm, unsigned n)
{
    unsigned rd = RD;
    int vol = VOLUME_Q8;
    for (unsigned i = 0; i < n; i++)
    {
        unsigned at = ((rd + i) & (RING_FRAMES - 1)) * 2;
        int l = RING[at], r = RING[at + 1];
        if (vol != 256)
        {
            l = (l * vol) >> 8;
            r = (r * vol) >> 8;
        }
        pcm[2 * i] = (short)l;
        pcm[2 * i + 1] = (short)r;
    }
    RD = rd + n;
}

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned rate_of(const BtSbcConfig *cfg) { return cfg->freq == 1 ? 48000 : cfg->freq == 2 ? 44100 : cfg->freq == 4 ? 32000 : 16000; }

/* Gets the encoder and the packet size ready for the stream that is open now. 0 = not possible. */
static int ensure_encoder(void)
{
    if (P.enc_ok)
        return 1;
    int slot;
    BtSbcConfig cfg;
    if (!bt_avdtp_media(&slot, &cfg) || bt_sbc_init(&P.enc, &cfg) != 0)
        return 0;
    P.spf = bt_sbc_samples(&P.enc);
    P.fb = bt_sbc_frame_bytes(&P.enc);
    P.rate = rate_of(&cfg);
    P.slot = slot;
    P.mtu = bt_chan_mtu(slot);
    if (P.mtu < RTP_BYTES + 1 + P.fb)
        return 0;
    P.nf = (P.mtu - RTP_BYTES - 1) / P.fb;
    if (P.nf > 15)
        P.nf = 15;
    P.enc_ok = 1;
    /* a new stream: the numbers and the counts start again */
    P.seq = P.ts = 0;
    P.packets = P.frames = P.underruns = P.errors = 0;
    P.sent = 0;
    P.t0 = bt_now();
    return 1;
}

int bt_a2dp_tone(unsigned seconds)
{
    if (!bt_avdtp_streaming())
        return BT_E_NOT_UP;
    if (P.active || PROD_BUSY)
        return BT_E_LINK_BUSY;
    if (seconds < 1 || seconds > 60)
        return -1;
    if (bt_now() == 0) /* without a clock the packets cannot be paced */
        return BT_E_NOT_UP;
    P.enc_ok = 0;
    if (!ensure_encoder())
        return -1;
    P.t0 = bt_now();
    P.sent = 0;
    P.total = (unsigned long long)seconds * P.rate;
    P.seq = 0;
    P.ts = 0;
    P.packets = P.frames = P.underruns = P.errors = 0;
    P.phase = P.note = 0;
    P.note_pos = 0;
    P.source = SRC_TONE;
    P.why = 0;
    P.state = 1;
    P.active = 1;
    return 0;
}

void bt_a2dp_stop_audio(void)
{
    if (P.active)
    {
        P.active = 0;
        P.state = 2;
    }
    FLUSH_REQ = 1;
}

void bt_a2dp_reset(void)
{
    if (P.active)
    {
        P.active = 0;
        P.state = 3;
        P.why = "the stream ended";
    }
    P.enc_ok = 0;
    P.dry = 1;
    P.idle_since = 0;
    RD = WR; /* the main loop is the one that moves RD: nothing is left to play it to */
    FLUSH_REQ = 1;
}

/* ---- the ring: the program's side ---- */

/* 48 kHz stereo 16-bit audio can go to the stream: it is open and was agreed at 48 kHz (the rate of everything else in the system). */
int bt_a2dp_audio_ready(void)
{
    int slot;
    BtSbcConfig cfg;
    BtSbcEnc probe;
    return bt_avdtp_ready() && bt_avdtp_media(&slot, &cfg) && cfg.freq == 1 && bt_sbc_init(&probe, &cfg) == 0;
}

int bt_a2dp_producer_begin(void)
{
    if (PROD_BUSY || P.active)
        return BT_E_LINK_BUSY;
    FLUSH_REQ = 0;
    PROD_BUSY = 1;
    return 0;
}

void bt_a2dp_producer_end(void) { PROD_BUSY = 0; }

unsigned bt_a2dp_ring_pending(void) { return WR - RD; }

/* Puts up to `frames` sample frames (left, right) into the ring; returns how many fitted. Never blocks. */
unsigned bt_a2dp_ring_write(const short *pcm, unsigned frames)
{
    unsigned wr = WR, free_frames = RING_FRAMES - (wr - RD);
    unsigned n = frames < free_frames ? frames : free_frames;
    for (unsigned i = 0; i < n; i++)
    {
        unsigned at = ((wr + i) & (RING_FRAMES - 1)) * 2;
        RING[at] = pcm[2 * i];
        RING[at + 1] = pcm[2 * i + 1];
    }
    __atomic_thread_fence(__ATOMIC_RELEASE); /* the samples are in before the sender can see them */
    WR = wr + n;
    return n;
}

/* the program was stopped: what is in the ring is dropped (by the sender, which owns RD) */
void bt_a2dp_flush(void) { FLUSH_REQ = 1; }

int bt_a2dp_volume(void) { return (VOLUME_Q8 * 100 + 128) / 256; }

int bt_a2dp_set_volume(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    VOLUME_Q8 = (percent * 256 + 50) / 100;
    return bt_a2dp_volume();
}

/* ---- the sender ---- */

/* How many whole SBC frames the source can give now. */
static unsigned available(int src)
{
    if (src == SRC_TONE)
        return (unsigned)((P.total - P.sent) / P.spf);
    return (WR - RD) / P.spf;
}

/* One packet: as many frames as fit and the source has, encoded, in RTP. 1 = sent, 0 = nothing to send, -1 = failed. */
static int send_packet(int src)
{
    short pcm[2 * 128];
    unsigned char *p = P.pkt;
    unsigned nf = P.nf, have = available(src);
    if (nf > have)
        nf = have;
    if (nf == 0)
        return 0;
    p[0] = 0x80;
    p[1] = PAYLOAD_TYPE;
    p[2] = (unsigned char)(P.seq >> 8);
    p[3] = (unsigned char)P.seq;
    put32(p + 4, P.ts);
    put32(p + 8, SSRC);
    p[RTP_BYTES] = (unsigned char)nf; /* not fragmented: the frame count alone */
    unsigned n = RTP_BYTES + 1;
    unsigned rd0 = RD;
    for (unsigned k = 0; k < nf; k++)
    {
        if (src == SRC_TONE)
            tune(pcm, P.spf);
        else
            from_ring(pcm, P.spf);
        if (bt_sbc_encode(&P.enc, pcm, p + n, P.fb) != (int)P.fb)
            return -1;
        n += P.fb;
    }
    if (bt_chan_send(P.slot, p, n) < 0)
    {
        if (src == SRC_RING)
            RD = rd0; /* the samples stay: the next step tries again */
        return -1;
    }
    P.seq = (P.seq + 1) & 0xFFFF;
    P.ts += nf * P.spf;
    P.sent += (unsigned long long)nf * P.spf;
    P.packets++;
    P.frames += nf;
    return 1;
}

/* Sends the packets that are due. 1 = the source is used up (the tune ended), 0 = go on, -1 = given up. */
static int pace(unsigned long long now, int src)
{
    unsigned long long played_ms = now - P.t0;
    unsigned long long sent_ms = P.sent * 1000 / P.rate;
    if (played_ms > sent_ms + BEHIND_MS) /* the system was busy for too long: do not catch up with a burst, start the clock again */
    {
        P.underruns++;
        P.t0 = now - sent_ms;
        played_ms = sent_ms;
    }
    for (int budget = 0; budget < PACKETS_PER_STEP; budget++)
    {
        if (src == SRC_TONE && P.sent >= P.total)
            break;
        sent_ms = P.sent * 1000 / P.rate;
        if (sent_ms >= played_ms + LEAD_MS)
            break;
        int r = send_packet(src);
        if (r < 0)
        {
            if (++P.errors > 20)
                return -1;
            return 0;
        }
        if (r == 0)
            break;
    }
    return src == SRC_TONE && (P.sent >= P.total || P.total - P.sent < P.spf);
}

/* audio of programs: the stream is started when samples come, packets go out as the clock asks, and the stream is suspended when it is quiet */
static void ring_step(unsigned long long now)
{
    if (!bt_avdtp_ready())
    {
        if (RD != WR)
            RD = WR; /* nobody to play it to */
        P.enc_ok = 0;
        P.dry = 1;
        P.was_ready = 0;
        return;
    }
    if (!P.was_ready) /* a new stream: what the line said about the last tune does not belong to it */
    {
        P.was_ready = 1;
        if (P.source == SRC_TONE)
        {
            P.source = SRC_NONE;
            P.state = 0;
        }
    }
    if (FLUSH_REQ)
    {
        RD = WR;
        FLUSH_REQ = 0;
    }
    unsigned pending = WR - RD;
    int want = PROD_BUSY || pending >= MIN_FRAMES;
    if (!bt_avdtp_streaming())
    {
        if (want)
            bt_avdtp_stream_start(); /* READY -> STARTING; tried again at every step until the command goes out */
        P.dry = 1;
        return;
    }
    if (!now || !ensure_encoder())
        return;
    if (pending < P.spf) /* nothing to send (or less than a frame) */
    {
        if (!P.dry)
        {
            P.dry = 1;
            P.idle_since = now;
            if (PROD_BUSY)
                P.underruns++; /* the program is behind: the sink will run dry */
        }
        if (!PROD_BUSY && P.idle_since && now - P.idle_since > IDLE_MS)
            bt_avdtp_stream_suspend();
        return;
    }
    if (P.dry) /* the samples come again: a new burst, the lead is built up again */
    {
        P.dry = 0;
        P.idle_since = 0;
        P.t0 = now;
        P.sent = 0;
    }
    P.source = SRC_RING;
    if (pace(now, SRC_RING) < 0)
    {
        P.state = 3;
        P.why = "the packets are not taken";
        RD = WR;
    }
}

void bt_a2dp_step(unsigned long long now)
{
    if (P.active)
    {
        if (!bt_avdtp_streaming())
        {
            P.active = 0;
            P.state = 3;
            P.why = "the stream is not running";
            return;
        }
        if (!now)
            return;
        int r = pace(now, SRC_TONE);
        if (r < 0)
        {
            P.active = 0;
            P.state = 3;
            P.why = "the packets are not taken";
        }
        else if (r > 0)
        {
            P.active = 0;
            P.state = 2;
        }
        return;
    }
    ring_step(now);
}

/* ---- text ---- */

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

static void putd(Out *o, unsigned long long v)
{
    char t[21];
    int k = 0;
    do
    {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k < 20);
    while (k)
        putc_(o, t[--k]);
}

int bt_a2dp_line(unsigned index, char *buf, unsigned cap)
{
    if (cap < 2 || index > 0)
        return -1;
    Out o = {buf, cap, 0, 0};
    int tune_shown = P.state != 0 && P.source != SRC_RING;
    if (tune_shown)
    {
        switch (P.state)
        {
        case 1:
            puts_(&o, "audio: playing the test tune");
            break;
        case 2:
            puts_(&o, "audio: tune finished");
            break;
        default:
            puts_(&o, "audio: failed: ");
            puts_(&o, P.why ? P.why : "?");
        }
    }
    else if (P.state == 3 && P.source == SRC_RING)
    {
        puts_(&o, "audio: failed: ");
        puts_(&o, P.why ? P.why : "?");
    }
    else if (bt_avdtp_ready())
    {
        puts_(&o, "audio: Bluetooth sink, ");
        puts_(&o, bt_avdtp_streaming() ? (P.dry ? "stream started, no audio now" : "playing") : "waiting for audio (stream suspended)");
    }
    else
        puts_(&o, "audio: -");
    if (tune_shown || P.source == SRC_RING)
    {
        puts_(&o, ", ");
        putd(&o, P.packets);
        puts_(&o, " packets of ");
        putd(&o, P.nf);
        puts_(&o, " frames (");
        putd(&o, (unsigned long long)P.frames * P.spf * 1000 / (P.rate ? P.rate : 1));
        puts_(&o, " ms of sound), ");
        putd(&o, P.underruns);
        puts_(&o, " underruns, ");
        putd(&o, P.errors);
        puts_(&o, " send errors");
    }
    if (!tune_shown && (P.source == SRC_RING || bt_avdtp_ready()))
    {
        puts_(&o, ", volume ");
        putd(&o, (unsigned long long)bt_a2dp_volume());
        puts_(&o, "%");
    }
    buf[o.n] = 0;
    if (o.cut)
        return -1;
    return (int)o.n;
}
