/* Bluetooth SDP (Service Discovery Protocol): building a request and reading the answer. Pure functions, no hardware (host-tested).
 *
 * A Bluetooth device keeps a small database of "service records": one for each thing it can do (hands-free calls, stereo music sink, remote
 * control ...). SDP lets a client ask for them over an L2CAP channel to PSM 1. Everything is written as "data elements": a header byte
 * (type in the top 5 bits, size index in the low 3) then the value; sequences hold other elements. Core specification volume 3 part B.
 * Request used: ServiceSearchAttributeRequest (PDU 0x06) for every record that has the L2CAP protocol (UUID 0x0100, which all have),
 * asking for four attributes: ServiceClassIDList (1), ProtocolDescriptorList (4), BluetoothProfileDescriptorList (9), ServiceName (0x100). */
#include "bluetooth.h"

#define PDU_SEARCH_ATTR_REQ 0x06

static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }

/* The ServiceSearchAttributeRequest: PDU id, transaction id, parameter length, then the parameters (search pattern, largest answer we take,
 * attribute list, continuation state). Returns the length, or 0 if `cap` is too small. */
unsigned bt_sdp_request(unsigned char *out, unsigned cap, unsigned tid, unsigned max_bytes, const unsigned char *cont, unsigned contlen)
{
    static const unsigned char pattern[5] = {0x35, 0x03, 0x19, 0x01, 0x00};          /* sequence of one UUID: 0x0100, L2CAP */
    static const unsigned char attrs[14] = {0x35, 0x0C, 0x09, 0x00, 0x01, 0x09, 0x00, 0x04, 0x09, 0x00, 0x09, 0x09, 0x01, 0x00};
    unsigned params = sizeof pattern + 2 + sizeof attrs + 1 + contlen;
    if (contlen > 16 || 5 + params > cap)
        return 0;
    unsigned n = 0;
    out[n++] = PDU_SEARCH_ATTR_REQ;
    out[n++] = (unsigned char)(tid >> 8);
    out[n++] = (unsigned char)tid;
    out[n++] = (unsigned char)(params >> 8);
    out[n++] = (unsigned char)params;
    for (unsigned i = 0; i < sizeof pattern; i++)
        out[n++] = pattern[i];
    out[n++] = (unsigned char)(max_bytes >> 8);
    out[n++] = (unsigned char)max_bytes;
    for (unsigned i = 0; i < sizeof attrs; i++)
        out[n++] = attrs[i];
    out[n++] = (unsigned char)contlen;
    for (unsigned i = 0; i < contlen; i++)
        out[n++] = cont[i];
    return n;
}

/* Reads the header of the data element at p (n bytes left). Fills its type, the header length and the length of the value; 0 if it is not a
 * valid element or does not fit. Types: 0 nil, 1 unsigned int, 2 signed int, 3 UUID, 4 text, 5 boolean, 6 sequence, 7 alternative, 8 URL. */
static int element(const unsigned char *p, unsigned n, unsigned *type, unsigned *hlen, unsigned *dlen)
{
    if (n < 1)
        return 0;
    unsigned t = p[0] >> 3, sz = p[0] & 7, h = 1, d = 0;
    if (t > 8)
        return 0;
    if (t == 0)
    {
        if (sz != 0)
            return 0;
    }
    else if (t == 4 || t >= 6) /* text, sequence, alternative, URL: the length follows in 1, 2 or 4 bytes */
    {
        if (sz == 5)
        {
            if (n < 2)
                return 0;
            d = p[1];
            h = 2;
        }
        else if (sz == 6)
        {
            if (n < 3)
                return 0;
            d = be16(p + 1);
            h = 3;
        }
        else if (sz == 7)
        {
            if (n < 5)
                return 0;
            d = be32(p + 1);
            h = 5;
        }
        else
            return 0;
    }
    else /* numbers, UUIDs, booleans: fixed sizes 1, 2, 4, 8, 16 bytes */
    {
        if (sz > 4 || (t == 5 && sz != 0))
            return 0;
        d = 1u << sz;
    }
    if (d > n || h + d > n)
        return 0;
    *type = t;
    *hlen = h;
    *dlen = d;
    return 1;
}

