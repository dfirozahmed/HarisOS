/* Host test for ntfs.c: reads an NTFS volume built by ntfsimg.h - resident and non-resident files, fragmented,
 * sparse and partly initialized data, an attribute list with extension records, DOS names, folders from a
 * resident index up to a two-level tree, the hidden system files - and refuses what is not NTFS.
 * Set NTFS_DUMP=file to write the image out, to check it with another reader (7-Zip: "7z l file"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "ntfs.h"
#include "ntfsimg.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int xrd(BlkDev *d, unsigned long long lba, unsigned n, void *buf)
{
    memcpy(buf, (unsigned char *)d->ctx + lba * 512, (size_t)n * 512);
    return 0;
}

static int add_blk(unsigned char *img, const char *name)
{
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, name);
    d.kind = BLK_RAM; d.parent = -1; d.sector_size = 512; d.sectors = NT_SECTORS; d.read = xrd; d.ctx = img;
    return blk_register(&d);
}

static unsigned char pat(unsigned seed, unsigned i) { return (unsigned char)((i * 29 + seed * 11 + (i >> 8) * 3) & 0xFF); }

static void fill(unsigned char *p, unsigned n, unsigned seed) { for (unsigned i = 0; i < n; i++) p[i] = pat(seed, i); }

/* ---- the volume ---- */
static NImg N;

static void nsystem(unsigned recno, const char *name)
{
    NRec r;
    nrec_begin(&r, recno, 1, 0);
    nstdinfo(&r, 0x6); /* hidden + system */
    NChild c = {name, recno, 0, 0, 1};
    nfilename_attrs(&r, 5ULL | (1ULL << 48), &c, 0);
    nrec_res(&r, 0x80, 0, 0, 0);
    nrec_finish(&N, &r);
}

/* a file record with a resident $DATA */
static void nfile_res(unsigned recno, const char *name, const unsigned char *data, unsigned len, unsigned long long parent, const char *dos)
{
    NRec r;
    nrec_begin(&r, recno, 1, 0);
    nstdinfo(&r, 0x20);
    NChild c = {name, recno, 0, len, 1};
    nfilename_attrs(&r, parent, &c, dos);
    nrec_res(&r, 0x80, 0, data, len);
    nrec_finish(&N, &r);
}

static unsigned long long REF(unsigned recno) { return (unsigned long long)recno | (1ULL << 48); }

/* the data of file `seed`, `size` bytes, stored in `runs` (clusters taken from the allocator; the caller picks the order) */
static void nfile_runs(unsigned recno, const char *name, unsigned long long size, unsigned long long init, const NRunSpec *runs, int nruns,
                       unsigned flags, unsigned long long parent)
{
    NRec r;
    nrec_begin(&r, recno, 1, 0);
    nstdinfo(&r, 0x20);
    NChild c = {name, recno, 0, size, 1};
    nfilename_attrs(&r, parent, &c, 0);
    unsigned long long clusters = 0;
    for (int i = 0; i < nruns; i++) clusters += runs[i].len;
    nrec_nonres(&r, 0x80, 0, runs, nruns, 0, clusters * NT_CS, size, init, flags);
    nrec_finish(&N, &r);
}

static unsigned char *DATA_OF[300]; /* what each record's content should be (for comparing) */
static unsigned DATA_LEN[300];

static void expect(unsigned recno, unsigned seed, unsigned len)
{
    DATA_OF[recno] = malloc(len ? len : 1);
    fill(DATA_OF[recno], len, seed);
    DATA_LEN[recno] = len;
}

static void put_data(const NRunSpec *runs, int nruns, unsigned recno, unsigned size)
{
    /* write DATA_OF[recno] into the clusters of the runs in order (holes are skipped) */
    unsigned long long off = 0;
    for (int i = 0; i < nruns; i++) {
        for (unsigned long long k = 0; k < runs[i].len; k++) {
            if (runs[i].lcn >= 0 && off < size) {
                unsigned long long n = size - off < NT_CS ? size - off : NT_CS;
                memcpy(ncl(&N, (unsigned)(runs[i].lcn + k)), DATA_OF[recno] + off, (size_t)n);
            }
            off += NT_CS;
        }
    }
}

