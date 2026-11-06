/* Builders of SDP data elements and two service records (an A2DP sink and a headset), shared by test_bt_sdp.c and test_bt_link.c. */
#ifndef HARIS_TEST_BT_SDP_FIXTURE_H
#define HARIS_TEST_BT_SDP_FIXTURE_H
#include <string.h>

/* ---- building data elements ---- */
typedef struct
{
    unsigned char b[3000];
    unsigned n;
} Buf;

static void add(Buf *b, const void *p, unsigned n)
{
    memcpy(b->b + b->n, p, n);
    b->n += n;
}

static void add_byte(Buf *b, unsigned v) { b->b[b->n++] = (unsigned char)v; }
static void add_uuid16(Buf *b, unsigned u)
{
    add_byte(b, 0x19);
    add_byte(b, u >> 8);
    add_byte(b, u);
}
static void add_u16(Buf *b, unsigned v)
{
    add_byte(b, 0x09);
    add_byte(b, v >> 8);
    add_byte(b, v);
}
static void add_u8(Buf *b, unsigned v)
{
    add_byte(b, 0x08);
    add_byte(b, v);
}
static void add_text(Buf *b, const char *s)
{
    unsigned n = (unsigned)strlen(s);
    if (n < 256)
    {
        add_byte(b, 0x25);
        add_byte(b, n);
    }
    else
    {
        add_byte(b, 0x26);
        add_byte(b, n >> 8);
        add_byte(b, n);
    }
    add(b, s, n);
}
/* a sequence around `inner`; the length field takes 1, 2 or 4 bytes, as asked (0 = the shortest that fits) */
static void add_seq(Buf *b, const Buf *inner, int lenbytes)
{
    if (lenbytes == 0)
        lenbytes = inner->n < 256 ? 1 : 2;
    if (lenbytes == 1)
    {
        add_byte(b, 0x35);
        add_byte(b, inner->n);
    }
    else if (lenbytes == 2)
    {
        add_byte(b, 0x36);
        add_byte(b, inner->n >> 8);
        add_byte(b, inner->n);
    }
    else
    {
        add_byte(b, 0x37);
        add_byte(b, 0);
        add_byte(b, 0);
        add_byte(b, inner->n >> 8);
        add_byte(b, inner->n);
    }
    add(b, inner->b, inner->n);
}

/* the A2DP sink record of a headset */
static void rec_a2dp(Buf *out)
{
    Buf r = {{0}, 0}, t, u, v;
    add_u16(&r, 0x0001); /* ServiceClassIDList */
    t.n = 0;
    add_uuid16(&t, 0x110B);
    add_seq(&r, &t, 0);
    add_u16(&r, 0x0004); /* ProtocolDescriptorList: L2CAP with PSM 0x19, AVDTP version 1.3 */
    t.n = 0;
    u.n = 0;
    add_uuid16(&u, 0x0100);
    add_u16(&u, 0x0019);
    add_seq(&t, &u, 0);
    u.n = 0;
    add_uuid16(&u, 0x0019);
    add_u16(&u, 0x0103);
    add_seq(&t, &u, 0);
    add_seq(&r, &t, 0);
    add_u16(&r, 0x0009); /* BluetoothProfileDescriptorList: A2DP 1.2 */
    t.n = 0;
    u.n = 0;
    add_uuid16(&u, 0x110D);
    add_u16(&u, 0x0102);
    add_seq(&t, &u, 0);
    add_seq(&r, &t, 0);
    add_u16(&r, 0x0100); /* ServiceName */
    add_text(&r, "Audio Sink");
    (void)v;
    add_seq(out, &r, 0);
}

/* the headset record: classes headset (0x1108) and generic audio (0x1203), RFCOMM channel 1, profile headset 1.2 */
static void rec_headset(Buf *out)
{
    Buf r = {{0}, 0}, t, u;
    add_u16(&r, 0x0001);
    t.n = 0;
    add_uuid16(&t, 0x1108);
    add_uuid16(&t, 0x1203);
    add_uuid16(&t, 0x9999); /* a third one is not kept */
    add_seq(&r, &t, 0);
    add_u16(&r, 0x0004);
    t.n = 0;
    u.n = 0;
    add_uuid16(&u, 0x0100);
    add_seq(&t, &u, 0); /* L2CAP without a parameter */
    u.n = 0;
    add_uuid16(&u, 0x0003);
    add_u8(&u, 1);
    add_seq(&t, &u, 0);
    add_seq(&r, &t, 0);
    add_u16(&r, 0x0009);
    t.n = 0;
    u.n = 0;
    add_uuid16(&u, 0x1108);
    add_u16(&u, 0x0102);
    add_seq(&t, &u, 0);
    add_seq(&r, &t, 0);
    add_u16(&r, 0x0100);
    add_text(&r, "Headset");
    add_seq(out, &r, 0);
}

#endif
