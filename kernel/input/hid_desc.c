/* Minimal HID report descriptor reader for mice. See include/hid_desc.h. */
#include "hid_desc.h"

#define MAX_USAGES 16
#define MAX_IDS 16

long hid_value(const unsigned char *data, unsigned len, unsigned off, unsigned size, int is_signed)
{
    unsigned long long v = 0;
    if (size == 0 || size > 32)
        return 0;
    for (unsigned i = 0; i < size; i++)
    {
        unsigned bit = off + i;
        if (bit / 8 < len && ((data[bit / 8] >> (bit % 8)) & 1))
            v |= 1ULL << i;
    }
    if (is_signed && (v >> (size - 1)) & 1)
        return (long)((long long)v - (1LL << size));
    return (long)v;
}

typedef struct
{
    unsigned page;
    int lmin;
    unsigned size, count, id;
} Global;

static int signed_item(const unsigned char *p, unsigned n)
{
    int v = 0;
    for (unsigned i = 0; i < n; i++)
        v |= (int)p[i] << (8 * i);
    if (n && n < 4 && (p[n - 1] & 0x80))
        v -= 1 << (8 * n);
    return v;
}

static unsigned unsigned_item(const unsigned char *p, unsigned n)
{
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++)
        v |= (unsigned)p[i] << (8 * i);
    return v;
}

/* Usage Page 0x0D (Digitizers): 0x05 Touch Pad, 0x22 Finger, 0x52 Device Mode (HID Usage Tables) */
int hid_find_touchpad(const unsigned char *d, unsigned len, HidTouchpad *t)
{
    unsigned page = 0, report_id = 0;
    unsigned usages[MAX_USAGES], upages[MAX_USAGES], nus = 0;
    t->is_ptp = t->fingers = t->has_mode = 0;
    t->mode_report_id = 0;

    for (unsigned i = 0; i < len;)
    {
        unsigned char b = d[i];
        unsigned sz, type, tag;
        if (b == 0xFE) /* long item: skip */
        {
            if (i + 1 >= len)
                break;
            i += 3u + d[i + 1];
            continue;
        }
        sz = b & 3;
        if (sz == 3)
            sz = 4;
        type = (b >> 2) & 3;
        tag = b >> 4;
        if (i + 1 + sz > len)
            break;
        const unsigned char *v = d + i + 1;
        i += 1 + sz;

        if (type == 1) /* global */
        {
            if (tag == 0)
                page = unsigned_item(v, sz);
            else if (tag == 8)
                report_id = unsigned_item(v, sz);
        }
        else if (type == 2) /* local */
        {
            if (tag == 0 && nus < MAX_USAGES)
            {
                unsigned u = unsigned_item(v, sz);
                upages[nus] = sz == 4 ? (u >> 16) : page;
                usages[nus++] = u & 0xFFFF;
            }
        }
        else if (type == 0) /* main */
        {
            unsigned kind = unsigned_item(v, sz);
            int digit_last = nus && upages[nus - 1] == 0x0D;
            if (tag == 0xA && digit_last) /* Collection */
            {
                if (kind == 1 && usages[nus - 1] == 0x05)
                    t->is_ptp = 1;
                else if (kind == 2 && usages[nus - 1] == 0x22)
                    t->fingers++;
            }
            else if (tag == 0xB) /* Feature */
            {
                for (unsigned k = 0; k < nus; k++)
                    if (upages[k] == 0x0D && usages[k] == 0x52)
                    {
                        t->has_mode = 1;
                        t->mode_report_id = (int)report_id;
                    }
            }
            nus = 0;
        }
    }
    return t->is_ptp;
}

