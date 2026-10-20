/* Test helper: builds an NTFS volume in RAM, written from the NTFS on-disk format: boot sector, MFT with
 * fixups, resident and non-resident attributes with data runs (fragmented, sparse, negative offsets), attribute
 * lists with extension records, folder indexes (resident root; a two-level tree with INDX blocks), Win32 + DOS
 * names. Used by test_ntfs.c. 7-Zip can read the images this makes (build with NTFS_DUMP=file to write one). */
#ifndef HARIS_TEST_NTFSIMG_H
#define HARIS_TEST_NTFSIMG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NT_SS 512
#define NT_SPC 8
#define NT_CS (NT_SS * NT_SPC)   /* 4096 bytes per cluster */
#define NT_REC 1024
#define NT_SECTORS 32768         /* 16 MiB */
#define NT_CLUSTERS (NT_SECTORS / NT_SPC)
#define NT_MFT_LCN 4
#define NT_MFT_RECORDS 256
#define REF_(n) ((unsigned long long)(n) | (1ULL << 48))

typedef struct
{
    unsigned char *img;
    unsigned next_lcn;                       /* the data allocator */
    unsigned char bitmap[NT_CLUSTERS / 8];   /* $Bitmap content */
    unsigned char *mft;                      /* records, one NT_REC each, before fixups are applied by nrec_finish */
} NImg;

static void np16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void np32(unsigned char *p, unsigned v) { np16(p, v); np16(p + 2, v >> 16); }
static void np64(unsigned char *p, unsigned long long v) { np32(p, (unsigned)v); np32(p + 4, (unsigned)(v >> 32)); }
static unsigned nalign8(unsigned v) { return (v + 7) & ~7u; }

static void nmark(NImg *n, unsigned lcn, unsigned count)
{
    for (unsigned i = 0; i < count; i++) n->bitmap[(lcn + i) / 8] |= (unsigned char)(1u << ((lcn + i) % 8));
}

/* take `count` consecutive clusters for data */
static unsigned nalloc(NImg *n, unsigned count)
{
    unsigned lcn = n->next_lcn;
    if (lcn + count > NT_CLUSTERS) { printf("test image full\n"); exit(2); }
    n->next_lcn += count;
    nmark(n, lcn, count);
    return lcn;
}

static unsigned char *ncl(NImg *n, unsigned lcn) { return n->img + (size_t)lcn * NT_CS; }

/* ---- records ---- */
typedef struct
{
    unsigned char b[NT_REC];
    unsigned used;      /* bytes used so far (attributes are appended from 0x38) */
    unsigned next_id;
    unsigned recno;
} NRec;

static void nrec_begin(NRec *r, unsigned recno, unsigned flags /* 1 = in use, 2 = folder */, unsigned long long base_ref)
{
    memset(r, 0, sizeof *r);
    memcpy(r->b, "FILE", 4);
    np16(r->b + 4, 0x30);   /* update sequence array */
    np16(r->b + 6, NT_REC / 512 + 1);
    np16(r->b + 16, 1);     /* sequence number */
    np16(r->b + 18, base_ref ? 0 : 1); /* hard link count */
    np16(r->b + 20, 0x38);  /* first attribute */
    np16(r->b + 22, flags);
    np32(r->b + 28, NT_REC);
    np64(r->b + 32, base_ref);
    np32(r->b + 44, recno);
    r->used = 0x38;
    r->next_id = 0;
    r->recno = recno;
}

/* append a resident attribute */
static unsigned nrec_res(NRec *r, unsigned type, const char *name, const void *data, unsigned len)
{
    unsigned nl = name ? (unsigned)strlen(name) : 0;
    unsigned name_off = 0x18, content_off = nalign8(name_off + nl * 2);
    unsigned total = nalign8(content_off + len);
    if (r->used + total + 8 > NT_REC) { printf("record %u full\n", r->recno); exit(2); }
    unsigned char *a = r->b + r->used;
    unsigned id = r->next_id++;
    np32(a, type); np32(a + 4, total);
    a[8] = 0; a[9] = (unsigned char)nl; np16(a + 10, nl ? name_off : 0); np16(a + 12, 0); np16(a + 14, id);
    np32(a + 16, len); np16(a + 20, content_off);
    for (unsigned i = 0; i < nl; i++) np16(a + name_off + 2 * i, (unsigned char)name[i]);
    if (len) memcpy(a + content_off, data, len);
    r->used += total;
    return id;
}