static unsigned uint_of(const unsigned char *p, unsigned n)
{
    unsigned v = 0;
    for (unsigned i = 0; i < n && i < 4; i++)
        v = (v << 8) | p[i];
    return v;
}

/* A UUID as the 16-bit number everybody uses (the long forms with the Bluetooth base UUID reduce to it); 0xFFFF if it does not. */
static unsigned uuid_of(const unsigned char *p, unsigned n)
{
    static const unsigned char base[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};
    if (n == 2)
        return be16(p);
    if (n == 4)
        return be32(p) <= 0xFFFF ? be32(p) : 0xFFFF;
    if (n == 16 && p[0] == 0 && p[1] == 0)
    {
        for (int i = 0; i < 12; i++)
            if (p[4 + i] != base[i])
                return 0xFFFF;
        return be16(p + 2);
    }
    return 0xFFFF;
}

/* ServiceClassIDList: a sequence of UUIDs; the first two are kept. */
static void read_classes(const unsigned char *p, unsigned n, BtSdpService *s)
{
    int k = 0;
    while (n && k < 2)
    {
        unsigned t, h, d;
        if (!element(p, n, &t, &h, &d))
            return;
        if (t == 3)
            s->class_id[k++] = uuid_of(p + h, d);
        p += h + d;
        n -= h + d;
    }
}

/* ProtocolDescriptorList: a sequence of sequences (protocol UUID, parameters ...). L2CAP carries the PSM, RFCOMM a channel, AVDTP a version. */
static void read_protocols(const unsigned char *p, unsigned n, BtSdpService *s)
{
    while (n)
    {
        unsigned t, h, d;
        if (!element(p, n, &t, &h, &d))
            return;
        if (t == 6)
        {
            const unsigned char *q = p + h;
            unsigned qn = d, t2, h2, d2;
            if (element(q, qn, &t2, &h2, &d2) && t2 == 3)
            {
                unsigned proto = uuid_of(q + h2, d2);
                q += h2 + d2;
                qn -= h2 + d2;
                if (qn && element(q, qn, &t2, &h2, &d2) && t2 == 1)
                {
                    unsigned v = uint_of(q + h2, d2);
                    if (proto == 0x0100 && !s->has_psm)
                    {
                        s->has_psm = 1;
                        s->psm = v;
                    }
                    else if (proto == 0x0003)
                    {
                        s->has_rfcomm = 1;
                        s->rfcomm = v;
                    }
                    else if (proto == 0x0019)
                        s->avdtp_ver = v;
                }
            }
        }
        p += h + d;
        n -= h + d;
    }
}

/* BluetoothProfileDescriptorList: a sequence of sequences (profile UUID, version as major.minor in 16 bits). The first one is kept. */
static void read_profiles(const unsigned char *p, unsigned n, BtSdpService *s)
{
    if (s->profile)
        return;
    while (n)
    {
        unsigned t, h, d;
        if (!element(p, n, &t, &h, &d))
            return;
        if (t == 6)
        {
            const unsigned char *q = p + h;
            unsigned qn = d, t2, h2, d2;
            if (element(q, qn, &t2, &h2, &d2) && t2 == 3)
            {
                unsigned u = uuid_of(q + h2, d2);
                q += h2 + d2;
                qn -= h2 + d2;
                if (qn && element(q, qn, &t2, &h2, &d2) && t2 == 1 && !s->profile)
                {
                    s->profile = u;
                    s->profile_ver = uint_of(q + h2, d2);
                    return;
                }
            }
        }
        p += h + d;
        n -= h + d;
    }
}

/* ServiceName: text; kept up to 23 bytes, cut between UTF-8 characters. */
static void read_name(const unsigned char *p, unsigned n, BtSdpService *s)
{
    if (n >= sizeof s->name)
    {
        n = sizeof s->name - 1;
        while (n > 0 && (p[n] & 0xC0) == 0x80)
            n--;
    }
    unsigned k = 0;
    for (unsigned i = 0; i < n && p[i]; i++)
        s->name[k++] = (char)p[i];
    s->name[k] = 0;
}

