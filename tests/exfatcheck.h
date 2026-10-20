/* Test helper: an exFAT consistency checker that does not use exfat.c. It reads the image the way a strict
 * driver would and reports: bad entry-set checksums, wrong name hashes, wrong secondary counts, chains that do not
 * end or do not match the size, clusters used twice, clusters used but not marked in the allocation bitmap (or
 * marked but owned by nobody), duplicate names. Needs exfatimg.h (XImg) included first. */
#ifndef HARIS_TEST_EXFATCHECK_H
#define HARIS_TEST_EXFATCHECK_H

static int xc_errors;
#define XBAD(...) do { printf("  xfsck: " __VA_ARGS__); printf("\n"); xc_errors++; } while (0)

static unsigned char *xc_owner;   /* who owns each cluster: 0 nobody, 1 owned */

static unsigned xc_rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned xc_rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned long long xc_rd64(const unsigned char *p) { return xc_rd32(p) | (unsigned long long)xc_rd32(p + 4) << 32; }

static void xc_claim(const XImg *x, unsigned c, const char *what)
{
    if (c < 2 || c >= x->clusters + 2) { XBAD("%s: cluster %u out of range", what, c); return; }
    if (xc_owner[c]) XBAD("%s: cluster %u is owned twice", what, c);
    xc_owner[c] = 1;
}

/* the clusters of a stream: returns how many; `size_bytes` = expected length (0 = do not compare) */
static unsigned xc_stream(const XImg *x, unsigned first, int nofat, unsigned long long size_bytes, int compare, const char *what)
{
    unsigned n = 0;
    if (first == 0) {
        if (compare && size_bytes) XBAD("%s: no cluster but %llu bytes", what, size_bytes);
        return 0;
    }
    if (nofat) {
        unsigned want = (unsigned)((size_bytes + xcs(x) - 1) / xcs(x));
        for (unsigned i = 0; i < want; i++) xc_claim(x, first + i, what);
        return want;
    }
    unsigned c = first;
    while (c >= 2 && c < 0xFFFFFFF8u) {
        xc_claim(x, c, what);
        n++;
        if (n > x->clusters) { XBAD("%s: loop in the chain", what); return n; }
        unsigned next = xfat_get(x, c);
        if (next == 0 || next == 0xFFFFFFF7u) { XBAD("%s: chain runs into %s cluster", what, next ? "a bad" : "a free"); return n; }
        c = next;
    }
    if (compare) {
        unsigned want = (unsigned)((size_bytes + xcs(x) - 1) / xcs(x));
        if (n != want) XBAD("%s: %u clusters for %llu bytes (want %u)", what, n, size_bytes, want);
    }
    return n;
}

static unsigned xc_upper(unsigned ch) { return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch; }

/* one entry of a folder given as (first, nofat, number of clusters) */
static const unsigned char *xc_entry(const XImg *x, unsigned first, int nofat, unsigned index)
{
    unsigned per = xcs(x) / 32, ci = index / per, c = first;
    if (nofat) c += ci;
    else for (unsigned i = 0; i < ci; i++) c = xfat_get(x, c);
    return xclus(x, c) + (index % per) * 32;
}

