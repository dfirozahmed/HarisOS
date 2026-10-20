/* Host test for fat32.c: formats a small FAT32 image in RAM, runs file / folder operations on it and
 * checks the result with an independent consistency checker (chains, long names, ".." links, no lost clusters). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "fat32.h"
#include "fatimg.h"

/* ---- helpers on top of the API ---- */
typedef struct { char names[600][FAT_NAME_MAX]; FatEntry e[600]; int n; } Listing;

static int lcb(const char *name, const FatEntry *e, void *user)
{
    Listing *l = (Listing *)user;
    if (l->n < 600) { strcpy(l->names[l->n], name); l->e[l->n] = *e; l->n++; }
    return 0;
}

static Listing LST;
static int list_dir(unsigned dir) { LST.n = 0; int r = fat_list(&F, dir, lcb, &LST); return r; }
static int find(unsigned dir, const char *name, FatEntry *out)
{
    list_dir(dir);
    for (int i = 0; i < LST.n; i++)
        if (strcmp(LST.names[i], name) == 0) { *out = LST.e[i]; return 1; }
    return 0;
}

static unsigned char pat(unsigned seed, unsigned i) { return (unsigned char)((i * 7 + seed * 13 + (i >> 8)) & 0xFF); }

static int write_pattern(FatEntry *e, unsigned off, unsigned len, unsigned seed)
{
    unsigned char *b = malloc(len);
    for (unsigned i = 0; i < len; i++) b[i] = pat(seed, off + i);
    int r = fat_write(&F, e, off, b, len);
    free(b);
    return r;
}

static int verify_pattern(const FatEntry *e, unsigned len, unsigned seed)
{
    unsigned char *b = malloc(len + 1);
    int ok = e->size == len;
    unsigned got = 0;
    while (ok && got < len) {
        int r = fat_read(&F, e, got, b + got, len - got > 777 ? 777 : len - got); /* odd chunk size on purpose */
        if (r <= 0) { ok = 0; break; }
        got += (unsigned)r;
    }
    for (unsigned i = 0; ok && i < len; i++) if (b[i] != pat(seed, i)) { ok = 0; printf("  mismatch at %u\n", i); }
    free(b);
    return ok;
}