/* One service record: a sequence of (attribute id, value) pairs. */
static void read_record(const unsigned char *p, unsigned n, BtSdpService *s)
{
    unsigned char *z = (unsigned char *)s;
    for (unsigned i = 0; i < sizeof *s; i++)
        z[i] = 0;
    while (n)
    {
        unsigned t, h, d;
        if (!element(p, n, &t, &h, &d) || t != 1 || d != 2) /* the attribute id: an unsigned 16-bit number */
            return;
        unsigned id = be16(p + h);
        p += h + d;
        n -= h + d;
        unsigned t2, h2, d2;
        if (!n || !element(p, n, &t2, &h2, &d2))
            return;
        if (t2 == 6 && id == 0x0001)
            read_classes(p + h2, d2, s);
        else if (t2 == 6 && id == 0x0004)
            read_protocols(p + h2, d2, s);
        else if (t2 == 6 && id == 0x0009)
            read_profiles(p + h2, d2, s);
        else if (t2 == 4 && id == 0x0100)
            read_name(p + h2, d2, s);
        p += h2 + d2;
        n -= h2 + d2;
    }
}

/* `lists` is the AttributeLists of a ServiceSearchAttributeResponse: a sequence with one sequence per record. Fills up to `max` services and
 * returns how many were found, or -1 if it is not a sequence. Damaged data ends the reading: what came before is kept. */
int bt_sdp_parse(const unsigned char *lists, unsigned len, BtSdpService *out, unsigned max)
{
    unsigned t, h, d;
    if (!element(lists, len, &t, &h, &d) || t != 6)
        return -1;
    const unsigned char *p = lists + h;
    unsigned n = d;
    int count = 0;
    while (n && (unsigned)count < max)
    {
        unsigned t2, h2, d2;
        if (!element(p, n, &t2, &h2, &d2) || t2 != 6)
            break;
        read_record(p + h2, d2, &out[count]);
        count++;
        p += h2 + d2;
        n -= h2 + d2;
    }
    return count;
}

/* The names people know the 16-bit service class / profile numbers by (Bluetooth Assigned Numbers). */
const char *bt_sdp_uuid_name(unsigned uuid)
{
    switch (uuid)
    {
    case 0x1101: return "serial port";
    case 0x1105: return "file push (OPP)";
    case 0x1106: return "file transfer (FTP)";
    case 0x1108: return "headset";
    case 0x1112: return "headset gateway";
    case 0x1131: return "headset (HS)";
    case 0x110A: return "A2DP source";
    case 0x110B: return "A2DP sink";
    case 0x110C: return "AVRCP target";
    case 0x110D: return "A2DP";
    case 0x110E: return "AVRCP";
    case 0x110F: return "AVRCP controller";
    case 0x111E: return "hands-free";
    case 0x111F: return "hands-free gateway";
    case 0x1124: return "HID";
    case 0x1200: return "PnP information";
    default: return "";
    }
}

/* ---- the server side: the one service record we offer, "Object Push" (receive files) ----
 * A phone that wants to send us a file asks our SDP server, over its own L2CAP channel to PSM 1, which RFCOMM channel carries the file service. The
 * three requests phones use are answered: ServiceSearch (0x02: which records match), ServiceAttribute (0x04: the attributes of a record) and
 * ServiceSearchAttribute (0x06: both in one go). There is one record, handle 0x00010001, and no answer is ever cut into pieces (no continuation). */
#define SRV_HANDLE 0x00010001u
#define SRV_ATTRS 8

typedef struct
{
    unsigned id, len;
    unsigned char v[24];
} SrvAttr;

static void srv_attr(SrvAttr *a, unsigned id, const unsigned char *v, unsigned n)
{
    a->id = id;
    a->len = n;
    for (unsigned i = 0; i < n; i++)
        a->v[i] = v[i];
}