/* a run list as bytes; lcn < 0 = hole. returns the length */
typedef struct { unsigned long long len; long long lcn; } NRunSpec;

static unsigned nencode_runs(const NRunSpec *runs, int n, unsigned char *out)
{
    unsigned o = 0;
    long long prev = 0;
    for (int i = 0; i < n; i++) {
        unsigned long long len = runs[i].len;
        unsigned nlen = 1;
        while (nlen < 8 && (len >> (8 * nlen))) nlen++;
        unsigned noff = 0;
        long long delta = 0;
        if (runs[i].lcn >= 0) {
            delta = runs[i].lcn - prev;
            prev = runs[i].lcn;
            noff = 1;
            for (;;) {   /* the smallest signed width that holds delta */
                long long lo = -(1LL << (8 * noff - 1)), hi = (1LL << (8 * noff - 1)) - 1;
                if (delta >= lo && delta <= hi) break;
                noff++;
            }
        }
        out[o++] = (unsigned char)(nlen | (noff << 4));
        for (unsigned k = 0; k < nlen; k++) out[o++] = (unsigned char)(len >> (8 * k));
        for (unsigned k = 0; k < noff; k++) out[o++] = (unsigned char)((unsigned long long)delta >> (8 * k));
    }
    out[o++] = 0;
    return o;
}

/* append a non-resident attribute extent covering VCN [start_vcn, start_vcn + sum of run lengths) */
static unsigned nrec_nonres(NRec *r, unsigned type, const char *name, const NRunSpec *runs, int nruns, unsigned long long start_vcn,
                            unsigned long long alloc, unsigned long long real, unsigned long long init, unsigned flags)
{
    unsigned nl = name ? (unsigned)strlen(name) : 0;
    unsigned ext = (flags & 0x8001) ? 8 : 0;      /* compressed or sparse: the header has a 'compressed size' field and a unit */
    unsigned name_off = 0x40 + ext, runs_off = nalign8(name_off + nl * 2);
    unsigned char rb[256];
    unsigned rlen = nencode_runs(runs, nruns, rb);
    unsigned total = nalign8(runs_off + rlen);
    if (r->used + total + 8 > NT_REC) { printf("record %u full\n", r->recno); exit(2); }
    unsigned long long vcns = 0;
    for (int i = 0; i < nruns; i++) vcns += runs[i].len;
    unsigned char *a = r->b + r->used;
    unsigned id = r->next_id++;
    np32(a, type); np32(a + 4, total);
    a[8] = 1; a[9] = (unsigned char)nl; np16(a + 10, nl ? name_off : 0); np16(a + 12, flags); np16(a + 14, id);
    np64(a + 16, start_vcn); np64(a + 24, start_vcn + vcns - 1);
    np16(a + 32, runs_off); np16(a + 34, ext ? 4 : 0); /* 2^4 = 16 clusters per compression unit */
    np64(a + 40, alloc); np64(a + 48, real); np64(a + 56, init);
    if (ext) np64(a + 64, alloc);
    for (unsigned i = 0; i < nl; i++) np16(a + name_off + 2 * i, (unsigned char)name[i]);
    memcpy(a + runs_off, rb, rlen);
    r->used += total;
    return id;
}

/* seal a record: end marker, sizes, fixups; copy into the MFT */
static void nrec_finish(NImg *n, NRec *r)
{
    np32(r->b + r->used, 0xFFFFFFFFu);
    r->used += 8;
    np32(r->b + 24, r->used);
    np16(r->b + 40, r->next_id);
    unsigned short usn = 0xBEEF;
    np16(r->b + 0x30, usn);
    for (unsigned i = 1; i <= NT_REC / 512; i++) {
        unsigned char *end = r->b + i * 512 - 2;
        r->b[0x30 + 2 * i] = end[0];
        r->b[0x30 + 2 * i + 1] = end[1];
        np16(end, usn);
    }
    memcpy(n->mft + (size_t)r->recno * NT_REC, r->b, NT_REC);
}