static void build(void)
{
    memset(&N, 0, sizeof N);
    N.img = calloc(NT_SECTORS, NT_SS);
    N.mft = calloc(NT_MFT_RECORDS, NT_REC);
    nboot(&N, "NTTEST");
    unsigned mft_clusters = NT_MFT_RECORDS * NT_REC / NT_CS;
    nmark(&N, 0, NT_MFT_LCN + mft_clusters + 4);
    N.next_lcn = NT_MFT_LCN + mft_clusters + 4;

    /* $MFT (record 0) */
    NRec r;
    nrec_begin(&r, 0, 1, 0);
    nstdinfo(&r, 0x6);
    NChild mftc = {"$MFT", 0, 0, 0, 1};
    nfilename_attrs(&r, REF(5), &mftc, 0);
    NRunSpec mft_run = {mft_clusters, NT_MFT_LCN};
    nrec_nonres(&r, 0x80, 0, &mft_run, 1, 0, (unsigned long long)mft_clusters * NT_CS, (unsigned long long)NT_MFT_RECORDS * NT_REC,
                (unsigned long long)NT_MFT_RECORDS * NT_REC, 0);
    unsigned char mbm[16];
    memset(mbm, 0xFF, sizeof mbm);
    nrec_res(&r, 0xB0, 0, mbm, sizeof mbm);
    nrec_finish(&N, &r);

    for (unsigned i = 1; i < 24; i++) {
        static const char *const names[] = {"", "$MFTMirr", "$LogFile", "$Volume", "$AttrDef", ".", "$Bitmap", "$Boot", "$BadClus",
                                            "$Secure", "$UpCase", "$Extend"};
        if (i == 3 || i == 5 || i == 6) continue;
        char tmp[16];
        if (i < 12) strcpy(tmp, names[i]); else snprintf(tmp, sizeof tmp, "$Reserved%u", i);
        nsystem(i, tmp);
    }
    /* $Volume: the label */
    nrec_begin(&r, 3, 1, 0);
    nstdinfo(&r, 0x6);
    NChild volc = {"$Volume", 3, 0, 0, 1};
    nfilename_attrs(&r, REF(5), &volc, 0);
    unsigned char label[12];
    for (int i = 0; i < 6; i++) np16(label + 2 * i, (unsigned char)"NTTEST"[i]);
    nrec_res(&r, 0x60, 0, label, 12);
    unsigned char vi[12] = {0}; vi[8] = 3; vi[9] = 1;
    nrec_res(&r, 0x70, 0, vi, 12);
    nrec_res(&r, 0x80, 0, 0, 0);
    nrec_finish(&N, &r);

    /* ---- files in the root ---- */
    unsigned char buf[40000];
    /* 24: hello.txt, resident */
    fill(buf, 300, 1); nfile_res(24, "hello.txt", buf, 300, REF(5), 0); expect(24, 1, 300);
    /* 25: contig.bin, one run of 5 clusters, 20000 bytes */
    {
        unsigned lcn = nalloc(&N, 5);
        NRunSpec runs[1] = {{5, lcn}};
        expect(25, 2, 20000);
        put_data(runs, 1, 25, 20000);
        nfile_runs(25, "contig.bin", 20000, 20000, runs, 1, 0, REF(5));
    }
    /* 26: fragmented.bin: three runs, the second one BEFORE the first on the disk (a negative offset) */
    {
        unsigned a = nalloc(&N, 2), gap1 = nalloc(&N, 1), b = nalloc(&N, 3), gap2 = nalloc(&N, 1), c = nalloc(&N, 1);
        (void)gap1; (void)gap2;
        NRunSpec runs[3] = {{2, a + 40}, {3, b}, {1, c}};
        /* move run 0 up so that run 1 lies below it: copy the clusters accordingly */
        unsigned hi = nalloc(&N, 2);
        runs[0].lcn = hi;
        (void)a;
        unsigned size = 6 * NT_CS - 100;
        expect(26, 3, size);
        put_data(runs, 3, 26, size);
        nfile_runs(26, "fragmented.bin", size, size, runs, 3, 0, REF(5));
    }
    /* 27: sparse.bin: 2 clusters of data, a hole of 3, 1 more cluster of data */
    {
        unsigned a = nalloc(&N, 2), b = nalloc(&N, 1);
        NRunSpec runs[3] = {{2, a}, {3, -1}, {1, b}};
        unsigned size = 6 * NT_CS;
        DATA_OF[27] = calloc(size, 1);
        DATA_LEN[27] = size;
        for (unsigned i = 0; i < 2 * NT_CS; i++) DATA_OF[27][i] = pat(4, i);
        for (unsigned i = 5 * NT_CS; i < size; i++) DATA_OF[27][i] = pat(4, i);
        put_data(runs, 3, 27, size);
        nfile_runs(27, "sparse.bin", size, size, runs, 3, 0x8000, REF(5));
    }
    /* 28: partial-init.bin: 8000 bytes, but only the first 3000 are initialized */
    {
        unsigned a = nalloc(&N, 2);
        NRunSpec runs[1] = {{2, a}};
        DATA_OF[28] = calloc(8000, 1);
        DATA_LEN[28] = 8000;
        for (unsigned i = 0; i < 3000; i++) DATA_OF[28][i] = pat(5, i);
        memcpy(ncl(&N, a), DATA_OF[28], 3000);
        memset(ncl(&N, a) + 3000, 0xEE, 1000); /* old junk after the initialized size must not show */
        nfile_runs(28, "partial-init.bin", 8000, 3000, runs, 1, 0, REF(5));
    }
    /* 29: a long name with a DOS twin: two names in the index, one file in the list */
    fill(buf, 50, 6); nfile_res(29, "Long File Name.txt", buf, 50, REF(5), "LONGFI~1.TXT"); expect(29, 6, 50);
    /* 30: a name with a non-ASCII letter */
    fill(buf, 10, 7); nfile_res(30, "caf\x01.txt", buf, 10, REF(5), 0); expect(30, 7, 10);
    /* 31: empty file */
    nfile_res(31, "empty.txt", 0, 0, REF(5), 0); expect(31, 0, 0);
    /* 32: compressed file: unsupported */
    {
        unsigned a = nalloc(&N, 1);
        NRunSpec runs[1] = {{1, a}};
        nfile_runs(32, "compressed.dat", 3000, 3000, runs, 1, 0x0001, REF(5));
    }
    /* 33: attribute list; $DATA is in two extents in the extension records 34 and 35 */
    {
        unsigned a = nalloc(&N, 3), b = nalloc(&N, 3);
        NRunSpec e1[1] = {{3, a}}, e2[1] = {{3, b}};
        unsigned size = 6 * NT_CS - 7;
        expect(33, 8, size);
        NRunSpec both[2] = {{3, a}, {3, b}};
        put_data(both, 2, 33, size);
        NRec base, x1, x2;
        nrec_begin(&base, 33, 1, 0);
        nstdinfo(&base, 0x20);
        NChild c = {"attrlist.bin", 33, 0, size, 1};
        nfilename_attrs(&base, REF(5), &c, 0);
        unsigned char list[4 * 32];
        memset(list, 0, sizeof list);
        struct { unsigned type; unsigned vcn; unsigned rec; unsigned id; } L[4] = {{0x10, 0, 33, 0}, {0x30, 0, 33, 1}, {0x80, 0, 34, 0}, {0x80, 3, 35, 0}};
        for (int i = 0; i < 4; i++) {
            unsigned char *e = list + i * 32;
            np32(e, L[i].type); np16(e + 4, 32); e[6] = 0; e[7] = 26; np64(e + 8, L[i].vcn);
            np64(e + 16, REF(L[i].rec)); np16(e + 24, L[i].id);
        }
        nrec_res(&base, 0x20, 0, list, sizeof list);
        nrec_finish(&N, &base);
        nrec_begin(&x1, 34, 1, REF(33));
        nrec_nonres(&x1, 0x80, 0, e1, 1, 0, 6ULL * NT_CS, size, size, 0);
        nrec_finish(&N, &x1);
        nrec_begin(&x2, 35, 1, REF(33));
        nrec_nonres(&x2, 0x80, 0, e2, 1, 3, 0, 0, 0, 0);
        nrec_finish(&N, &x2);
    }
    /* the attribute ids in the base record: SI = 0, FILE_NAME = 1; the extension records' DATA has id 0 */

    /* ---- Sub (small, resident index) ---- */
    {
        NChild kids[2];
        NRec d;
        nrec_begin(&d, 36, 3, 0);
        nstdinfo(&d, 0x10);
        NChild c = {"Sub", 36, 1, 0, 1};
        nfilename_attrs(&d, REF(5), &c, 0);
        fill(buf, 400, 9); nfile_res(37, "inner.txt", buf, 400, REF(36), 0); expect(37, 9, 400);
        kids[0] = (NChild){"inner.txt", 37, 0, 400, 1};
        kids[1] = (NChild){"deeper", 38, 1, 0, 1};
        nindex(&N, &d, kids, 2, REF(36), 0);
        nrec_finish(&N, &d);
        NRec d2;
        nrec_begin(&d2, 38, 3, 0);
        nstdinfo(&d2, 0x10);
        NChild c2 = {"deeper", 38, 1, 0, 1};
        nfilename_attrs(&d2, REF(36), &c2, 0);
        fill(buf, 5000, 10); expect(39, 10, 5000);
        {
            unsigned lcn = nalloc(&N, 2);
            NRunSpec runs[1] = {{2, lcn}};
            put_data(runs, 1, 39, 5000);
            nfile_runs(39, "bottom.txt", 5000, 5000, runs, 1, 0, REF(38));
        }
        NChild k2[1] = {{"bottom.txt", 39, 0, 5000, 1}};
        nindex(&N, &d2, k2, 1, REF(38), 0);
        nrec_finish(&N, &d2);
    }
    /* ---- Big: 150 files, an index tree ---- */
    {
        static NChild kids[150];
        static char names[150][64];
        NRec d;
        nrec_begin(&d, 40, 3, 0);
        nstdinfo(&d, 0x10);
        NChild c = {"Big", 40, 1, 0, 1};
        nfilename_attrs(&d, REF(5), &c, 0);
        for (int i = 0; i < 150; i++) {
            snprintf(names[i], sizeof names[i], "File%03d.txt", i);
            unsigned rec = 41 + (unsigned)i;
            nfile_res(rec, names[i], 0, 0, REF(40), 0);
            kids[i] = (NChild){names[i], rec, 0, 0, 1};
        }
        nindex(&N, &d, kids, 150, REF(40), 0);
        nrec_finish(&N, &d);
    }

    /* ---- the root folder: the files above, Sub, Big, and entries for the system files (they must stay hidden) ---- */
    {
        static NChild kids[40];
        int nk = 0;
        kids[nk++] = (NChild){"hello.txt", 24, 0, 300, 1};
        kids[nk++] = (NChild){"contig.bin", 25, 0, 20000, 1};
        kids[nk++] = (NChild){"fragmented.bin", 26, 0, 6 * NT_CS - 100, 1};
        kids[nk++] = (NChild){"sparse.bin", 27, 0, 6 * NT_CS, 1};
        kids[nk++] = (NChild){"partial-init.bin", 28, 0, 8000, 1};
        kids[nk++] = (NChild){"Long File Name.txt", 29, 0, 50, 1};
        kids[nk++] = (NChild){"LONGFI~1.TXT", 29, 0, 50, 2};
        kids[nk++] = (NChild){"caf\x01.txt", 30, 0, 10, 1};
        kids[nk++] = (NChild){"empty.txt", 31, 0, 0, 1};
        kids[nk++] = (NChild){"compressed.dat", 32, 0, 3000, 1};
        kids[nk++] = (NChild){"attrlist.bin", 33, 0, 6 * NT_CS - 7, 1};
        kids[nk++] = (NChild){"Sub", 36, 1, 0, 1};
        kids[nk++] = (NChild){"Big", 40, 1, 0, 1};
        kids[nk++] = (NChild){"$MFT", 0, 0, 0, 1};
        kids[nk++] = (NChild){"$Volume", 3, 0, 0, 1};
        kids[nk++] = (NChild){"$Bitmap", 6, 0, 0, 1};
        kids[nk++] = (NChild){"$Boot", 7, 0, 0, 1};
        kids[nk++] = (NChild){"$UpCase", 10, 0, 0, 1};
        kids[nk++] = (NChild){"$Extend", 11, 1, 0, 1};
        NRec d;
        nrec_begin(&d, 5, 3, 0);
        nstdinfo(&d, 0x6 | 0x10);
        NChild c = {".", 5, 1, 0, 1};
        nfilename_attrs(&d, REF(5), &c, 0);
        nindex(&N, &d, kids, nk, REF(5), 0);
        nrec_finish(&N, &d);
    }
    /* $Bitmap (record 6): the cluster bitmap of the volume, in its own clusters */
    {
        unsigned lcn = nalloc(&N, 1);
        NRec bm;
        nrec_begin(&bm, 6, 1, 0);
        nstdinfo(&bm, 0x6);
        NChild c = {"$Bitmap", 6, 0, 0, 1};
        nfilename_attrs(&bm, REF(5), &c, 0);
        NRunSpec run = {1, lcn};
        nrec_nonres(&bm, 0x80, 0, &run, 1, 0, NT_CS, NT_CLUSTERS / 8, NT_CLUSTERS / 8, 0);
        nrec_finish(&N, &bm);
        memcpy(ncl(&N, lcn), N.bitmap, sizeof N.bitmap); /* all allocations are done: write it */
    }
    memcpy(N.img + (size_t)NT_MFT_LCN * NT_CS, N.mft, (size_t)NT_MFT_RECORDS * NT_REC);
    memcpy(N.img + (size_t)(NT_SECTORS - 1) * NT_SS, N.img, NT_SS); /* backup boot sector */
}

