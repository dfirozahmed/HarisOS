/* Test helper: builds an exFAT volume in RAM, written from the exFAT specification (boot sector fields, entry
 * sets with checksums and name hashes, allocation bitmap, up-case table, FAT chains and NoFatChain files).
 * Used by test_exfat.c and test_volume.c. The kernel's reader (exfat.c) does not check the checksums; the
 * image has them so that other readers accept it. */
#ifndef HARIS_TEST_EXFATIMG_H
#define HARIS_TEST_EXFATIMG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    unsigned char *img;       /* the whole volume */
    unsigned total_sectors;
    unsigned ss;
    unsigned spc_shift;
    unsigned fat_off, fat_len, heap_off, clusters;
    unsigned next_free;       /* next cluster the simple allocator hands out */
    unsigned char *bitmap;    /* in-memory copy of the allocation bitmap */
    unsigned bitmap_cluster;
    unsigned root_cluster;
} XImg;

typedef struct XDir
{
    unsigned first;           /* first cluster of the folder (chain or contiguous) */
    unsigned nclusters;
    unsigned entries;         /* 32-byte entries written so far */
    int nofat;
    struct XDir *parent;      /* the folder this one is listed in (NULL for the root) */
    unsigned set_index, set_total; /* where its entry set is in the parent */
} XDir;

static unsigned xcs(const XImg *x) { return x->ss << x->spc_shift; }
static unsigned char *xclus(const XImg *x, unsigned c)
{
    return x->img + ((size_t)x->heap_off + (size_t)(c - 2) * (1u << x->spc_shift)) * x->ss;
}
static void xp16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void xp32(unsigned char *p, unsigned v) { xp16(p, v); xp16(p + 2, v >> 16); }
static void xp64(unsigned char *p, unsigned long long v) { xp32(p, (unsigned)v); xp32(p + 4, (unsigned)(v >> 32)); }

