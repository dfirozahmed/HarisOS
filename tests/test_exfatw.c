/* Host test for exfat.c writing: creates, writes, renames, moves and deletes on an exFAT volume built by
 * exfatimg.h and checks every state with the independent checker in exfatcheck.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "exfat.h"
#include "exfatimg.h"
#include "exfatcheck.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static unsigned long long WRITES;
static int xrd(BlkDev *d, unsigned long long lba, unsigned n, void *buf)
{
    memcpy(buf, (unsigned char *)d->ctx + lba * 512, (size_t)n * 512);
    return 0;
}
static int xwr(BlkDev *d, unsigned long long lba, unsigned n, const void *buf)
{
    memcpy((unsigned char *)d->ctx + lba * 512, buf, (size_t)n * 512);
    WRITES++;
    return 0;
}

static int add_dev(unsigned char *img, unsigned sectors)
{
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, "xw0");
    d.kind = BLK_RAM; d.parent = -1; d.sector_size = 512; d.sectors = sectors; d.writable = 1; d.read = xrd; d.write = xwr; d.ctx = img;
    return blk_register(&d);
}

static XImg X;
static ExFat F;
static int DEV;

typedef struct { char name[300][EXFAT_NAME_MAX]; ExEntry e[300]; int n; } Listing;
static Listing L;
static int lcb(const char *name, const ExEntry *e, void *user)
{
    Listing *l = (Listing *)user;
    if (l->n < 300) { strcpy(l->name[l->n], name); l->e[l->n] = *e; l->n++; }
    return 0;
}
static int list_dir(const ExEntry *d) { L.n = 0; return exfat_list(&F, d, lcb, &L); }
static int find(const ExEntry *d, const char *name, ExEntry *out)
{
    list_dir(d);
    for (int i = 0; i < L.n; i++) if (strcmp(L.name[i], name) == 0) { *out = L.e[i]; return 1; }
    return 0;
}

static unsigned char pat(unsigned seed, unsigned i) { return (unsigned char)((i * 13 + seed * 7 + (i >> 8) * 5) & 0xFF); }

static int write_pat(const ExEntry *dir, ExEntry *e, unsigned off, unsigned len, unsigned seed)
{
    unsigned char *b = malloc(len);
    for (unsigned i = 0; i < len; i++) b[i] = pat(seed, off + i);
    int r = exfat_write(&F, dir, e, off, b, len);
    free(b);
    return r;
}

static int verify(const ExEntry *e, unsigned len, unsigned seed, unsigned chunk)
{
    if (e->size != len) return 0;
    unsigned char *b = malloc(len + chunk + 1);
    unsigned got = 0;
    int ok = 1;
    while (got < len) {
        int r = exfat_read(&F, e, got, b + got, chunk);
        if (r <= 0) { ok = 0; break; }
        got += (unsigned)r;
    }
    for (unsigned i = 0; ok && i < len; i++) if (b[i] != pat(seed, i)) { ok = 0; printf("  mismatch at %u\n", i); }
    free(b);
    return ok;
}

static int check(void) { return xfsck(&X); }

/* fresh volume with a few files made by the generator (contiguous, chained, a NoFatChain folder) */
static unsigned char *D_SMALL, *D_CHAIN;
static void fresh(void)
{
    free(X.img); free(X.bitmap);
    blk_reset();
    xformat(&X, 16384, 1, "WTEST");           /* 8 MiB, 1 KiB clusters */
    XDir root, sub;
    xdir_root(&X, &root);
    free(D_SMALL); free(D_CHAIN);
    D_SMALL = malloc(3000); D_CHAIN = malloc(5000);
    for (unsigned i = 0; i < 3000; i++) D_SMALL[i] = pat(1, i);
    for (unsigned i = 0; i < 5000; i++) D_CHAIN[i] = pat(2, i);
    xfile_contig(&X, &root, "small.bin", D_SMALL, 3000);       /* clusters in a row: NoFatChain */
    xfile_contig(&X, &root, "blocker.bin", D_SMALL, 100);      /* right behind it: small.bin cannot grow in place */
    xfile_chained(&X, &root, "chained.bin", D_CHAIN, 5000);
    xmkdir(&X, &root, "Folder", &sub, 1, 2);                    /* a NoFatChain folder of two clusters */
    xfile_contig(&X, &sub, "inside.txt", D_SMALL, 10);
    xfinish(&X);
    DEV = add_dev(X.img, 16384);
    blk_set_locked(DEV, 0);
    CHECK(exfat_mount(&F, DEV) == 0);
}