/* ---- names and index entries ---- */
static unsigned nmakekey(unsigned char *key, unsigned long long parent_ref, const char *name, unsigned long long size, unsigned flags, unsigned ns)
{
    unsigned nl = (unsigned)strlen(name);
    memset(key, 0, 66 + nl * 2);
    np64(key, parent_ref);
    np64(key + 40, size); np64(key + 48, size);
    np32(key + 56, flags);
    key[64] = (unsigned char)nl; key[65] = (unsigned char)ns;
    for (unsigned i = 0; i < nl; i++) {
        unsigned ch = (unsigned char)name[i];
        if (ch == 1) ch = 0xE9; /* a test name can say "\x01" for U+00E9 */
        np16(key + 66 + 2 * i, ch);
    }
    return 66 + nl * 2;
}

typedef struct
{
    const char *name;
    unsigned recno;
    int is_dir;
    unsigned long long size;
    unsigned ns;         /* 1 = Win32, 2 = DOS, 3 = both */
} NChild;

static int ncmp(const void *a, const void *b)
{
    const NChild *x = (const NChild *)a, *y = (const NChild *)b;
    const char *p = x->name, *q = y->name;
    while (*p && *q) {
        int c = (*p >= 'a' && *p <= 'z' ? *p - 32 : *p) - (*q >= 'a' && *q <= 'z' ? *q - 32 : *q);
        if (c) return c;
        p++; q++;
    }
    return (int)(unsigned char)*p - (int)(unsigned char)*q;
}

/* one index entry: ref | length | key length | flags | key | [pad] | [sub-node VCN]. Returns its length. */
static unsigned nentry(unsigned char *out, const NChild *c, unsigned long long parent_ref, int subnode, unsigned long long vcn)
{
    unsigned char key[66 + 2 * 256];
    unsigned klen = nmakekey(key, parent_ref, c->name, c->size, c->is_dir ? 0x10000000u : 0x20u, c->ns);
    unsigned len = nalign8(16 + klen) + (subnode ? 8 : 0);
    memset(out, 0, len);
    np64(out, (unsigned long long)c->recno | (1ULL << 48));
    np16(out + 8, len); np16(out + 10, klen); np16(out + 12, subnode ? 1 : 0);
    memcpy(out + 16, key, klen);
    if (subnode) np64(out + len - 8, vcn);
    return len;
}

/* the end entry: no key; with a sub-node pointer when subnode */
static unsigned nend_entry(unsigned char *out, int subnode, unsigned long long vcn)
{
    unsigned len = subnode ? 24 : 16;
    memset(out, 0, len);
    np16(out + 8, len); np16(out + 12, subnode ? 3 : 2);
    if (subnode) np64(out + 16, vcn);
    return len;
}

/* the $FILE_NAME attribute (and the DOS twin) of a child, in its own record */
static void nfilename_attrs(NRec *r, unsigned long long parent_ref, const NChild *c, const char *dos_name)
{
    unsigned char key[66 + 2 * 256];
    unsigned klen = nmakekey(key, parent_ref, c->name, c->size, c->is_dir ? 0x10000000u : 0x20u, dos_name ? 1 : (c->ns ? c->ns : 1));
    nrec_res(r, 0x30, 0, key, klen);
    if (dos_name) {
        klen = nmakekey(key, parent_ref, dos_name, c->size, c->is_dir ? 0x10000000u : 0x20u, 2);
        nrec_res(r, 0x30, 0, key, klen);
    }
}

static void nstdinfo(NRec *r, unsigned flags)
{
    unsigned char si[72];
    memset(si, 0, sizeof si);
    np64(si, 0x01D9000000000000ULL); np64(si + 8, 0x01D9000000000000ULL);
    np64(si + 16, 0x01D9000000000000ULL); np64(si + 24, 0x01D9000000000000ULL);
    np32(si + 32, flags);
    nrec_res(r, 0x10, 0, si, sizeof si);
}

/* make the index of a folder: resident when it fits, otherwise a two-level tree. kids are sorted here. The folder's
 * own record `d` gets $INDEX_ROOT (and $INDEX_ALLOCATION + $BITMAP). */