/* the record's attributes in ascending order of attribute id */
static void srv_record(SrvAttr *a, unsigned channel)
{
    static const unsigned char handle[] = {0x0A, 0x00, 0x01, 0x00, 0x01};
    static const unsigned char classes[] = {0x35, 0x03, 0x19, 0x11, 0x05}; /* Object Push */
    unsigned char proto[] = {0x35, 0x11, 0x35, 0x03, 0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0, 0x35, 0x03, 0x19, 0x00, 0x08}; /* L2CAP, RFCOMM channel, OBEX */
    static const unsigned char browse[] = {0x35, 0x03, 0x19, 0x10, 0x02};                                    /* public browse group */
    static const unsigned char lang[] = {0x35, 0x09, 0x09, 0x65, 0x6E, 0x09, 0x00, 0x6A, 0x09, 0x01, 0x00}; /* "en", UTF-8, names start at 0x0100 */
    static const unsigned char prof[] = {0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x05, 0x09, 0x01, 0x00};       /* Object Push profile 1.0 */
    static const unsigned char name[] = {0x25, 0x07, 'H', 'a', 'r', 'i', 's', 'O', 'S'};
    static const unsigned char formats[] = {0x35, 0x02, 0x08, 0xFF}; /* any object format */
    proto[13] = (unsigned char)channel;
    srv_attr(&a[0], 0x0000, handle, sizeof handle);
    srv_attr(&a[1], 0x0001, classes, sizeof classes);
    srv_attr(&a[2], 0x0004, proto, sizeof proto);
    srv_attr(&a[3], 0x0005, browse, sizeof browse);
    srv_attr(&a[4], 0x0006, lang, sizeof lang);
    srv_attr(&a[5], 0x0009, prof, sizeof prof);
    srv_attr(&a[6], 0x0100, name, sizeof name);
    srv_attr(&a[7], 0x0303, formats, sizeof formats);
}

/* is attribute `id` in the AttributeIDList (a sequence of 16-bit ids and 32-bit ranges)? */
static int attr_wanted(const unsigned char *list, unsigned n, unsigned id)
{
    unsigned t, h, d;
    if (!element(list, n, &t, &h, &d) || t != 6)
        return 0;
    const unsigned char *p = list + h;
    unsigned left = d;
    while (left)
    {
        unsigned t2, h2, d2;
        if (!element(p, left, &t2, &h2, &d2) || t2 != 1)
            return 0;
        if (d2 == 2 && be16(p + h2) == id)
            return 1;
        if (d2 == 4 && id >= be16(p + h2) && id <= be16(p + h2 + 2))
            return 1;
        p += h2 + d2;
        left -= h2 + d2;
    }
    return 0;
}

/* does our record match the pattern (a sequence of UUIDs)? Every UUID in it must be one the record contains. */
static int pattern_matches(const unsigned char *pat, unsigned n)
{
    unsigned t, h, d;
    if (!element(pat, n, &t, &h, &d) || t != 6)
        return 0;
    const unsigned char *p = pat + h;
    unsigned left = d;
    while (left)
    {
        unsigned t2, h2, d2;
        if (!element(p, left, &t2, &h2, &d2) || t2 != 3)
            return 0;
        unsigned u = uuid_of(p + h2, d2);
        if (u != 0x0100 && u != 0x0003 && u != 0x0008 && u != 0x1002 && u != 0x1105)
            return 0;
        p += h2 + d2;
        left -= h2 + d2;
    }
    return 1;
}

/* the matching attributes as the pairs (id, value) of one record, wrapped in a sequence; returns its length or 0 if it does not fit */
static unsigned srv_pairs(unsigned char *out, unsigned cap, unsigned channel, const unsigned char *list, unsigned listn)
{
    SrvAttr a[SRV_ATTRS];
    srv_record(a, channel);
    unsigned n = 2;
    if (cap < 2)
        return 0;
    for (int i = 0; i < SRV_ATTRS; i++)
    {
        if (!attr_wanted(list, listn, a[i].id))
            continue;
        if (n + 3 + a[i].len > cap)
            return 0;
        out[n++] = 0x09;
        out[n++] = (unsigned char)(a[i].id >> 8);
        out[n++] = (unsigned char)a[i].id;
        for (unsigned k = 0; k < a[i].len; k++)
            out[n++] = a[i].v[k];
    }
    if (n - 2 > 255)
        return 0;
    out[0] = 0x35;
    out[1] = (unsigned char)(n - 2);
    return n;
}

static unsigned srv_error(unsigned char *out, unsigned cap, unsigned tid, unsigned code)
{
    if (cap < 7)
        return 0;
    out[0] = 0x01;
    out[1] = (unsigned char)(tid >> 8);
    out[2] = (unsigned char)tid;
    out[3] = 0;
    out[4] = 2;
    out[5] = (unsigned char)(code >> 8);
    out[6] = (unsigned char)code;
    return 7;
}