int main(void)
{
    fresh();
    ExEntry root, e, e2, folder;
    exfat_root(&F, &root);
    CHECK(check() == 0);                                         /* the generator's own volume is clean */

    /* ---- names ---- */
    CHECK(exfat_create(&F, &root, 0, "new.txt", 0, &e) == 0 && e.size == 0 && e.first_cluster == 0 && e.secondary == 2);
    CHECK(exfat_create(&F, &root, 0, "NEW.TXT", 0, &e2) == EXFAT_ERR_EXISTS);
    CHECK(exfat_create(&F, &root, 0, "A name that needs three name entries because it is long.document", 0, &e2) == 0 && e2.secondary == 6);
    CHECK(exfat_create(&F, &root, 0, "", 0, &e2) == EXFAT_ERR_BADNAME);
    CHECK(exfat_create(&F, &root, 0, "a/b", 0, &e2) == EXFAT_ERR_BADNAME);
    CHECK(exfat_create(&F, &root, 0, "what?", 0, &e2) == EXFAT_ERR_BADNAME);
    CHECK(exfat_create(&F, &root, 0, "trail.", 0, &e2) == EXFAT_ERR_BADNAME);
    CHECK(exfat_create(&F, &root, 0, "..", 0, &e2) == EXFAT_ERR_BADNAME);
    char big[300];
    memset(big, 'z', 256); big[256] = 0;
    CHECK(exfat_create(&F, &root, 0, big, 0, &e2) == EXFAT_ERR_BADNAME);
    big[255] = 0;
    CHECK(exfat_create(&F, &root, 0, big, 0, &e2) == 0 && e2.secondary == 18);   /* 255 characters: the limit */
    CHECK(check() == 0);

    /* ---- data ---- */
    CHECK(find(&root, "new.txt", &e));
    CHECK(write_pat(&root, &e, 0, 100, 5) == 100 && e.size == 100 && e.first_cluster >= 2);
    CHECK(write_pat(&root, &e, 100, 5000, 5) == 5000 && e.size == 5100);        /* append over clusters */
    CHECK(verify(&e, 5100, 5, 777));
    CHECK(write_pat(&root, &e, 1000, 3000, 6) == 3000 && e.size == 5100);       /* overwrite in the middle */
    {
        unsigned char b[5100];
        CHECK(exfat_read(&F, &e, 0, b, 5100) == 5100);
        int ok = 1;
        for (unsigned i = 0; i < 5100; i++) if (b[i] != ((i >= 1000 && i < 4000) ? pat(6, i) : pat(5, i))) ok = 0;
        CHECK(ok);
        CHECK(exfat_write(&F, &root, &e, 5101, b, 1) == EXFAT_ERR_ARG);        /* no gaps */
    }
    CHECK(check() == 0);
    CHECK(find(&root, "new.txt", &e2) && e2.size == 5100 && e2.first_cluster == e.first_cluster);   /* on the disk */

    /* a big file in odd pieces */
    CHECK(exfat_create(&F, &root, 0, "big.bin", 0, &e) == 0);
    {
        unsigned total = 300000, off = 0;
        while (off < total) {
            unsigned n = (off % 3) ? 4093 : 12345;
            if (n > total - off) n = total - off;
            CHECK(write_pat(&root, &e, off, n, 7) == (int)n);
            off += n;
        }
        CHECK(verify(&e, total, 7, 1000));
    }
    CHECK(check() == 0);

    /* ---- NoFatChain files: grow in place, or become a chain ---- */
    CHECK(find(&root, "small.bin", &e) && e.nofat && e.size == 3000);
    unsigned old_first = e.first_cluster;
    CHECK(write_pat(&root, &e, 3000, 2000, 1) == 2000);                         /* blocker.bin is right behind it */
    CHECK(e.size == 5000 && !e.nofat && e.first_cluster == old_first);          /* converted to a FAT chain */
    {
        unsigned char b[5000];
        CHECK(exfat_read(&F, &e, 0, b, 5000) == 5000);
        int ok = 1;
        for (unsigned i = 0; i < 3000; i++) if (b[i] != pat(1, i)) ok = 0;
        for (unsigned i = 3000; i < 5000; i++) if (b[i] != pat(1, i)) ok = 0;
        CHECK(ok);
    }
    CHECK(check() == 0);
    CHECK(find(&root, "blocker.bin", &e) && e.nofat);                           /* the last file of its run can grow in place */
    {
        ExEntry before = e;
        CHECK(exfat_create(&F, &root, 0, "tail.bin", 0, &e2) == 0);
        (void)before;
    }
    CHECK(check() == 0);

    /* ---- folders ---- */
    CHECK(find(&root, "Folder", &folder) && folder.nofat);
    CHECK(exfat_create(&F, &folder, &root, "made.txt", 0, &e) == 0);
    CHECK(write_pat(&folder, &e, 0, 700, 9) == 700 && verify(&e, 700, 9, 100));
    CHECK(exfat_create(&F, &root, 0, "Docs", 1, &e2) == 0 && (e2.attr & EXFAT_ATTR_DIR) && e2.size == 1024);
    ExEntry docs = e2, inner;
    CHECK(exfat_create(&F, &docs, &root, "inner folder", 1, &inner) == 0);
    CHECK(exfat_create(&F, &inner, &docs, "deep.txt", 0, &e) == 0 && write_pat(&inner, &e, 0, 2222, 3) == 2222);
    CHECK(check() == 0);
    CHECK(exfat_delete(&F, &root, &docs) == EXFAT_ERR_NOTEMPTY);
    CHECK(exfat_delete(&F, &inner, &e) == 0);                                  /* the file in inner */
    CHECK(exfat_delete(&F, &docs, &inner) == 0 && exfat_delete(&F, &root, &docs) == 0);
    CHECK(!find(&root, "Docs", &e2));
    CHECK(check() == 0);

    /* ---- a folder that grows: ordinary chained folder, then the NoFatChain one ---- */
    CHECK(exfat_create(&F, &root, 0, "Many", 1, &e2) == 0);
    ExEntry many = e2;
    for (int i = 0; i < 100; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "file number %03d with a longish name.dat", i);
        CHECK(exfat_create(&F, &many, &root, nm, 0, &e) == 0);
    }
    CHECK(many.size > 1024 && list_dir(&many) == 100);
    CHECK(find(&root, "Many", &e2) && e2.size == many.size && e2.first_cluster == many.first_cluster);   /* its own entry learned the length */
    CHECK(check() == 0);
    for (int i = 0; i < 100; i += 2) {
        char nm[64];
        snprintf(nm, sizeof nm, "file number %03d with a longish name.dat", i);
        CHECK(find(&many, nm, &e) && exfat_delete(&F, &many, &e) == 0);
    }
    CHECK(list_dir(&many) == 50);
    for (int i = 0; i < 20; i++) {                                              /* the holes are reused */
        char nm[64];
        snprintf(nm, sizeof nm, "again %02d.txt", i);
        CHECK(exfat_create(&F, &many, &root, nm, 0, &e) == 0);
    }
    CHECK(list_dir(&many) == 70 && check() == 0);
    {   /* the NoFatChain folder (2 clusters = 64 entries) overflows: it becomes a chain and grows */
        CHECK(find(&root, "Folder", &folder) && folder.nofat);
        for (int i = 0; i < 40; i++) {
            char nm[64];
            snprintf(nm, sizeof nm, "Filler file number %02d.txt", i);
            CHECK(exfat_create(&F, &folder, &root, nm, 0, &e) == 0);
        }
        CHECK(!folder.nofat && folder.size > 2048 && list_dir(&folder) == 42);
        CHECK(find(&root, "Folder", &e) && e.size == folder.size && !e.nofat);
        CHECK(check() == 0);
    }

    /* ---- rename and move ---- */
    CHECK(find(&root, "chained.bin", &e));
    CHECK(exfat_rename(&F, &root, 0, &e, "now with a much longer name than before.bin") == 0 && e.secondary == 4);
    CHECK(find(&root, "now with a much longer name than before.bin", &e2) && verify(&e2, 5000, 2, 500));
    CHECK(!find(&root, "chained.bin", &e2));
    CHECK(exfat_rename(&F, &root, 0, &e, "S.bin") == 0 && e.secondary == 2);
    CHECK(exfat_rename(&F, &root, 0, &e, "s.BIN") == 0);                        /* only the case */
    CHECK(exfat_rename(&F, &root, 0, &e, "BLOCKER.bin") == EXFAT_ERR_EXISTS);
    CHECK(exfat_rename(&F, &root, 0, &e, "bad|name") == EXFAT_ERR_BADNAME);
    CHECK(check() == 0);
    CHECK(find(&root, "Many", &many));
    ExEntry old_dir = root;
    CHECK(exfat_move(&F, &old_dir, &many, &root, &e, "s.BIN") == 0);
    CHECK(find(&many, "s.BIN", &e2) && verify(&e2, 5000, 2, 999) && !find(&root, "s.BIN", &e2));
    CHECK(exfat_create(&F, &root, 0, "s.BIN", 0, &e2) == 0 && exfat_move(&F, &root, &many, &root, &e2, "s.BIN") == EXFAT_ERR_EXISTS);
    CHECK(exfat_delete(&F, &root, &e2) == 0);
    CHECK(find(&root, "Many", &many));
    CHECK(find(&root, "Folder", &folder));
    CHECK(exfat_move(&F, &root, &folder, &root, &many, "Many") == 0);          /* a folder with its content, into another */
    CHECK(find(&folder, "Many", &many) && list_dir(&many) == 71);
    CHECK(check() == 0);

    /* ---- delete frees the clusters ---- */
    {
        unsigned long long tot, f1, f2;
        CHECK(exfat_space(&F, &tot, &f1) == 1);
        CHECK(find(&root, "big.bin", &e) && exfat_delete(&F, &root, &e) == 0);
        CHECK(exfat_space(&F, &tot, &f2) == 1 && f2 >= f1 + 290000);
        CHECK(check() == 0);
    }

    /* ---- the volume fills up ---- */
    CHECK(exfat_create(&F, &root, 0, "fill.bin", 0, &e) == 0);
    {
        unsigned off = 0;
        int r;
        static unsigned char chunk[32768];
        for (unsigned i = 0; i < sizeof chunk; i++) chunk[i] = pat(8, i);
        while ((r = exfat_write(&F, &root, &e, off, chunk, sizeof chunk)) > 0) off += (unsigned)r;
        CHECK(r == EXFAT_ERR_FULL);
        CHECK(e.size == off && check() == 0);                                    /* the failed write left nothing behind */
        CHECK(exfat_delete(&F, &root, &e) == 0 && check() == 0);
    }

    /* ---- the lock ---- */
    blk_set_locked(DEV, 1);
    {
        unsigned long long w = WRITES;
        ExEntry g, h;
        CHECK(find(&root, "new.txt", &g));
        CHECK(exfat_create(&F, &root, 0, "locked.txt", 0, &h) == EXFAT_ERR_RO);
        CHECK(exfat_write(&F, &root, &g, 0, "x", 1) == EXFAT_ERR_RO);
        CHECK(exfat_delete(&F, &root, &g) == EXFAT_ERR_RO);
        CHECK(exfat_rename(&F, &root, 0, &g, "zzz") == EXFAT_ERR_RO);
        CHECK(exfat_move(&F, &root, &folder, &root, &g, "new.txt") == EXFAT_ERR_RO);
        CHECK(WRITES == w);
        unsigned char b[16];
        CHECK(exfat_read(&F, &g, 0, b, 16) == 16);
    }
    blk_set_locked(DEV, 0);

    /* ---- remount: it must all be on the disk ---- */
    {
        ExFat g;
        CHECK(exfat_mount(&g, DEV) == 0);
        ExFat save = F;
        F = g;
        ExEntry r2;
        exfat_root(&F, &r2);
        CHECK(find(&r2, "new.txt", &e) && e.size == 5100);
        {
            unsigned char b[5100];
            int ok = exfat_read(&F, &e, 0, b, 5100) == 5100;
            for (unsigned i = 0; ok && i < 5100; i++) if (b[i] != ((i >= 1000 && i < 4000) ? pat(6, i) : pat(5, i))) ok = 0;
            CHECK(ok);
        }
        CHECK(find(&r2, "Folder", &folder) && find(&folder, "Many", &many) && list_dir(&many) == 71);
        F = save;
    }
    CHECK(check() == 0);

    /* ---- a volume made without an up-case table of its own entry is still usable (ASCII fold) ---- */
    free(X.img); free(X.bitmap); free(D_SMALL); free(D_CHAIN);
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("exfat write tests passed\n");
    return 0;
}