static void nindex(NImg *n, NRec *d, NChild *kids, int nk, unsigned long long self_ref, const char *dos_for_each_hint)
{
    (void)dos_for_each_hint;
    qsort(kids, (size_t)nk, sizeof *kids, ncmp);
    static unsigned char ents[64 * 1024];
    unsigned char root[16 + 16 + 4096];
    unsigned o = 0, offs[600];
    for (int i = 0; i < nk; i++) { offs[i] = o; o += nentry(ents + o, &kids[i], self_ref, 0, 0); }
    offs[nk] = o;
    if (16 + o + 16 <= 600) {   /* fits in the resident root */
        memset(root, 0, sizeof root);
        np32(root, 0x30); np32(root + 4, 1); np32(root + 8, 4096); root[12] = 1;
        unsigned total = o + 16;
        np32(root + 16, 16); np32(root + 20, 16 + total); np32(root + 24, 16 + total); root[28] = 0;
        memcpy(root + 32, ents, o);
        nend_entry(root + 32 + o, 0, 0);
        nrec_res(d, 0x90, "$I30", root, 32 + total);
        return;
    }
    /* a tree: leaf blocks hold groups of entries, the root holds one separator entry per block and an end entry */
    int per_block = 30;
    int nblocks = (nk + per_block) / (per_block + 1);   /* each block is followed by a separator, the last needs none */
    unsigned lcn = nalloc(n, (unsigned)nblocks);
    unsigned char *rp = root + 32;
    memset(root, 0, sizeof root);
    unsigned ro = 0;
    int idx = 0;
    for (int b = 0; b < nblocks; b++) {
        unsigned char *blk = ncl(n, lcn + (unsigned)b);
        memset(blk, 0, NT_CS);
        memcpy(blk, "INDX", 4);
        np16(blk + 4, 0x28); np16(blk + 6, 4096 / 512 + 1);
        np64(blk + 16, (unsigned long long)b);
        unsigned bo = 0x40;
        int count = 0;
        while (idx < nk && count < per_block) { bo += nentry(blk + bo, &kids[idx], self_ref, 0, 0); idx++; count++; }
        bo += nend_entry(blk + bo, 0, 0);
        np32(blk + 24, 0x28); np32(blk + 28, bo - 0x18); np32(blk + 32, 4096 - 0x18); blk[36] = 0;
        unsigned short usn = 0x7A7A;
        np16(blk + 0x28, usn);
        for (unsigned i = 1; i <= 4096 / 512; i++) {
            unsigned char *end = blk + i * 512 - 2;
            blk[0x28 + 2 * i] = end[0]; blk[0x28 + 2 * i + 1] = end[1];
            np16(end, usn);
        }
        if (b < nblocks - 1) {   /* the next entry is the separator, with a pointer to this block */
            ro += nentry(rp + ro, &kids[idx], self_ref, 1, (unsigned long long)b);
            idx++;
        } else {
            ro += nend_entry(rp + ro, 1, (unsigned long long)b);
        }
    }
    if (idx != nk) { printf("index builder: %d of %d entries placed\n", idx, nk); exit(2); }
    np32(root, 0x30); np32(root + 4, 1); np32(root + 8, 4096); root[12] = 1;
    np32(root + 16, 16); np32(root + 20, 16 + ro); np32(root + 24, 16 + ro); root[28] = 1; /* has sub-nodes */
    nrec_res(d, 0x90, "$I30", root, 32 + ro);
    NRunSpec run = {(unsigned long long)nblocks, (long long)lcn};
    nrec_nonres(d, 0xA0, "$I30", &run, 1, 0, (unsigned long long)nblocks * 4096, (unsigned long long)nblocks * 4096,
                (unsigned long long)nblocks * 4096, 0);
    unsigned char bm[8] = {0};
    for (int b = 0; b < nblocks; b++) bm[b / 8] |= (unsigned char)(1u << (b % 8));
    nrec_res(d, 0xB0, "$I30", bm, 8);
}

/* the boot sector */
static void nboot(NImg *n, const char *label_unused)
{
    (void)label_unused;
    unsigned char *b = n->img;
    b[0] = 0xEB; b[1] = 0x52; b[2] = 0x90;
    memcpy(b + 3, "NTFS    ", 8);
    np16(b + 11, NT_SS); b[13] = NT_SPC;
    b[21] = 0xF8; np16(b + 24, 63); np16(b + 26, 255);
    np64(b + 40, NT_SECTORS - 1);
    np64(b + 48, NT_MFT_LCN);
    np64(b + 56, NT_CLUSTERS / 2);        /* $MFTMirr (not used) */
    b[64] = (unsigned char)(signed char)-10; /* 2^10 = 1024 bytes per record */
    b[68] = 1;                               /* one cluster per index block */
    np64(b + 72, 0x1122334455667788ULL);
    b[510] = 0x55; b[511] = 0xAA;
}

#endif