int hid_find_mouse(const unsigned char *d, unsigned len, HidMouse *m)
{
    Global g = {0, 0, 0, 0, 0};
    unsigned usages[MAX_USAGES], upages[MAX_USAGES], nus = 0, umin = 0;
    int have_range = 0;
    unsigned bits[MAX_IDS], ids[MAX_IDS], nids = 0; /* next free input bit per report ID */
    int depth = 0, mouse_depth = -1, uses_ids = 0;
    int got_x = 0, got_y = 0;
    HidMouse r = {0};

    for (unsigned i = 0; i < len;)
    {
        unsigned char b = d[i];
        unsigned sz, type, tag;
        if (b == 0xFE) /* long item: skip */
        {
            if (i + 1 >= len)
                break;
            i += 3u + d[i + 1];
            continue;
        }
        sz = b & 3;
        if (sz == 3)
            sz = 4;
        type = (b >> 2) & 3;
        tag = b >> 4;
        if (i + 1 + sz > len)
            break;
        const unsigned char *v = d + i + 1;
        i += 1 + sz;

        if (type == 1) /* global */
        {
            switch (tag)
            {
            case 0: g.page = unsigned_item(v, sz); break;
            case 1: g.lmin = signed_item(v, sz); break;
            case 7: g.size = unsigned_item(v, sz); break;
            case 8:
                g.id = unsigned_item(v, sz);
                uses_ids = 1;
                break;
            case 9: g.count = unsigned_item(v, sz); break;
            }
        }
        else if (type == 2) /* local */
        {
            if (tag == 0 && nus < MAX_USAGES)
            {
                unsigned u = unsigned_item(v, sz);
                upages[nus] = sz == 4 ? (u >> 16) : g.page; /* a 32-bit usage carries its page */
                usages[nus++] = u & 0xFFFF;
            }
            else if (tag == 1)
                umin = unsigned_item(v, sz), have_range = 1;
            else if (tag == 2)
                have_range = 1; /* the maximum is not needed: the count comes from Report Count */
        }
        else if (type == 0) /* main */
        {
            if (tag == 0xA) /* Collection */
            {
                if (depth == 0 && unsigned_item(v, sz) == 1 && nus && upages[nus - 1] == 1 && usages[nus - 1] == 2 &&
                    mouse_depth < 0 && !got_x)
                    mouse_depth = 0; /* Generic Desktop / Mouse, application collection */
                depth++;
            }
            else if (tag == 0xC) /* End Collection */
            {
                if (depth > 0)
                    depth--;
                if (mouse_depth >= 0 && depth == mouse_depth && got_x && got_y)
                {
                    r.uses_ids = uses_ids;
                    *m = r;
                    return 1;
                }
                if (depth == mouse_depth)
                    mouse_depth = -1;
            }
            else if (tag == 8) /* Input */
            {
                unsigned flags = unsigned_item(v, sz);
                unsigned k;
                for (k = 0; k < nids && ids[k] != g.id; k++)
                    ;
                if (k == nids && nids < MAX_IDS)
                {
                    ids[nids] = g.id;
                    bits[nids++] = 0;
                }
                if (k < MAX_IDS && k < nids)
                {
                    unsigned off = bits[k];
                    if (mouse_depth >= 0 && !(flags & 1) && (flags & 2)) /* variable data (not constant padding) */
                    {
                        for (unsigned c = 0; c < g.count; c++)
                        {
                            unsigned page = g.page, usage = 0;
                            if (have_range)
                                usage = umin + c;
                            else if (c < nus)
                                usage = usages[c], page = upages[c];
                            else if (nus)
                                usage = usages[nus - 1], page = upages[nus - 1];
                            unsigned at = off + c * g.size;
                            if (page == 9) /* buttons */
                            {
                                if (r.btn_n == 0)
                                    r.btn_off = at;
                                r.btn_n++;
                                r.report_id = (int)g.id;
                            }
                            else if (page == 1 && (flags & 4)) /* relative axes */
                            {
                                if (usage == 0x30 && !got_x)
                                    r.x_off = at, r.x_size = g.size, r.x_signed = g.lmin < 0, got_x = 1, r.report_id = (int)g.id;
                                else if (usage == 0x31 && !got_y)
                                    r.y_off = at, r.y_size = g.size, r.y_signed = g.lmin < 0, got_y = 1;
                                else if (usage == 0x38 && !r.has_wheel)
                                    r.w_off = at, r.w_size = g.size, r.w_signed = g.lmin < 0, r.has_wheel = 1;
                            }
                        }
                    }
                    bits[k] += g.count * g.size;
                }
            }
            nus = 0;
            have_range = 0;
        }
    }
    return 0;
}