/* ---- listing helpers ---- */
typedef struct { char name[400][NTFS_NAME_MAX]; NtEntry e[400]; int n; } Listing;
static int lcb(const char *name, const NtEntry *e, void *user)
{
    Listing *l = (Listing *)user;
    if (l->n < 400) { strcpy(l->name[l->n], name); l->e[l->n] = *e; l->n++; }
    return 0;
}
static Ntfs F;
static int STOP_COUNT;
static int cb_stop(const char *n, const NtEntry *x, void *u) { (void)n; (void)x; (void)u; return ++STOP_COUNT == 3; }
static Listing L;
static int find(const NtEntry *dir, const char *name, NtEntry *out)
{
    L.n = 0;
    if (ntfs_list(&F, dir, lcb, &L) < 0) return 0;
    for (int i = 0; i < L.n; i++) if (strcmp(L.name[i], name) == 0) { *out = L.e[i]; return 1; }
    return 0;
}

static int same(const NtEntry *e, unsigned recno, unsigned chunk)
{
    unsigned len = DATA_LEN[recno];
    unsigned char *b = malloc(len + chunk + 1);
    unsigned got = 0;
    int ok = 1;
    while (got < len) {
        int r = ntfs_read(&F, e, got, b + got, chunk);
        if (r <= 0) { ok = 0; break; }
        got += (unsigned)r;
    }
    if (ok && len && memcmp(b, DATA_OF[recno], len) != 0) ok = 0;
    if (ok && ntfs_read(&F, e, len, b, 10) != 0) ok = 0;   /* nothing after the end */
    free(b);
    return ok;
}