int main(void)
{
    FatEntry e, d1, d2, f1;

    /* ---- mount checks ---- */
    new_volume(20000, 2);
    CHECK(strcmp(F.label, "TESTVOL") == 0 && F.clusters == NCLUSTERS && F.root_cluster == 2);
    CHECK(list_dir(fat_root(&F)) == 0);
    unsigned long long total, freeb;
    fat_space(&F, &total, &freeb);
    CHECK(total == (unsigned long long)NCLUSTERS * 1024 && freeb == total - 1024);
    CHECK(fsck() == 0);
    {   /* a damaged boot sector is refused */
        DISK[510] = 0;
        Fat g;
        CHECK(fat_mount(&g, disk_idx) == -1);
        DISK[510] = 0x55;
        DISK[17] = 1; /* FAT16-style root entry count */
        CHECK(fat_mount(&g, disk_idx) == -1);
        DISK[17] = 0;
        DISK[13] = 3; /* sectors per cluster not a power of two */
        CHECK(fat_mount(&g, disk_idx) == -1);
        DISK[13] = 2;
        CHECK(fat_mount(&g, disk_idx) == 0);
    }

    /* ---- creating files: names ---- */
    unsigned root = fat_root(&F);
    CHECK(fat_create(&F, root, "readme.txt", 0, &e) == 0 && e.nlfn == 0 && e.size == 0 && e.first_cluster == 0);
    CHECK(fat_create(&F, root, "README.TXT", 0, &e) == FAT_ERR_EXISTS);       /* same name, other case */
    CHECK(fat_create(&F, root, "Hello World.txt", 0, &e) == 0 && e.nlfn == 2);
    CHECK(fat_create(&F, root, "A very long file name that needs several long-name entries.document", 0, &e) == 0 && e.nlfn == 6);
    CHECK(fat_create(&F, root, "Hello World2.txt", 0, &e) == 0);
    CHECK(fat_create(&F, root, "Hello Wor~1.txt", 0, &e) == 0);               /* looks like a generated short name */
    CHECK(fat_create(&F, root, ".hidden", 0, &e) == 0);
    CHECK(fat_create(&F, root, "a.b.c", 0, &e) == 0);
    CHECK(fat_create(&F, root, "UPPER.TXT", 0, &e) == 0 && e.nlfn == 0);
    CHECK(fat_create(&F, root, "Mixed.Txt", 0, &e) == 0 && e.nlfn == 1);
    CHECK(fat_create(&F, root, "no_ext", 0, &e) == 0);
    CHECK(fat_create(&F, root, "", 0, &e) == FAT_ERR_BADNAME);
    CHECK(fat_create(&F, root, "a/b", 0, &e) == FAT_ERR_BADNAME);
    CHECK(fat_create(&F, root, "what?", 0, &e) == FAT_ERR_BADNAME);
    CHECK(fat_create(&F, root, "trail.", 0, &e) == FAT_ERR_BADNAME);
    CHECK(fat_create(&F, root, "trail ", 0, &e) == FAT_ERR_BADNAME);
    CHECK(fat_create(&F, root, " lead", 0, &e) == FAT_ERR_BADNAME);
    CHECK(fat_create(&F, root, "..", 0, &e) == FAT_ERR_BADNAME);
    CHECK(fat_create(&F, root, ".", 1, &e) == FAT_ERR_BADNAME);
    char big[300];
    memset(big, 'x', 256); big[256] = 0;
    CHECK(fat_create(&F, root, big, 0, &e) == FAT_ERR_BADNAME);                /* 256 characters */
    memset(big, 'y', 255); big[255] = 0;
    CHECK(fat_create(&F, root, big, 0, &e) == 0 && e.nlfn == 20);              /* 255 is the limit */
    CHECK(fsck() == 0);

    list_dir(root);
    CHECK(LST.n == 11);
    CHECK(find(root, "readme.txt", &e) && find(root, "Hello World.txt", &e) && find(root, "UPPER.TXT", &e));
    CHECK(find(root, ".hidden", &e) && find(root, "a.b.c", &e) && find(root, "Mixed.Txt", &e));
    CHECK(!find(root, "README.TXT", &e)); /* names are listed as stored */

    /* ---- file data ---- */
    CHECK(find(root, "readme.txt", &f1));
    CHECK(write_pattern(&f1, 0, 100, 1) == 100 && f1.size == 100 && f1.first_cluster >= 3);
    CHECK(write_pattern(&f1, 100, 5000, 1) == 5000 && f1.size == 5100);        /* append over cluster boundaries */
    CHECK(verify_pattern(&f1, 5100, 1));
    CHECK(write_pattern(&f1, 1000, 3000, 2) == 3000 && f1.size == 5100);       /* overwrite in the middle */
    {
        unsigned char buf[5100];
        CHECK(fat_read(&F, &f1, 0, buf, 5100) == 5100);
        int ok = 1;
        for (unsigned i = 0; i < 5100; i++) {
            unsigned char want = (i >= 1000 && i < 4000) ? pat(2, i) : pat(1, i);
            if (buf[i] != want) ok = 0;
        }
        CHECK(ok);
        CHECK(fat_read(&F, &f1, 5100, buf, 10) == 0 && fat_read(&F, &f1, 5090, buf, 100) == 10);
        CHECK(fat_write(&F, &f1, 5101, buf, 1) == FAT_ERR_ARG);               /* a gap is not allowed */
    }
    CHECK(fsck() == 0);
    CHECK(find(root, "readme.txt", &e) && e.size == 5100 && e.first_cluster == f1.first_cluster); /* it is on the disk */

    /* a big file: many clusters, written in odd pieces */
    CHECK(fat_create(&F, root, "big.bin", 0, &f1) == 0);
    {
        unsigned total_len = 300000, off = 0;
        while (off < total_len) {
            unsigned n = (off % 3) ? 4093 : 12345;
            if (n > total_len - off) n = total_len - off;
            CHECK(write_pattern(&f1, off, n, 5) == (int)n);
            off += n;
        }
        CHECK(verify_pattern(&f1, total_len, 5));
    }
    CHECK(fsck() == 0);

    /* shrinking and deleting give the clusters back */
    fat_space(&F, &total, &freeb);
    unsigned long long free_before = freeb;
    CHECK(fat_shrink(&F, &f1, 100000) == 0 && f1.size == 100000);
    CHECK(verify_pattern(&f1, 100000, 5));
    CHECK(fat_shrink(&F, &f1, 100001) == FAT_ERR_ARG);
    fat_space(&F, &total, &freeb);
    CHECK(freeb > free_before + 190000);
    CHECK(fsck() == 0);
    CHECK(fat_shrink(&F, &f1, 0) == 0 && f1.first_cluster == 0 && f1.size == 0);
    CHECK(fsck() == 0);
    CHECK(write_pattern(&f1, 0, 3000, 6) == 3000 && verify_pattern(&f1, 3000, 6)); /* an emptied file can grow again */
    CHECK(fat_delete(&F, &f1) == 0);
    CHECK(!find(root, "big.bin", &e));
    CHECK(fsck() == 0);

    /* ---- folders ---- */
    CHECK(fat_create(&F, root, "Docs", 1, &d1) == 0 && (d1.attr & FAT_ATTR_DIR) && d1.first_cluster >= 3);
    CHECK(fat_create(&F, root, "docs", 1, &d2) == FAT_ERR_EXISTS);
    CHECK(fat_create(&F, d1.first_cluster, "inner folder", 1, &d2) == 0);
    CHECK(fat_create(&F, d2.first_cluster, "deep.txt", 0, &f1) == 0);
    CHECK(write_pattern(&f1, 0, 777, 9) == 777);
    CHECK(fsck() == 0);                               /* "." and ".." are checked there */
    CHECK(list_dir(d1.first_cluster) == 1 && strcmp(LST.names[0], "inner folder") == 0);
    CHECK(fat_delete(&F, &d1) == FAT_ERR_NOTEMPTY);   /* not empty */
    CHECK(fat_delete(&F, &d2) == FAT_ERR_NOTEMPTY);
    CHECK(fat_delete(&F, &f1) == 0 && fat_delete(&F, &d2) == 0 && fat_delete(&F, &d1) == 0);
    CHECK(!find(root, "Docs", &e));
    CHECK(fsck() == 0);

    /* ---- rename and move ---- */
    CHECK(fat_create(&F, root, "Alpha", 1, &d1) == 0 && fat_create(&F, root, "Beta", 1, &d2) == 0);
    CHECK(fat_create(&F, d1.first_cluster, "note.txt", 0, &f1) == 0 && write_pattern(&f1, 0, 2500, 3) == 2500);
    CHECK(fat_rename(&F, &f1, "A much longer new name.txt") == 0 && f1.nlfn >= 1);
    CHECK(find(d1.first_cluster, "A much longer new name.txt", &e) && e.size == 2500 && verify_pattern(&e, 2500, 3));
    CHECK(!find(d1.first_cluster, "note.txt", &e));
    CHECK(fat_rename(&F, &f1, "short.txt") == 0 && f1.nlfn == 0);
    CHECK(fat_rename(&F, &f1, "SHORT.TXT") == 0);                 /* only the case changes */
    CHECK(find(d1.first_cluster, "SHORT.TXT", &e));
    CHECK(fat_create(&F, d1.first_cluster, "other.txt", 0, &e) == 0);
    CHECK(fat_rename(&F, &f1, "OTHER.txt") == FAT_ERR_EXISTS);    /* used by another entry */
    CHECK(fat_rename(&F, &f1, "bad|name") == FAT_ERR_BADNAME);
    CHECK(fsck() == 0);
    CHECK(fat_move(&F, &f1, "SHORT.TXT", d2.first_cluster) == 0 && f1.dir_cluster == d2.first_cluster);
    CHECK(find(d2.first_cluster, "SHORT.TXT", &e) && verify_pattern(&e, 2500, 3));
    CHECK(!find(d1.first_cluster, "SHORT.TXT", &e));
    CHECK(fat_move(&F, &f1, "SHORT.TXT", d2.first_cluster) == 0);   /* already there */
    CHECK(fat_create(&F, d1.first_cluster, "SHORT.TXT", 0, &e) == 0);
    CHECK(fat_move(&F, &e, "SHORT.TXT", d2.first_cluster) == FAT_ERR_EXISTS);
    CHECK(fsck() == 0);
    /* a folder moves with its content and its ".." follows */
    CHECK(fat_move(&F, &d1, "Alpha", d2.first_cluster) == 0);        /* /Alpha -> /Beta/Alpha */
    CHECK(find(d2.first_cluster, "Alpha", &e) && list_dir(e.first_cluster) == 2);
    CHECK(fsck() == 0);
    CHECK(fat_move(&F, &d1, "Alpha", root) == 0);                    /* and back to the root: ".." becomes 0 */
    CHECK(fsck() == 0);

    /* ---- a folder that grows over many clusters ---- */
    CHECK(fat_create(&F, root, "Many", 1, &d1) == 0);
    for (int i = 0; i < 150; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "file number %03d with a longish name.dat", i);
        CHECK(fat_create(&F, d1.first_cluster, nm, 0, &e) == 0);
    }
    CHECK(list_dir(d1.first_cluster) == 150 && LST.n == 150);
    CHECK(fsck() == 0);
    for (int i = 0; i < 150; i += 2) {                               /* delete every second one */
        char nm[64];
        snprintf(nm, sizeof nm, "file number %03d with a longish name.dat", i);
        CHECK(find(d1.first_cluster, nm, &e) && fat_delete(&F, &e) == 0);
    }
    CHECK(list_dir(d1.first_cluster) == 75);
    for (int i = 0; i < 40; i++) {                                   /* new entries reuse the holes */
        char nm[64];
        snprintf(nm, sizeof nm, "again %02d.txt", i);
        CHECK(fat_create(&F, d1.first_cluster, nm, 0, &e) == 0);
    }
    CHECK(list_dir(d1.first_cluster) == 115);
    CHECK(fsck() == 0);

    /* ---- remount: everything must be on the disk, not in a cache ---- */
    {
        Fat g;
        CHECK(fat_mount(&g, disk_idx) == 0);
        Fat save = F;
        F = g;
        CHECK(list_dir(d1.first_cluster) == 115);
        CHECK(find(root, "A very long file name that needs several long-name entries.document", &e));
        F = save;
    }

    /* ---- the disk fills up ---- */
    CHECK(fat_create(&F, root, "fill.bin", 0, &f1) == 0);
    {
        unsigned off = 0;
        int r = 0;
        static unsigned char chunk[65536];
        for (unsigned i = 0; i < sizeof chunk; i++) chunk[i] = pat(7, i);
        while ((r = fat_write(&F, &f1, off, chunk, sizeof chunk)) > 0) off += (unsigned)r;
        CHECK(r == FAT_ERR_FULL);
        fat_space(&F, &total, &freeb);
        CHECK(freeb < 65536);
        CHECK(fat_create(&F, root, "no-room-for-folders", 1, &e) == FAT_ERR_FULL || freeb > 0);
        CHECK(fsck() == 0);                                           /* a full disk is still consistent */
        CHECK(fat_delete(&F, &f1) == 0);
        CHECK(fsck() == 0);
        fat_space(&F, &total, &freeb);
        CHECK(freeb > total / 2);
    }

    /* ---- the lock ---- */
    blk_set_locked(disk_idx, 1);
    {
        unsigned long long w = WRITES;
        FatEntry g;
        CHECK(fat_create(&F, root, "locked.txt", 0, &g) == FAT_ERR_RO);
        CHECK(find(root, "readme.txt", &g));
        CHECK(fat_write(&F, &g, 0, "x", 1) == FAT_ERR_RO);
        CHECK(fat_delete(&F, &g) == FAT_ERR_RO);
        CHECK(fat_rename(&F, &g, "zzz") == FAT_ERR_RO);
        CHECK(fat_move(&F, &g, "readme.txt", d1.first_cluster) == FAT_ERR_RO);
        CHECK(fat_shrink(&F, &g, 0) == FAT_ERR_RO);
        CHECK(WRITES == w);                                           /* nothing reached the disk */
        unsigned char b[16];
        CHECK(fat_read(&F, &g, 0, b, 16) == 16);                      /* reading works */
    }
    blk_set_locked(disk_idx, 0);

    /* ---- 4 KiB sectors would need another RAM disk; clusters of 1 sector and 8 sectors ---- */
    for (unsigned spc = 1; spc <= 8; spc *= 8) {
        new_volume(9000, spc);
        root = fat_root(&F);
        CHECK(fat_create(&F, root, "d", 1, &d1) == 0 && fat_create(&F, d1.first_cluster, "f.bin", 0, &f1) == 0);
        CHECK(write_pattern(&f1, 0, 50000, 11) == 50000 && verify_pattern(&f1, 50000, 11));
        CHECK(fsck() == 0);
    }

    /* ---- the real thing: the ESP in build/haris.img, made by tools/mkimg.c and booted by the firmware ---- */
    FILE *img = fopen("build/haris.img", "rb");
    if (img) {
        fseek(img, 0, SEEK_END);
        long len = ftell(img);
        fseek(img, 0, SEEK_SET);
        free(DISK);
        NSEC = (unsigned long long)len / SS;
        DISK = malloc((size_t)len);
        if (fread(DISK, 1, (size_t)len, img) != (size_t)len) { printf("read error\n"); return 1; }
        fclose(img);
        blk_reset();
        BlkDev d;
        memset(&d, 0, sizeof d);
        strcpy(d.name, "sata0"); d.kind = BLK_SATA; d.parent = -1; d.sector_size = SS; d.sectors = NSEC; d.writable = 1; d.read = rd; d.write = wr; d.ctx = DISK;
        int di = blk_register(&d);
        CHECK(part_scan(di) == 1);
        CHECK(fat_mount(&F, 1) == 0);
        root = fat_root(&F);
        list_dir(root);
        printf("haris.img ESP: label '%s', %d entries in the root:", F.label, LST.n);
        for (int i = 0; i < LST.n; i++) printf(" %s", LST.names[i]);
        printf("\n");
        FatEntry k;
        CHECK(find(root, "KERNEL.EFI", &k));
        FILE *kf = fopen("build/KERNEL.EFI", "rb");
        if (kf && k.size) {
            unsigned char *want = malloc(k.size), *got = malloc(k.size);
            CHECK(fread(want, 1, k.size, kf) == k.size);
            CHECK(fat_read(&F, &k, 0, got, k.size) == (int)k.size && memcmp(want, got, k.size) == 0); /* bit-exact */
            free(want); free(got);
            fclose(kf);
        }
        FatEntry efi, boot, loader;
        CHECK(find(root, "EFI", &efi) && (efi.attr & FAT_ATTR_DIR));
        CHECK(find(efi.first_cluster, "BOOT", &boot));
        CHECK(find(boot.first_cluster, "BOOTX64.EFI", &loader) && loader.size > 1000);
    }

    free(DISK);
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("fat32 tests passed\n");
    return 0;
}