static void xfat_set(XImg *x, unsigned c, unsigned v) { xp32(x->img + ((size_t)x->fat_off) * x->ss + (size_t)c * 4, v); }
static unsigned xfat_get(const XImg *x, unsigned c)
{
    const unsigned char *p = x->img + ((size_t)x->fat_off) * x->ss + (size_t)c * 4;
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

static void xmark(XImg *x, unsigned c)
{
    unsigned i = c - 2;
    x->bitmap[i / 8] |= (unsigned char)(1u << (i % 8));
}

/* the next free cluster (one at a time; clusters of one file are then consecutive unless the test interleaves) */
static unsigned xalloc(XImg *x)
{
    unsigned c = x->next_free++;
    if (c >= x->clusters + 2) { printf("test image is full\n"); exit(2); }
    xmark(x, c);
    memset(xclus(x, c), 0, xcs(x));
    return c;
}

static unsigned short xchecksum16(const unsigned char *p, size_t n, int skip_checksum_bytes)
{
    unsigned short cs = 0;
    for (size_t i = 0; i < n; i++) {
        if (skip_checksum_bytes && (i == 2 || i == 3)) continue;
        cs = (unsigned short)(((cs & 1) ? 0x8000 : 0) + (cs >> 1) + p[i]);
    }
    return cs;
}

static unsigned xchecksum32(const unsigned char *p, size_t n)
{
    unsigned cs = 0;
    for (size_t i = 0; i < n; i++) cs = (((cs & 1) ? 0x80000000u : 0) + (cs >> 1)) + p[i];
    return cs;
}

static unsigned xupper(unsigned ch) { return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch; }

/* a new volume. label may be NULL. */
static void xformat(XImg *x, unsigned total_sectors, unsigned spc_shift, const char *label)
{
    memset(x, 0, sizeof *x);
    x->ss = 512;
    x->total_sectors = total_sectors;
    x->spc_shift = spc_shift;
    x->img = calloc(total_sectors, 512);
    x->fat_off = 128;
    unsigned spc = 1u << spc_shift;
    unsigned clusters = (total_sectors - 256) / spc;
    for (int i = 0; i < 4; i++) { /* the FAT length depends on the cluster count, which depends on the FAT length */
        x->fat_len = ((clusters + 2) * 4 + 511) / 512;
        x->heap_off = (x->fat_off + x->fat_len + 127) / 128 * 128;
        clusters = (total_sectors - x->heap_off) / spc;
    }
    x->clusters = clusters;
    x->bitmap = calloc((clusters + 7) / 8 + 1, 1);
    unsigned char *b = x->img;
    b[0] = 0xEB; b[1] = 0x76; b[2] = 0x90;
    memcpy(b + 3, "EXFAT   ", 8);
    xp64(b + 72, total_sectors);
    xp32(b + 80, x->fat_off); xp32(b + 84, x->fat_len); xp32(b + 88, x->heap_off); xp32(b + 92, clusters);
    x->bitmap_cluster = 2;
    x->root_cluster = 4;
    xp32(b + 96, x->root_cluster);
    xp32(b + 100, 0xC0FFEE01);
    xp16(b + 104, 0x0100);
    xp16(b + 106, 0);
    b[108] = 9; b[109] = (unsigned char)spc_shift; b[110] = 1; b[111] = 0x80; b[112] = 0;
    b[510] = 0x55; b[511] = 0xAA;
    for (int i = 1; i <= 8; i++) { x->img[i * 512 + 510] = 0x55; x->img[i * 512 + 511] = 0xAA; } /* extended boot sectors */
    /* boot region checksum sector: the checksum repeated, over sectors 0..10 without VolumeFlags / PercentInUse */
    unsigned cs = 0;
    for (int i = 0; i < 11 * 512; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        cs = (((cs & 1) ? 0x80000000u : 0) + (cs >> 1)) + x->img[i];
    }
    for (int i = 0; i < 512; i += 4) xp32(x->img + 11 * 512 + i, cs);
    memcpy(x->img + 12 * 512, x->img, 12 * 512); /* backup boot region */

    xfat_set(x, 0, 0xFFFFFFF8); xfat_set(x, 1, 0xFFFFFFFF);
    /* cluster 2: allocation bitmap, 3: up-case table, 4: root folder */
    x->next_free = 2;
    unsigned bm = xalloc(x), uc = xalloc(x), root = xalloc(x);
    xfat_set(x, bm, 0xFFFFFFFF); xfat_set(x, uc, 0xFFFFFFFF); xfat_set(x, root, 0xFFFFFFFF);
    unsigned char table[260];
    for (unsigned i = 0; i < 128; i++) xp16(table + 2 * i, xupper(i));
    xp16(table + 256, 0xFFFF);
    xp16(table + 258, 0xFF80); /* the other 65408 characters map to themselves */
    memcpy(xclus(x, uc), table, sizeof table);
    unsigned char *r = xclus(x, root);
    /* entry 0: volume label, 1: allocation bitmap, 2: up-case table */
    if (label && label[0]) {
        r[0] = 0x83; r[1] = (unsigned char)strlen(label);
        for (size_t i = 0; label[i]; i++) xp16(r + 2 + 2 * i, (unsigned char)label[i]);
    }
    r[32] = 0x81; r[32 + 1] = 0; xp32(r + 32 + 20, bm); xp64(r + 32 + 24, (clusters + 7) / 8);
    r[64] = 0x82; xp32(r + 64 + 4, xchecksum32(table, sizeof table)); xp32(r + 64 + 20, uc); xp64(r + 64 + 24, sizeof table);
}

static void xdir_root(XImg *x, XDir *d)
{
    memset(d, 0, sizeof *d);
    d->first = x->root_cluster;
    d->nclusters = 1;
    d->entries = 3; /* label, bitmap, up-case */
}

static unsigned char *xdir_slot(XImg *x, XDir *d, unsigned index)
{
    unsigned per = xcs(x) / 32;
    unsigned ci = index / per, c = d->first;
    if (d->nofat) c += ci;
    else for (unsigned i = 0; i < ci; i++) c = xfat_get(x, c);
    return xclus(x, c) + (index % per) * 32;
}

/* write an entry set for `name` into a folder; extends a chained folder by a cluster when it is full */
static void xupdate_set(XImg *x, XDir *d, unsigned index, unsigned total, unsigned long long size)
{   /* rewrite DataLength / ValidDataLength of a set in folder d and fix its checksum */
    unsigned char set[32 * 18];
    for (unsigned i = 0; i < total; i++) memcpy(set + 32 * i, xdir_slot(x, d, index + i), 32);
    xp64(set + 32 + 8, size); xp64(set + 32 + 24, size);
    xp16(set + 2, xchecksum16(set, 32 * total, 1));
    for (unsigned i = 0; i < total; i++) memcpy(xdir_slot(x, d, index + i), set + 32 * i, 32);
}

static void xadd(XImg *x, XDir *d, const char *name, unsigned attr, unsigned first, unsigned long long size,
                 unsigned long long valid, int nofat)
{
    unsigned len = (unsigned)strlen(name), nnames = (len + 14) / 15;
    unsigned total = 2 + nnames;
    unsigned per = xcs(x) / 32;
    while (d->entries + total > d->nclusters * per) {
        if (d->nofat) { printf("contiguous test folder is full\n"); exit(2); }
        unsigned c = xalloc(x), tail = d->first;
        for (unsigned i = 1; i < d->nclusters; i++) tail = xfat_get(x, tail);
        xfat_set(x, tail, c); xfat_set(x, c, 0xFFFFFFFF);
        d->nclusters++;
        if (d->parent) xupdate_set(x, d->parent, d->set_index, d->set_total, (unsigned long long)d->nclusters * xcs(x)); /* the folder grew */
    }
    unsigned char set[32 * 18];
    memset(set, 0, sizeof set);
    set[0] = 0x85; set[1] = (unsigned char)(total - 1); xp16(set + 4, attr);
    xp32(set + 8, 0x4E2A0000); xp32(set + 12, 0x4E2A0000); xp32(set + 16, 0x4E2A0000);
    unsigned char *st = set + 32;
    st[0] = 0xC0; st[1] = (unsigned char)(1 | (nofat ? 2 : 0)); st[3] = (unsigned char)len;
    unsigned hash = 0;
    for (unsigned i = 0; i < len; i++) {
        unsigned u = xupper((unsigned char)name[i]);
        hash = ((hash << 15) | (hash >> 1)) & 0xFFFF; hash = (hash + (u & 0xFF)) & 0xFFFF;
        hash = ((hash << 15) | (hash >> 1)) & 0xFFFF; hash = (hash + (u >> 8)) & 0xFFFF;
    }
    xp16(st + 4, hash);
    xp64(st + 8, valid); xp32(st + 20, first); xp64(st + 24, size);
    for (unsigned n = 0; n < nnames; n++) {
        unsigned char *ne = set + 32 * (2 + n);
        ne[0] = 0xC1;
        for (unsigned c = 0; c < 15; c++) {
            unsigned idx = n * 15 + c;
            unsigned ch = idx < len ? (unsigned char)name[idx] : 0;
            xp16(ne + 2 + 2 * c, ch);
        }
    }
    xp16(set + 2, xchecksum16(set, 32 * total, 1));
    for (unsigned i = 0; i < total; i++) memcpy(xdir_slot(x, d, d->entries + i), set + 32 * i, 32);
    d->entries += total;
}

/* make a folder `name` in `parent`; its own XDir is returned in *out (contiguous = NoFatChain folder of `nclusters`) */
static void xmkdir(XImg *x, XDir *parent, const char *name, XDir *out, int contiguous, unsigned nclusters)
{
    memset(out, 0, sizeof *out);
    unsigned first = 0;
    out->nofat = contiguous;
    out->nclusters = contiguous ? nclusters : 1;
    for (unsigned i = 0; i < out->nclusters; i++) {
        unsigned c = xalloc(x);
        if (i == 0) first = c;
        if (!contiguous) xfat_set(x, c, 0xFFFFFFFF);
    }
    out->first = first;
    out->parent = parent;
    out->set_index = parent->entries;
    out->set_total = 2 + (((unsigned)strlen(name) + 14) / 15);
    xadd(x, parent, name, 0x10, first, (unsigned long long)out->nclusters * xcs(x), (unsigned long long)out->nclusters * xcs(x), contiguous);
}

/* a file with the given content in consecutive clusters (NoFatChain) */
static void xfile_contig(XImg *x, XDir *d, const char *name, const unsigned char *data, unsigned size)
{
    unsigned n = (size + xcs(x) - 1) / xcs(x), first = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned c = xalloc(x);
        if (i == 0) first = c;
        memcpy(xclus(x, c), data + (size_t)i * xcs(x), size - (size_t)i * xcs(x) < xcs(x) ? size - (size_t)i * xcs(x) : xcs(x));
    }
    xadd(x, d, name, 0x20, first, size, size, 1);
}

/* a file in a FAT chain whose clusters are NOT consecutive: another cluster is taken between every two */
static void xfile_chained(XImg *x, XDir *d, const char *name, const unsigned char *data, unsigned size)
{
    unsigned n = (size + xcs(x) - 1) / xcs(x), first = 0, prev = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned c = xalloc(x);
        x->next_free++;  /* a gap: this cluster stays free, so the next one of the file is not next to this one */
        if (i == 0) first = c; else xfat_set(x, prev, c);
        xfat_set(x, c, 0xFFFFFFFF);
        memcpy(xclus(x, c), data + (size_t)i * xcs(x), size - (size_t)i * xcs(x) < xcs(x) ? size - (size_t)i * xcs(x) : xcs(x));
        prev = c;
    }
    xadd(x, d, name, 0x20, first, size, size, 0);
}

/* the bitmap must reach the image before it is used */
static void xfinish(XImg *x)
{
    memcpy(xclus(x, x->bitmap_cluster), x->bitmap, (x->clusters + 7) / 8);
}

#endif
