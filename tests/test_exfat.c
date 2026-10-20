/* Host test for exfat.c: reads exFAT volumes built by exfatimg.h (contiguous and chained files, nested and
 * multi-cluster folders, long names, ValidDataLength, the allocation bitmap) and refuses things that are not exFAT. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "exfat.h"
#include "exfatimg.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int xrd(BlkDev *d, unsigned long long lba, unsigned n, void *buf)
{
    memcpy(buf, (unsigned char *)d->ctx + lba * 512, (size_t)n * 512);
    return 0;
}

static int add_blk(unsigned char *img, unsigned sectors, const char *name)
{
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, name);
    d.kind = BLK_RAM; d.parent = -1; d.sector_size = 512; d.sectors = sectors; d.read = xrd; d.ctx = img;
    return blk_register(&d);
}

typedef struct { char name[64][EXFAT_NAME_MAX]; ExEntry e[64]; int n; } Listing;

static int lcb(const char *name, const ExEntry *e, void *user)
{
    Listing *l = (Listing *)user;
    if (l->n < 64) { strcpy(l->name[l->n], name); l->e[l->n] = *e; l->n++; }
    return 0;
}

static ExFat F;
static Listing L;

static int find(const ExEntry *dir, const char *name, ExEntry *out)
{
    L.n = 0;
    if (exfat_list(&F, dir, lcb, &L) < 0) return 0;
    for (int i = 0; i < L.n; i++) if (strcmp(L.name[i], name) == 0) { *out = L.e[i]; return 1; }
    return 0;
}

static unsigned char pat(unsigned seed, unsigned i) { return (unsigned char)((i * 31 + seed * 17 + (i >> 9)) & 0xFF); }

static unsigned char *make_data(unsigned size, unsigned seed)
{
    unsigned char *d = malloc(size ? size : 1);
    for (unsigned i = 0; i < size; i++) d[i] = pat(seed, i);
    return d;
}

static int content_ok(const ExEntry *e, unsigned size, unsigned seed, unsigned chunk)
{
    if (e->size != size) return 0;
    unsigned char *b = malloc(size + chunk), *want = make_data(size, seed);
    unsigned got = 0;
    int ok = 1;
    while (got < size) {
        int r = exfat_read(&F, e, got, b + got, chunk);
        if (r <= 0) { ok = 0; break; }
        got += (unsigned)r;
    }
    if (ok && memcmp(b, want, size) != 0) ok = 0;
    free(b); free(want);
    return ok;
}

int main(void)
{
    XImg x;
    xformat(&x, 32768, 2, "EXTEST");
    XDir root, docs, many, big;
    xdir_root(&x, &root);

    unsigned char *d1 = make_data(100, 1), *d2 = make_data(9000, 2), *d3 = make_data(20000, 3), *d4 = make_data(5000, 4);
    xfile_contig(&x, &root, "small.txt", d1, 100);                /* one cluster, NoFatChain */
    xfile_contig(&x, &root, "Medium File.bin", d2, 9000);         /* five clusters in a row */
    xfile_chained(&x, &root, "fragmented.dat", d3, 20000);        /* ten clusters with gaps: a real FAT chain */
    xadd(&x, &root, "empty.txt", 0x20, 0, 0, 0, 0);               /* no cluster at all */
    {   /* ValidDataLength shorter than DataLength: the end reads as zeros */
        unsigned c = xalloc(&x);
        memcpy(xclus(&x, c), d4, 2048);
        xfat_set(&x, c, 0xFFFFFFFF);
        xadd(&x, &root, "sparse-end.bin", 0x20, c, 2048, 1000, 0);
    }
    char longname[256];
    for (int i = 0; i < 200; i++) longname[i] = (char)('a' + i % 26);
    strcpy(longname + 196, ".txt");                               /* 200 characters */
    xfile_contig(&x, &root, longname, d1, 100);
    xfile_contig(&x, &root, "caf\xc3\xa9.txt", d1, 10);           /* UTF-8 bytes are not UTF-16: stay ASCII here */
    {   /* a real non-ASCII character: write the entry set by hand with U+00E9 */
        XDir tmp = root;
        xadd(&x, &tmp, "cafX.txt", 0x20, 0, 0, 0, 0);
        root = tmp;
        unsigned char *slot = xdir_slot(&x, &root, root.entries - 1); /* the last name entry of "cafX.txt" */
        xp16(slot + 2 + 2 * 3, 0x00E9);                           /* 'X' -> e acute */
    }

    xmkdir(&x, &root, "Docs", &docs, 0, 1);                       /* a folder in a FAT chain */
    xfile_contig(&x, &docs, "notes.txt", d2, 3000);
    xmkdir(&x, &docs, "deep", &many, 1, 3);                       /* a contiguous (NoFatChain) folder of 3 clusters */
    xfile_chained(&x, &many, "bottom.bin", d3, 4097);
    for (int i = 0; i < 150; i++) {                               /* a folder that needs several clusters */
        char nm[64];
        snprintf(nm, sizeof nm, "Entry number %03d with a long-ish name.txt", i);
        if (i == 0) xmkdir(&x, &root, "Big", &big, 0, 1);
        xadd(&x, &big, nm, 0x20, 0, 0, 0, 0);
    }
    xfinish(&x);

    const char *dump = getenv("EXFAT_DUMP"); /* write the image out, to try it elsewhere */
    if (dump) {
        FILE *df = fopen(dump, "wb");
        if (df) { fwrite(x.img, 512, 32768, df); fclose(df); puts("image written"); }
    }
    int dev = add_blk(x.img, 32768, "xdisk0");
    CHECK(exfat_mount(&F, dev) == 0);
    CHECK(strcmp(F.label, "EXTEST") == 0 && F.clusters == x.clusters && F.root_cluster == 4);

    ExEntry r, e;
    exfat_root(&F, &r);
    CHECK(exfat_list(&F, &r, lcb, &L) == (L.n = 0, exfat_list(&F, &r, lcb, &L)));
    L.n = 0;
    int total = exfat_list(&F, &r, lcb, &L);
    CHECK(total == 10 && L.n == 10);                              /* 8 files + Docs + Big */

    CHECK(find(&r, "small.txt", &e) && !(e.attr & EXFAT_ATTR_DIR) && e.nofat && e.size == 100);
    CHECK(content_ok(&e, 100, 1, 64));
    CHECK(find(&r, "Medium File.bin", &e) && content_ok(&e, 9000, 2, 777) && content_ok(&e, 9000, 2, 4096));
    CHECK(find(&r, "fragmented.dat", &e) && !e.nofat && content_ok(&e, 20000, 3, 1000) && content_ok(&e, 20000, 3, 2048));
    CHECK(find(&r, "empty.txt", &e) && e.size == 0 && e.first_cluster == 0 && exfat_read(&F, &e, 0, &e.secondary, 1) == 0);
    CHECK(find(&r, longname, &e) && e.size == 100 && content_ok(&e, 100, 1, 100));
    CHECK(find(&r, "caf_.txt", &e));                              /* U+00E9 shows as '_' */
    CHECK(find(&r, "Docs", &e) && (e.attr & EXFAT_ATTR_DIR));

    /* ValidDataLength */
    CHECK(find(&r, "sparse-end.bin", &e) && e.size == 2048 && e.valid == 1000);
    {
        unsigned char b[2048];
        CHECK(exfat_read(&F, &e, 0, b, 2048) == 2048);
        int ok = 1;
        for (unsigned i = 0; i < 2048; i++) if (b[i] != (i < 1000 ? pat(4, i) : 0)) ok = 0;
        CHECK(ok);
    }

    /* reads at the edges */
    {
        ExEntry m;
        CHECK(find(&r, "Medium File.bin", &m));
        unsigned char b[5000], *want = make_data(9000, 2);
        CHECK(exfat_read(&F, &m, 9000, b, 10) == 0 && exfat_read(&F, &m, 100000, b, 10) == 0);
        CHECK(exfat_read(&F, &m, 8990, b, 100) == 10 && memcmp(b, want + 8990, 10) == 0);
        CHECK(exfat_read(&F, &m, 2047, b, 3) == 3 && memcmp(b, want + 2047, 3) == 0);      /* across a cluster boundary */
        CHECK(exfat_read(&F, &m, 511, b, 2) == 2 && memcmp(b, want + 511, 2) == 0);        /* across a sector boundary */
        CHECK(exfat_read(&F, &m, 1000, b, 5000) == 5000 && memcmp(b, want + 1000, 5000) == 0);
        CHECK(exfat_read(&F, &m, 0, b, 0) == 0);
        free(want);
    }

    /* folders: chained, nested contiguous, and a big one over several clusters */
    ExEntry docs_e, deep, big_e;
    CHECK(find(&r, "Docs", &docs_e));
    CHECK(find(&docs_e, "notes.txt", &e) && content_ok(&e, 3000, 2, 500));
    CHECK(find(&docs_e, "deep", &deep) && deep.nofat && (deep.attr & EXFAT_ATTR_DIR));
    CHECK(find(&deep, "bottom.bin", &e) && content_ok(&e, 4097, 3, 1000));
    CHECK(find(&r, "Big", &big_e));
    L.n = 0;
    {
        static Listing big_l;
        big_l.n = 0;
        int cnt = 0;
        /* count through a callback that only counts: the listing buffer holds 64 */
        cnt = exfat_list(&F, &big_e, lcb, &big_l);
        if (cnt != 150) printf("  big folder listed %d entries", cnt);
        CHECK(cnt == 150);
    }
    CHECK(find(&big_e, "Entry number 000 with a long-ish name.txt", &e) == 1);
    L.n = 0;
    {
        int stop_after = 0;
        (void)stop_after;
    }

    /* space: the bitmap says how many clusters are used */
    unsigned long long tot, fr;
    CHECK(exfat_space(&F, &tot, &fr) == 1 && tot == (unsigned long long)x.clusters * 2048);
    unsigned used = 0;
    for (unsigned i = 0; i < x.clusters; i++) used += (x.bitmap[i / 8] >> (i % 8)) & 1;
    CHECK(fr == (unsigned long long)(x.clusters - used) * 2048);

    /* things that are not exFAT are refused */
    {
        unsigned char *copy = malloc(32768 * 512);
        memcpy(copy, x.img, 32768 * 512);
        int di = add_blk(copy, 32768, "xdisk1");
        ExFat g;
        CHECK(exfat_mount(&g, di) == 0);
        copy[3] = 'N'; CHECK(exfat_mount(&g, di) == -1); copy[3] = 'E';       /* name */
        copy[20] = 1; CHECK(exfat_mount(&g, di) == -1); copy[20] = 0;         /* "must be zero" */
        copy[108] = 10; CHECK(exfat_mount(&g, di) == -1); copy[108] = 9;      /* sector size does not match the disk */
        copy[510] = 0; CHECK(exfat_mount(&g, di) == -1); copy[510] = 0x55;
        copy[105] = 2; CHECK(exfat_mount(&g, di) == -1); copy[105] = 1;       /* revision 2 */
        xp32(copy + 96, 0); CHECK(exfat_mount(&g, di) == -1);                 /* root cluster out of range */
        xp32(copy + 96, 4);
        xp64(copy + 72, 100000); CHECK(exfat_mount(&g, di) == -1);            /* longer than the disk */
        free(copy);
    }

    free(d1); free(d2); free(d3); free(d4);
    free(x.img); free(x.bitmap);
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("exfat tests passed\n");
    return 0;
}