unsigned bt_sdps_answer(const unsigned char *req, unsigned n, unsigned char *out, unsigned cap, unsigned channel)
{
    if (n < 5 || cap < 16)
        return 0;
    unsigned pdu = req[0], tid = be16(req + 1), plen = be16(req + 3);
    if (5 + plen > n)
        return srv_error(out, cap, tid, 0x0003);
    const unsigned char *p = req + 5;
    unsigned left = plen;
    unsigned t, h, d;
    out[1] = (unsigned char)(tid >> 8);
    out[2] = (unsigned char)tid;
    if (pdu == 0x02) /* ServiceSearchRequest: pattern, max record count (2), continuation (1) */
    {
        if (!element(p, left, &t, &h, &d) || h + d + 3 > left)
            return srv_error(out, cap, tid, 0x0003);
        if (p[h + d + 2] != 0)
            return srv_error(out, cap, tid, 0x0005);
        int match = pattern_matches(p, h + d) && be16(p + h + d) > 0;
        out[0] = 0x03;
        out[3] = 0;
        out[4] = (unsigned char)(match ? 9 : 5);
        out[5] = 0;
        out[6] = (unsigned char)(match ? 1 : 0); /* total records */
        out[7] = 0;
        out[8] = (unsigned char)(match ? 1 : 0); /* records in this answer */
        unsigned k = 9;
        if (match)
        {
            out[k++] = (unsigned char)(SRV_HANDLE >> 24);
            out[k++] = (unsigned char)(SRV_HANDLE >> 16);
            out[k++] = (unsigned char)(SRV_HANDLE >> 8);
            out[k++] = (unsigned char)SRV_HANDLE;
        }
        out[k++] = 0; /* no continuation */
        return k;
    }
    if (pdu == 0x04) /* ServiceAttributeRequest: handle (4), max bytes (2), attribute list, continuation (1) */
    {
        if (left < 7 || !element(p + 6, left - 6, &t, &h, &d) || 6 + h + d + 1 > left)
            return srv_error(out, cap, tid, 0x0003);
        if (p[6 + h + d] != 0)
            return srv_error(out, cap, tid, 0x0005);
        if (be32(p) != SRV_HANDLE)
            return srv_error(out, cap, tid, 0x0002);
        unsigned m = srv_pairs(out + 7, cap - 8, channel, p + 6, h + d);
        if (!m)
            return srv_error(out, cap, tid, 0x0003);
        out[0] = 0x05;
        out[5] = (unsigned char)(m >> 8);
        out[6] = (unsigned char)m;
        out[7 + m] = 0;
        out[3] = (unsigned char)((m + 3) >> 8);
        out[4] = (unsigned char)(m + 3);
        return 5 + 2 + m + 1;
    }
    if (pdu == 0x06) /* ServiceSearchAttributeRequest: pattern, max bytes (2), attribute list, continuation (1) */
    {
        if (!element(p, left, &t, &h, &d) || h + d + 2 > left)
            return srv_error(out, cap, tid, 0x0003);
        unsigned plen1 = h + d, off = plen1 + 2;
        unsigned t2, h2, d2;
        if (!element(p + off, left - off, &t2, &h2, &d2) || off + h2 + d2 + 1 > left)
            return srv_error(out, cap, tid, 0x0003);
        if (p[off + h2 + d2] != 0)
            return srv_error(out, cap, tid, 0x0005);
        unsigned char *lists = out + 7;
        unsigned m;
        if (pattern_matches(p, plen1))
        {
            unsigned r = srv_pairs(lists + 2, cap - 10, channel, p + off, h2 + d2);
            if (!r)
                return srv_error(out, cap, tid, 0x0003);
            lists[0] = 0x35;
            lists[1] = (unsigned char)r;
            m = r + 2;
        }
        else
        {
            lists[0] = 0x35;
            lists[1] = 0;
            m = 2;
        }
        out[0] = 0x07;
        out[5] = (unsigned char)(m >> 8);
        out[6] = (unsigned char)m;
        out[7 + m] = 0;
        out[3] = (unsigned char)((m + 3) >> 8);
        out[4] = (unsigned char)(m + 3);
        return 5 + 2 + m + 1;
    }
    return srv_error(out, cap, tid, 0x0003);
}