int main(void)
{
    build();
    const char *dump = getenv("NTFS_DUMP");
    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (f) { fwrite(N.img, 1, (size_t)NT_SECTORS * NT_SS, f); fclose(f); printf("wrote %s\n", dump); }
    }
    const char *expdir = getenv("NTFS_EXPECT");   /* write what each file should contain, to compare with another reader */
    if (expdir) {
        static const struct { unsigned rec; const char *name; } X[] = {{24, "hello.txt"}, {25, "contig.bin"}, {26, "fragmented.bin"}, {27, "sparse.bin"}, {28, "partial-init.bin"}, {29, "Long File Name.txt"}, {33, "attrlist.bin"}, {37, "inner.txt"}, {39, "bottom.txt"}};
        for (unsigned i = 0; i < sizeof X / sizeof X[0]; i++) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s", expdir, X[i].name);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(DATA_OF[X[i].rec], 1, DATA_LEN[X[i].rec], f); fclose(f); }
        }
    }
    blk_reset();
    int dev = add_blk(N.img, "ntdisk0");
    CHECK(ntfs_mount(&F, dev) == 0);
    CHECK(strcmp(F.label, "NTTEST") == 0 && F.csize == 4096 && F.rec_size == 1024 && F.idx_size == 4096);

    NtEntry root, e;
    ntfs_root(&root);
    L.n = 0;
    int total = ntfs_list(&F, &root, lcb, &L);
    CHECK(total == 12 && L.n == 12);   /* 10 files (the DOS twin and the system files are hidden) + Sub + Big */
    for (int i = 0; i < L.n; i++) CHECK(L.name[i][0] != '$' && strcmp(L.name[i], "LONGFI~1.TXT") != 0);

    CHECK(find(&root, "hello.txt", &e) && !e.is_dir && e.size == 300 && same(&e, 24, 100) && same(&e, 24, 4096));
    CHECK(find(&root, "contig.bin", &e) && same(&e, 25, 777) && same(&e, 25, 8192) && same(&e, 25, 1));
    CHECK(find(&root, "fragmented.bin", &e) && same(&e, 26, 1000) && same(&e, 26, 5000) && same(&e, 26, 40000));
    CHECK(find(&root, "sparse.bin", &e) && same(&e, 27, 3000) && same(&e, 27, 20000));
    CHECK(find(&root, "partial-init.bin", &e) && same(&e, 28, 999) && same(&e, 28, 8000));
    CHECK(find(&root, "Long File Name.txt", &e) && same(&e, 29, 50));
    CHECK(find(&root, "caf_.txt", &e) && same(&e, 30, 10));            /* U+00E9 shows as '_' */
    CHECK(find(&root, "empty.txt", &e) && e.size == 0 && ntfs_read(&F, &e, 0, &e.pad, 1) == 0);
    CHECK(find(&root, "compressed.dat", &e) && ntfs_read(&F, &e, 0, &e.pad, 1) == -2);
    CHECK(find(&root, "attrlist.bin", &e) && same(&e, 33, 1500) && same(&e, 33, 10000));

    /* reads at the edges of a non-resident file */
    {
        NtEntry c;
        CHECK(find(&root, "contig.bin", &c));
        unsigned char b[300];
        CHECK(ntfs_read(&F, &c, 20000, b, 10) == 0 && ntfs_read(&F, &c, 999999, b, 10) == 0);
        CHECK(ntfs_read(&F, &c, 19995, b, 100) == 5 && memcmp(b, DATA_OF[25] + 19995, 5) == 0);
        CHECK(ntfs_read(&F, &c, 4095, b, 2) == 2 && memcmp(b, DATA_OF[25] + 4095, 2) == 0);   /* across a cluster */
        CHECK(ntfs_read(&F, &c, 511, b, 2) == 2 && memcmp(b, DATA_OF[25] + 511, 2) == 0);     /* across a sector */
        CHECK(ntfs_read(&F, &c, 0, b, 0) == 0);
    }

    /* folders */
    NtEntry sub, deeper, big;
    CHECK(find(&root, "Sub", &sub) && sub.is_dir && find(&root, "Big", &big) && big.is_dir);
    L.n = 0;
    CHECK(ntfs_list(&F, &sub, lcb, &L) == 2);
    CHECK(find(&sub, "inner.txt", &e) && same(&e, 37, 300));
    CHECK(find(&sub, "deeper", &deeper) && deeper.is_dir);
    CHECK(find(&deeper, "bottom.txt", &e) && same(&e, 39, 1234));
    L.n = 0;
    CHECK(ntfs_list(&F, &big, lcb, &L) == 150 && L.n == 150);          /* an index tree: separators + 8 leaf blocks */
    {
        int seen[150] = {0};
        for (int i = 0; i < L.n; i++) {
            int k;
            if (sscanf(L.name[i], "File%d.txt", &k) == 1 && k >= 0 && k < 150) seen[k]++;
        }
        int all = 1;
        for (int i = 0; i < 150; i++) if (seen[i] != 1) all = 0;
        CHECK(all);                                                      /* every entry exactly once */
    }
    CHECK(ntfs_list(&F, &(NtEntry){24, 0, 0, {0}}, lcb, &L) == -1);     /* a file is not a folder */
    CHECK(ntfs_list(&F, &(NtEntry){200, 0, 1, {0}}, lcb, &L) == -1);    /* an unused record */

    /* stop early */
    STOP_COUNT = 0;
    CHECK(ntfs_list(&F, &root, cb_stop, 0) == 3 && STOP_COUNT == 3);

    /* space: the $Bitmap says how many clusters are used */
    unsigned long long tot, fr;
    CHECK(ntfs_space(&F, &tot, &fr) == 1 && tot == (unsigned long long)(NT_CLUSTERS - 1) * NT_CS); /* the last sector is the backup boot sector */
    unsigned used = 0;
    for (unsigned i = 0; i < NT_CLUSTERS - 1; i++) used += (N.bitmap[i / 8] >> (i % 8)) & 1;
    CHECK(fr == (unsigned long long)(NT_CLUSTERS - 1 - used) * NT_CS);

    /* damage: things that must be refused or noticed */
    {
        unsigned char *copy = malloc((size_t)NT_SECTORS * NT_SS);
        memcpy(copy, N.img, (size_t)NT_SECTORS * NT_SS);
        int di = add_blk(copy, "ntdisk1");
        Ntfs g;
        CHECK(ntfs_mount(&g, di) == 0);
        copy[3] = 'X'; CHECK(ntfs_mount(&g, di) == -1); copy[3] = 'N';                 /* name */
        copy[510] = 0; CHECK(ntfs_mount(&g, di) == -1); copy[510] = 0x55;
        copy[13] = 3; CHECK(ntfs_mount(&g, di) == -1); copy[13] = NT_SPC;              /* clusters not a power of two */
        copy[64] = 3; CHECK(ntfs_mount(&g, di) == -1); copy[64] = (unsigned char)(signed char)-10; /* 3 clusters per record */
        np64(copy + 48, 999999); CHECK(ntfs_mount(&g, di) == -1); np64(copy + 48, NT_MFT_LCN);   /* MFT out of the volume */
        np64(copy + 40, 1ULL << 40); CHECK(ntfs_mount(&g, di) == -1); np64(copy + 40, NT_SECTORS - 1);
        CHECK(ntfs_mount(&g, di) == 0);
        /* a torn MFT record 0 (fixup mismatch) cannot be used to mount */
        copy[(size_t)NT_MFT_LCN * NT_CS + 510] ^= 0xFF;
        CHECK(ntfs_mount(&g, di) == -1);
        copy[(size_t)NT_MFT_LCN * NT_CS + 510] ^= 0xFF;
        CHECK(ntfs_mount(&g, di) == 0);
        /* FAT32 / exFAT-looking sectors are refused */
        memcpy(copy + 3, "EXFAT   ", 8); CHECK(ntfs_mount(&g, di) == -1);
        free(copy);
    }

    for (int i = 0; i < 300; i++) free(DATA_OF[i]);
    free(N.img); free(N.mft);
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("ntfs tests passed\n");
    return 0;
}
