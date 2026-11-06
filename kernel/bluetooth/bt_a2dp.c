/* A2DP media sender (stage 8): turns audio into SBC frames, puts them into RTP packets and sends them on the media channel at the speed
 * of the music. See bt_avdtp.c for how the stream was set up (the sink has accepted Start).
 *
 * A media packet is one L2CAP PDU on the media channel: a 12 byte RTP header (RFC 3550: version 2, payload type 96 = "dynamic", a sequence
 * number counting packets, a time stamp counting samples, a fixed source id), one byte SBC payload header (the number of frames, 1 .. 15, in
 * the low four bits) and that many SBC frames. As many frames as the channel's MTU takes go into a packet (BlueZ and PulseAudio do the same).
 *
 * The sink plays at its own clock, so the packets must arrive at the rate of the music: not much faster (its buffer would overflow) nor
 * slower (it would run dry). The sender keeps about LEAD_MS of audio sent ahead of real time, measured with the system clock. */
#include "bluetooth.h"
#include "bt_sbc.h"

#define LEAD_MS 60        /* how far ahead of real time the packets are sent */
#define BEHIND_MS 250     /* more than this behind: the audio is cut (counted as an underrun) and the clock starts again */
#define PACKETS_PER_STEP 4
#define RTP_BYTES 12
#define PAYLOAD_TYPE 96
#define SSRC 0x48415249u  /* "HARI": any number will do, it only has to stay the same */

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

static struct
{
    int active;          /* a source is playing */
    int source;          /* 1 = the test tune */
    BtSbcEnc enc;
    unsigned spf, fb;    /* samples per frame, bytes per frame */
    unsigned rate;       /* Hz */
    int slot;            /* the media channel */
    unsigned mtu;
    unsigned nf;         /* frames in a packet */
    unsigned long long t0;       /* when the first packet went out */
    unsigned long long sent;     /* sample frames sent since t0 */
    unsigned long long total;    /* the length of the tune in sample frames */
    unsigned seq, ts;
    unsigned packets, frames, underruns, errors;
    int state;           /* 0 nothing yet, 1 playing, 2 finished, 3 failed */
    const char *why;
    /* the tune */
    unsigned phase, note;
    unsigned long long note_pos;
    unsigned char pkt[RTP_BYTES + 1 + 15 * BT_SBC_MAX_FRAME];
} P;

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

static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

int bt_a2dp_tone(unsigned seconds)
{
    int slot;
    BtSbcConfig cfg;
    if (!bt_avdtp_media(&slot, &cfg))
        return BT_E_NOT_UP;
    if (P.active)
        return BT_E_LINK_BUSY;
    if (seconds < 1 || seconds > 60)
        return -1;
    if (bt_now() == 0) /* without a clock the packets cannot be paced */
        return BT_E_NOT_UP;
    if (bt_sbc_init(&P.enc, &cfg) != 0)
        return -1;
    P.spf = bt_sbc_samples(&P.enc);
    P.fb = bt_sbc_frame_bytes(&P.enc);
    P.rate = cfg.freq == 1 ? 48000 : cfg.freq == 2 ? 44100 : cfg.freq == 4 ? 32000 : 16000;
    P.slot = slot;
    P.mtu = bt_chan_mtu(slot);
    if (P.mtu < RTP_BYTES + 1 + P.fb)
        return -1;
    P.nf = (P.mtu - RTP_BYTES - 1) / P.fb;
    if (P.nf > 15)
        P.nf = 15;
    P.t0 = bt_now();
    P.sent = 0;
    P.total = (unsigned long long)seconds * P.rate;
    P.seq = 0;
    P.ts = 0;
    P.packets = P.frames = P.underruns = P.errors = 0;
    P.phase = P.note = 0;
    P.note_pos = 0;
    P.source = 1;
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
}

void bt_a2dp_reset(void)
{
    if (P.active)
    {
        P.active = 0;
        P.state = 3;
        P.why = "the stream ended";
    }
}

/* One packet: nf frames from the source, encoded, in RTP. */
static int send_packet(void)
{
    short pcm[2 * 128];
    unsigned char *p = P.pkt;
    unsigned nf = P.nf;
    unsigned left_frames = (unsigned)((P.total - P.sent) / P.spf);
    if (nf > left_frames)
        nf = left_frames;
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
    for (unsigned k = 0; k < nf; k++)
    {
        tune(pcm, P.spf);
        int r = bt_sbc_encode(&P.enc, pcm, p + n, P.fb);
        if (r != (int)P.fb)
            return -1;
        n += P.fb;
    }
    if (bt_chan_send(P.slot, p, n) < 0)
        return -1;
    P.seq = (P.seq + 1) & 0xFFFF;
    P.ts += nf * P.spf;
    P.sent += (unsigned long long)nf * P.spf;
    P.packets++;
    P.frames += nf;
    return 1;
}

void bt_a2dp_step(unsigned long long now)
{
    if (!P.active)
        return;
    if (!bt_avdtp_streaming())
    {
        P.active = 0;
        P.state = 3;
        P.why = "the stream is not running";
        return;
    }
    if (!now)
        return;
    unsigned long long played_ms = now - P.t0;
    unsigned long long sent_ms = P.sent * 1000 / P.rate;
    if (played_ms > sent_ms + BEHIND_MS) /* the system was busy for too long: do not catch up with a burst, start the clock again */
    {
        P.underruns++;
        P.t0 = now - sent_ms;
        played_ms = sent_ms;
    }
    for (int budget = 0; budget < PACKETS_PER_STEP && P.sent < P.total; budget++)
    {
        sent_ms = P.sent * 1000 / P.rate;
        if (sent_ms >= played_ms + LEAD_MS)
            break;
        int r = send_packet();
        if (r < 0)
        {
            if (++P.errors > 20)
            {
                P.active = 0;
                P.state = 3;
                P.why = "the packets are not taken";
            }
            return;
        }
        if (r == 0)
            break;
    }
    if (P.sent >= P.total || P.total - P.sent < P.spf)
    {
        P.active = 0;
        P.state = 2;
    }
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
    switch (P.state)
    {
    case 0:
        puts_(&o, "audio: -");
        break;
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
    if (P.state)
    {
        puts_(&o, ", ");
        putd(&o, P.packets);
        puts_(&o, " packets of ");
        putd(&o, P.nf);
        puts_(&o, " frames (");
        putd(&o, P.sent * 1000 / (P.rate ? P.rate : 1));
        puts_(&o, " ms of sound), ");
        putd(&o, P.underruns);
        puts_(&o, " underruns, ");
        putd(&o, P.errors);
        puts_(&o, " send errors");
    }
    buf[o.n] = 0;
    if (o.cut)
        return -1;
    return (int)o.n;
}