static void xc_dir(const XImg *x, unsigned first, int nofat, unsigned nclusters, const char *path, int depth, int is_root)
{
    unsigned per = xcs(x) / 32, total = nclusters * per;
    char names[600][256];
    int nn = 0;
    int ended = 0;
    for (unsigned idx = 0; idx < total; idx++) {
        const unsigned char *e = xc_entry(x, first, nofat, idx);
        if (e[0] == 0) { ended = 1; continue; }
        if (ended) { XBAD("%s: entry after the end marker", path); break; }
        if (!(e[0] & 0x80)) continue;                   /* deleted */
        if (e[0] == 0x83 || e[0] == 0x81 || e[0] == 0x82) continue;
        if (e[0] != 0x85) { XBAD("%s: unexpected entry type 0x%02x at %u", path, e[0], idx); continue; }
        unsigned sec = e[1];
        if (sec < 2 || sec > 18 || idx + sec >= total) { XBAD("%s: bad secondary count %u", path, sec); continue; }
        unsigned char set[32 * 20];
        for (unsigned i = 0; i <= sec; i++) memcpy(set + 32 * i, xc_entry(x, first, nofat, idx + i), 32);
        unsigned short cs = xchecksum16(set, 32 * (sec + 1), 1);
        if (cs != xc_rd16(set + 2)) XBAD("%s: set at %u has checksum %04x, should be %04x", path, idx, xc_rd16(set + 2), cs);
        const unsigned char *st = set + 32;
        if (st[0] != 0xC0) { XBAD("%s: no stream entry at %u", path, idx); idx += sec; continue; }
        unsigned nlen = st[3];
        if (nlen == 0 || (nlen + 14) / 15 != sec - 1) XBAD("%s: name length %u does not fit %u name entries", path, nlen, sec - 1);
        char name[256];
        unsigned hash = 0;
        for (unsigned k = 0; k < nlen && k < 255; k++) {
            const unsigned char *ne = set + 32 * (2 + k / 15);
            if (ne[0] != 0xC1) { XBAD("%s: name entry missing", path); break; }
            unsigned ch = xc_rd16(ne + 2 + 2 * (k % 15));
            name[k] = (char)ch;
            unsigned u = xc_upper(ch);
            hash = ((hash << 15) | (hash >> 1)) & 0xFFFF; hash = (hash + (u & 0xFF)) & 0xFFFF;
            hash = ((hash << 15) | (hash >> 1)) & 0xFFFF; hash = (hash + (u >> 8)) & 0xFFFF;
        }
        name[nlen < 255 ? nlen : 255] = 0;
        if (hash != xc_rd16(st + 4)) XBAD("%s/%s: name hash %04x, should be %04x", path, name, xc_rd16(st + 4), hash);
        for (int i = 0; i < nn; i++) {
            const char *a = names[i], *b = name;
            while (*a && xc_upper((unsigned char)*a) == xc_upper((unsigned char)*b)) a++, b++;
            if (*a == *b) XBAD("%s: duplicate name %s", path, name);
        }
        if (nn < 600) strcpy(names[nn++], name);
        unsigned attr = xc_rd16(e + 4);
        unsigned fcl = xc_rd32(st + 20);
        unsigned long long dlen = xc_rd64(st + 24), vlen = xc_rd64(st + 8);
        int nofat_f = (st[1] & 2) != 0;
        if (!(st[1] & 1) && fcl) XBAD("%s/%s: AllocationPossible not set", path, name);
        if (vlen > dlen) XBAD("%s/%s: valid length %llu above data length %llu", path, name, vlen, dlen);
        char sub[512];
        snprintf(sub, sizeof sub, "%s/%s", path, name);
        unsigned n = xc_stream(x, fcl, nofat_f, dlen, 1, sub);
        if (attr & 0x10) {
            if (dlen % xcs(x)) XBAD("%s: folder length %llu is not a whole number of clusters", sub, dlen);
            if (depth < 12 && fcl) xc_dir(x, fcl, nofat_f, n, sub, depth + 1, 0);
        } else if (dlen == 0 && fcl) XBAD("%s: empty file owns cluster %u", sub, fcl);
        idx += sec;
    }
    (void)is_root;
}

/* run the whole check on x; returns the number of problems */
static int xfsck(const XImg *x)
{
    xc_errors = 0;
    xc_owner = calloc(x->clusters + 2, 1);
    /* the root folder, the bitmap and the up-case table come from the boot sector and the root entries */
    unsigned root = xc_rd32(x->img + 96);
    unsigned rn = xc_stream(x, root, 0, 0, 0, "root");
    const unsigned char *r0 = xclus(x, root);
    (void)r0;
    for (unsigned idx = 0; idx < rn * (xcs(x) / 32); idx++) {
        const unsigned char *e = xc_entry(x, root, 0, idx);
        if (e[0] == 0x81) { unsigned c = xc_rd32(e + 20); unsigned long long len = xc_rd64(e + 24); for (unsigned long long i = 0; i * xcs(x) < len; i++) xc_claim(x, c + (unsigned)i, "bitmap"); }
        if (e[0] == 0x82) { unsigned c = xc_rd32(e + 20); unsigned long long len = xc_rd64(e + 24); for (unsigned long long i = 0; i * xcs(x) < len; i++) xc_claim(x, c + (unsigned)i, "upcase"); }
        if (e[0] == 0) break;
    }
    xc_dir(x, root, 0, rn, "", 0, 1);
    /* the bitmap must say exactly this */
    const unsigned char *bm = NULL;
    for (unsigned idx = 0; idx < rn * (xcs(x) / 32); idx++) {
        const unsigned char *e = xc_entry(x, root, 0, idx);
        if (e[0] == 0x81) { bm = xclus(x, xc_rd32(e + 20)); break; }
    }
    if (!bm) XBAD("no allocation bitmap entry");
    else for (unsigned c = 2; c < x->clusters + 2; c++) {
        int bit = (bm[(c - 2) / 8] >> ((c - 2) % 8)) & 1;
        if (bit && !xc_owner[c]) { XBAD("cluster %u is marked used but nobody owns it", c); break; }
        if (!bit && xc_owner[c]) { XBAD("cluster %u is owned but not marked used", c); break; }
    }
    free(xc_owner);
    return xc_errors;
}

#endif
