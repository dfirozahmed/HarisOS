/* Host test for volume.c + vfs.c: FAT32 images mounted as drives under "This PC", and every file operation
 * of the File Explorer done through the VFS (lazy loading, refresh, create, copy, move, rename, delete, the
 * write lock, errors), each time checked against the independent FAT consistency checker. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "fat32.h"
#include "fatimg.h"
#include "exfatimg.h"
#include "ntfsimg.h"
#include "exfatcheck.h"
#include "ntfscheck.h"
#include "ntfswimg.h"
#include "vfs.h"
#include "volume.h"

/* a second image next to the first: same size and cluster size, so the checker's constants fit both */
static unsigned char *IMG[2];

static void make_image(int n, const char *name)
{
    DISK = 0; /* mkfs would free the previous one */
    mkfs(20000, 2);
    IMG[n] = DISK;
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, name);
    d.kind = BLK_SATA; d.media = BLK_MEDIA_SSD; d.parent = -1; d.sector_size = SS; d.sectors = NSEC;
    d.writable = 1; d.read = rd; d.write = wr; d.ctx = DISK;
    blk_register(&d);
}

static int check_image(int n)
{
    DISK = IMG[n];
    return fsck();
}

static unsigned char pat(unsigned seed, unsigned i) { return (unsigned char)((i * 7 + seed * 13 + (i >> 8)) & 0xFF); }

static unsigned EX_CLUSTERS; /* clusters of the exFAT test volume */

/* put a file with known content on image n through the FAT code directly (another "computer" wrote it) */
static void external_file(int blk, unsigned dir, const char *name, unsigned size, unsigned seed)
{
    Fat g;
    FatEntry e;
    CHECK(fat_mount(&g, blk) == 0);
    CHECK(fat_create(&g, dir, name, 0, &e) == 0);
    unsigned char *b = malloc(size ? size : 1);
    for (unsigned i = 0; i < size; i++) b[i] = pat(seed, i);
    if (size) CHECK(fat_write(&g, &e, 0, b, size) == (int)size);
    free(b);
}

/* does the file `id` hold exactly these bytes? */
static int same_content_buf(int id, const unsigned char *want, unsigned size)
{
    const VfsNode *n = vfs_node(id);
    if (!n || n->size != size) return 0;
    unsigned char *b = malloc(size + 1);
    unsigned got = 0;
    int ok = 1;
    while (got < size) {
        int r = vfs_read(id, got, b + got, size - got > 5000 ? 5000 : size - got);
        if (r <= 0) { ok = 0; break; }
        got += (unsigned)r;
    }
    if (ok && memcmp(b, want, size) != 0) ok = 0;
    free(b);
    return ok;
}

static int same_content(int id, unsigned size, unsigned seed)
{
    const VfsNode *n = vfs_node(id);
    if (!n || n->size != size) return 0;
    unsigned char *b = malloc(size + 1);
    unsigned got = 0;
    int ok = 1;
    while (got < size) {
        int r = vfs_read(id, got, b + got, size - got > 4000 ? 4000 : size - got);
        if (r <= 0) { ok = 0; break; }
        got += (unsigned)r;
    }
    for (unsigned i = 0; ok && i < size; i++) if (b[i] != pat(seed, i)) ok = 0;
    free(b);
    return ok;
}

/* ---- a small exFAT and a small NTFS volume, registered as read-only block devices ---- */
static XImg EXX;                 /* the exFAT test volume (the checker reads its image) */
static unsigned char *EXIMG, *EXBITMAP;
static int wr_rw(BlkDev *d, unsigned long long lba, unsigned n, const void *buf)
{
    memcpy((unsigned char *)d->ctx + lba * 512, buf, (size_t)n * 512);
    return 0;
}
static int rd_ro(BlkDev *d, unsigned long long lba, unsigned n, void *buf)
{
    memcpy(buf, (unsigned char *)d->ctx + lba * 512, (size_t)n * 512);
    return 0;
}

static void add_ro_disk(unsigned char *img, unsigned sectors, const char *name)
{
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, name);
    d.kind = BLK_NVME; d.media = BLK_MEDIA_SSD; d.parent = -1; d.sector_size = 512; d.sectors = sectors; d.writable = 0; d.read = rd_ro; d.ctx = img;
    blk_register(&d);
}

static unsigned char EX_DATA_SMALL[3000], EX_DATA_BIG[70000];
static void make_exfat(void)
{
    XImg x;
    xformat(&x, 32768, 2, "EXVOL");
    XDir root, sub;
    xdir_root(&x, &root);
    for (unsigned i = 0; i < sizeof EX_DATA_SMALL; i++) EX_DATA_SMALL[i] = pat(21, i);
    for (unsigned i = 0; i < sizeof EX_DATA_BIG; i++) EX_DATA_BIG[i] = pat(22, i);
    xfile_contig(&x, &root, "small.txt", EX_DATA_SMALL, sizeof EX_DATA_SMALL);
    xmkdir(&x, &root, "Folder", &sub, 0, 1);
    xfile_chained(&x, &sub, "big.bin", EX_DATA_BIG, sizeof EX_DATA_BIG);
    xfinish(&x);
    EXIMG = x.img;
    EXBITMAP = x.bitmap;
    EX_CLUSTERS = x.clusters;
    EXX = x;
    {   /* this one can be written (when unlocked) */
        BlkDev d;
        memset(&d, 0, sizeof d);
        strcpy(d.name, "nvme7");
        d.kind = BLK_NVME; d.media = BLK_MEDIA_SSD; d.parent = -1; d.sector_size = 512; d.sectors = 32768; d.writable = 1;
        d.read = rd_ro; d.write = wr_rw; d.ctx = EXIMG;
        blk_register(&d);
    }
}

static unsigned char NT_SMALL[300], NT_BIG[20000];
static NImg NTI;
static void make_ntfs(void)
{
    memset(&NTI, 0, sizeof NTI);
    NTI.img = calloc(NT_SECTORS, NT_SS);
    NTI.mft = calloc(NT_MFT_RECORDS, NT_REC);
    nboot(&NTI, "");
    unsigned mc = NT_MFT_RECORDS * NT_REC / NT_CS;
    nmark(&NTI, 0, NT_MFT_LCN + mc + 4);
    NTI.next_lcn = NT_MFT_LCN + mc + 4;
    NRec r;
    NChild c0 = {"$MFT", 0, 0, 0, 1};
    nrec_begin(&r, 0, 1, 0); nstdinfo(&r, 6); nfilename_attrs(&r, REF_(5), &c0, 0);
    NRunSpec mrun = {mc, NT_MFT_LCN};
    nrec_nonres(&r, 0x80, 0, &mrun, 1, 0, (unsigned long long)mc * NT_CS, (unsigned long long)NT_MFT_RECORDS * NT_REC, (unsigned long long)NT_MFT_RECORDS * NT_REC, 0);
    nrec_finish(&NTI, &r);
    NChild c3 = {"$Volume", 3, 0, 0, 1};
    nrec_begin(&r, 3, 1, 0); nstdinfo(&r, 6); nfilename_attrs(&r, REF_(5), &c3, 0);
    unsigned char lab[10];
    for (int i = 0; i < 5; i++) np16(lab + 2 * i, (unsigned char)"NTVOL"[i]);
    nrec_res(&r, 0x60, 0, lab, 10);
    nrec_finish(&NTI, &r);
    for (unsigned i = 0; i < sizeof NT_SMALL; i++) NT_SMALL[i] = pat(23, i);
    for (unsigned i = 0; i < sizeof NT_BIG; i++) NT_BIG[i] = pat(24, i);
    /* 24: small.txt (resident) */
    NChild f24 = {"small.txt", 24, 0, sizeof NT_SMALL, 1};
    nrec_begin(&r, 24, 1, 0); nstdinfo(&r, 0x20); nfilename_attrs(&r, REF_(5), &f24, 0); nrec_res(&r, 0x80, 0, NT_SMALL, sizeof NT_SMALL); nrec_finish(&NTI, &r);
    /* 25: big.bin (two runs) */
    unsigned a = nalloc(&NTI, 3), gap = nalloc(&NTI, 1), b = nalloc(&NTI, 2);
    (void)gap;
    memcpy(ncl(&NTI, a), NT_BIG, 3 * NT_CS);
    memcpy(ncl(&NTI, b), NT_BIG + 3 * NT_CS, sizeof NT_BIG - 3 * NT_CS);
    NRunSpec runs[2] = {{3, a}, {2, b}};
    NChild f25 = {"big.bin", 25, 0, sizeof NT_BIG, 1};
    nrec_begin(&r, 25, 1, 0); nstdinfo(&r, 0x20); nfilename_attrs(&r, REF_(5), &f25, 0);
    nrec_nonres(&r, 0x80, 0, runs, 2, 0, 5ULL * NT_CS, sizeof NT_BIG, sizeof NT_BIG, 0); nrec_finish(&NTI, &r);
    /* 26: packed.dat (compressed flag) */
    unsigned cl = nalloc(&NTI, 1);
    NRunSpec crun = {1, cl};
    NChild f26 = {"packed.dat", 26, 0, 3000, 1};
    nrec_begin(&r, 26, 1, 0); nstdinfo(&r, 0x20); nfilename_attrs(&r, REF_(5), &f26, 0);
    nrec_nonres(&r, 0x80, 0, &crun, 1, 0, NT_CS, 3000, 3000, 0x0001); nrec_finish(&NTI, &r);
    /* root folder */
    NChild kids[3] = {f24, f25, f26};
    NChild rc = {".", 5, 1, 0, 1};
    nrec_begin(&r, 5, 3, 0); nstdinfo(&r, 0x10); nfilename_attrs(&r, REF_(5), &rc, 0);
    nindex(&NTI, &r, kids, 3, REF_(5), 0);
    nrec_finish(&NTI, &r);
    /* $Bitmap */
    unsigned bl = nalloc(&NTI, 1);
    NChild c6 = {"$Bitmap", 6, 0, 0, 1};
    nrec_begin(&r, 6, 1, 0); nstdinfo(&r, 6); nfilename_attrs(&r, REF_(5), &c6, 0);
    NRunSpec brun = {1, bl};
    nrec_nonres(&r, 0x80, 0, &brun, 1, 0, NT_CS, NT_CLUSTERS / 8, NT_CLUSTERS / 8, 0);
    nrec_finish(&NTI, &r);
    memcpy(ncl(&NTI, bl), NTI.bitmap, sizeof NTI.bitmap);
    memcpy(NTI.img + (size_t)NT_MFT_LCN * NT_CS, NTI.mft, (size_t)NT_MFT_RECORDS * NT_REC);
    add_ro_disk(NTI.img, NT_SECTORS, "sata9");
}

/* a second NTFS volume that can be written (when unlocked): the one the NTFS write tests use */
static unsigned char *NTWIMG;
static void make_ntfsw(void)
{
    wb_build(0);
    NTWIMG = malloc((size_t)NT_SECTORS * NT_SS);
    memcpy(NTWIMG, WB_BASE, (size_t)NT_SECTORS * NT_SS);
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, "nvme8");
    d.kind = BLK_NVME; d.media = BLK_MEDIA_SSD; d.parent = -1; d.sector_size = 512; d.sectors = NT_SECTORS; d.writable = 1;
    d.read = rd_ro; d.write = wr_rw; d.ctx = NTWIMG;
    blk_register(&d);
}

int main(void)
{
    blk_reset();
    make_image(0, "sata0");
    make_image(1, "sata1");
    make_exfat();
    make_ntfs();
    make_ntfsw();
    vfs_init();
    CHECK(volume_mount_all() == 5);
    CHECK(vfs_volume_count() == 6);                      /* the RAM Disk, two FAT32 drives, an exFAT and two NTFS ones */
    int a = vfs_lookup("/TESTVOL (sata0)"), b = vfs_lookup("/TESTVOL (sata1)");
    CHECK(a > 0 && b > 0 && a != b);
    CHECK(vfs_volume_node(1) == a && vfs_volume_node(2) == b && vfs_volume_of(a) == 1);

    VfsVolInfo vi;
    CHECK(vfs_volume_info(1, &vi) == 0 && strcmp(vi.fs_name, "FAT32") == 0 && strcmp(vi.device, "SSD (SATA)") == 0);
    CHECK(vi.writable == 1 && vi.locked == 1);            /* locked until the user says so */
    CHECK(vi.total == (unsigned long long)NCLUSTERS * 1024 && vi.free == vi.total - 1024);

    /* ---- locked: nothing is written ---- */
    unsigned long long w0 = WRITES;
    CHECK(vfs_add(a, "nope", 1, 0) == -1 && vfs_last_error() == VFS_ERR_RO);
    CHECK(WRITES == w0);
    CHECK(vfs_list(a, (int[16]){0}, 16) == 0);

    /* ---- unlock, create ---- */
    CHECK(vfs_set_locked(1, 0) == 0 && vfs_set_locked(2, 0) == 0);
    CHECK(vfs_volume_info(1, &vi) == 0 && vi.locked == 0);
    int docs = vfs_add(a, "Docs", 1, 0);
    CHECK(docs > 0 && vfs_node(docs)->vol == 1 && vfs_node(docs)->loaded);
    CHECK(vfs_add(a, "docs", 1, 0) == -1 && vfs_last_error() == VFS_ERR_EXISTS);
    CHECK(vfs_add(a, "bad?name", 0, 0) == -1 && vfs_last_error() == VFS_ERR_BADNAME);
    int note = vfs_add(docs, "A long note name.txt", 0, 0);
    CHECK(note > 0 && vfs_node(note)->size == 0);
    CHECK(check_image(0) == 0);

    /* ---- files written by someone else show up after a refresh; ids that stay keep their id ---- */
    unsigned root_a = 2;
    external_file(0, root_a, "outside.bin", 123456, 3);          /* block device 0 = sata0 */
    int ids[64];
    CHECK(vfs_list(a, ids, 64) == 1);                            /* cached: not yet in the list */
    CHECK(vfs_refresh(a) == 0);
    CHECK(vfs_list(a, ids, 64) == 2);
    int outside = vfs_lookup("/TESTVOL (sata0)/outside.bin");
    CHECK(outside > 0 && vfs_node(outside)->size == 123456 && same_content(outside, 123456, 3));
    CHECK(vfs_lookup("/TESTVOL (sata0)/Docs") == docs);          /* same id after the reload */
    CHECK(vfs_lookup("/TESTVOL (sata0)/Docs/A long note name.txt") == note);

    /* ---- copy: within a drive, with content ---- */
    int c1 = vfs_copy(outside, docs);
    CHECK(c1 > 0 && strcmp(vfs_node(c1)->name, "outside.bin") == 0 && same_content(c1, 123456, 3));
    int c2 = vfs_copy(outside, docs);
    CHECK(c2 > 0 && strcmp(vfs_node(c2)->name, "outside (2).bin") == 0 && same_content(c2, 123456, 3));
    CHECK(check_image(0) == 0);
    int c3 = vfs_copy(docs, a);                                  /* a folder, next to itself */
    CHECK(c3 > 0 && strcmp(vfs_node(c3)->name, "Docs (2)") == 0);
    CHECK(vfs_subtree_count(c3) == 3);
    CHECK(same_content(vfs_lookup("/TESTVOL (sata0)/Docs (2)/outside (2).bin"), 123456, 3));
    CHECK(vfs_copy(docs, docs) == -1 && vfs_last_error() == VFS_ERR_ARG);   /* not into itself */
    CHECK(vfs_copy(docs, c1) == -1);                              /* ... or into a file */
    CHECK(check_image(0) == 0);

    /* ---- copy between two drives ---- */
    int x = vfs_copy(docs, b);
    CHECK(x > 0 && vfs_node(x)->vol == 2 && vfs_subtree_count(x) == 3);
    CHECK(same_content(vfs_lookup("/TESTVOL (sata1)/Docs/outside.bin"), 123456, 3));
    CHECK(check_image(0) == 0 && check_image(1) == 0);

    /* ---- move: inside a drive the id stays; between drives the entry is copied and the old one deleted ---- */
    int sub = vfs_add(a, "Sub", 1, 0);
    CHECK(sub > 0);
    CHECK(vfs_move(c3, sub) == c3 && vfs_node(c3)->parent == sub);   /* a whole folder */
    CHECK(vfs_lookup("/TESTVOL (sata0)/Sub/Docs (2)/outside.bin") > 0);
    CHECK(vfs_move(outside, sub) == outside && vfs_node(outside)->parent == sub);
    CHECK(same_content(outside, 123456, 3));
    CHECK(vfs_move(sub, c3) == -1);                                  /* into its own content */
    CHECK(check_image(0) == 0);
    int moved = vfs_move(outside, b);                                /* to the other drive */
    CHECK(moved > 0 && vfs_node(moved)->vol == 2 && same_content(moved, 123456, 3));
    CHECK(vfs_lookup("/TESTVOL (sata0)/Sub/outside.bin") == -1);
    CHECK(check_image(0) == 0 && check_image(1) == 0);

    /* ---- rename ---- */
    CHECK(vfs_rename(note, "short.txt") == 0 && strcmp(vfs_node(note)->name, "short.txt") == 0);
    CHECK(vfs_rename(note, "SHORT.TXT") == 0);
    CHECK(vfs_rename(c1, "short.txt") == -1 && vfs_last_error() == VFS_ERR_EXISTS);
    CHECK(vfs_rename(c1, "a|b") == -1 && vfs_last_error() == VFS_ERR_BADNAME);
    CHECK(vfs_rename(a, "Other") == -1);                              /* a drive keeps its name */
    CHECK(vfs_rename(docs, "Documents") == 0);
    CHECK(vfs_lookup("/TESTVOL (sata0)/Documents/SHORT.TXT") == note);
    CHECK(check_image(0) == 0);

    /* ---- the RAM Disk and the drives do not mix ---- */
    int ram = vfs_lookup("/RAM Disk/Documents");
    CHECK(ram > 0);
    CHECK(vfs_copy(ram, a) == -1 && vfs_last_error() == VFS_ERR_UNSUPPORTED);
    CHECK(vfs_move(note, ram) == -1 && vfs_last_error() == VFS_ERR_UNSUPPORTED);
    CHECK(vfs_copy(note, ram) == -1);

    /* ---- delete: a folder with everything in it; the space comes back ---- */
    CHECK(vfs_volume_info(1, &vi) == 0);
    unsigned long long free_used = vi.free;
    int n_removed = vfs_remove(sub);
    CHECK(n_removed == 5);                                            /* Sub, Docs (2), 3 files */
    CHECK(vfs_lookup("/TESTVOL (sata0)/Sub") == -1);
    CHECK(vfs_volume_info(1, &vi) == 0 && vi.free > free_used + 200000);
    CHECK(check_image(0) == 0);
    CHECK(vfs_remove(a) == -1);                                       /* the drive itself stays */

    /* ---- a full drive: the copy fails and leaves nothing behind ---- */
    int fillhere = vfs_add(b, "fill", 1, 0);
    CHECK(fillhere > 0);
    int copies = 0;
    for (int i = 0; i < 200 && vfs_volume_info(2, &vi) == 0 && vi.free > 150000; i++) {
        int c = vfs_copy(moved, fillhere);
        if (c < 0) break;
        copies++;
    }
    CHECK(copies >= 3);
    int nodes_before = vfs_count();
    CHECK(vfs_copy(moved, fillhere) == -1 && vfs_last_error() == VFS_ERR_FULL);
    CHECK(vfs_count() == nodes_before);
    CHECK(check_image(1) == 0);

    /* ---- locking again ---- */
    CHECK(vfs_set_locked(1, 1) == 0);
    w0 = WRITES;
    CHECK(vfs_remove(note) == -1 && vfs_last_error() == VFS_ERR_RO);
    CHECK(vfs_rename(note, "zzz.txt") == -1 && vfs_last_error() == VFS_ERR_RO);
    CHECK(vfs_copy(moved, a) == -1 && vfs_last_error() == VFS_ERR_RO);
    CHECK(WRITES == w0);
    CHECK(vfs_lookup("/TESTVOL (sata0)/Documents/SHORT.TXT") == note);   /* reading still works */

    /* ---- exFAT and NTFS drives: browse, read, copy out; every change is refused (locked, or no write driver) ---- */
    {
        int ex = vfs_lookup("/EXVOL (nvme7)"), nt = vfs_lookup("/NTVOL (sata9)");
        CHECK(ex > 0 && nt > 0 && vfs_volume_of(ex) == 3 && vfs_volume_of(nt) == 4);
        CHECK(vfs_volume_info(3, &vi) == 0 && strcmp(vi.fs_name, "exFAT") == 0 && strcmp(vi.device, "SSD (NVMe)") == 0);
        CHECK(vi.writable == 1 && vi.lockable == 1 && vi.locked == 1 && vi.total == (unsigned long long)EX_CLUSTERS * 2048 && vi.free != (unsigned long long)-1);
        CHECK(vfs_volume_info(4, &vi) == 0 && strcmp(vi.fs_name, "NTFS") == 0 && vi.writable == 0 && vi.total > 0);
        CHECK(vi.lockable == 1 && vi.locked == 1);                                /* NTFS has the lock too; this disk has no write driver */

        int es = vfs_lookup("/EXVOL (nvme7)/small.txt"), eb = vfs_lookup("/EXVOL (nvme7)/Folder/big.bin");
        CHECK(es > 0 && eb > 0 && vfs_node(es)->size == sizeof EX_DATA_SMALL && vfs_node(eb)->size == sizeof EX_DATA_BIG);
        {
            unsigned char buf[sizeof EX_DATA_BIG];
            CHECK(vfs_read(eb, 0, buf, sizeof buf) == (int)sizeof buf && memcmp(buf, EX_DATA_BIG, sizeof buf) == 0);
            CHECK(vfs_read(es, 2990, buf, 100) == 10 && memcmp(buf, EX_DATA_SMALL + 2990, 10) == 0);
        }
        int ns = vfs_lookup("/NTVOL (sata9)/small.txt"), nb = vfs_lookup("/NTVOL (sata9)/big.bin"), np = vfs_lookup("/NTVOL (sata9)/packed.dat");
        CHECK(ns > 0 && nb > 0 && np > 0 && vfs_node(nb)->size == sizeof NT_BIG);
        {
            unsigned char buf[sizeof NT_BIG];
            CHECK(vfs_read(nb, 0, buf, sizeof buf) == (int)sizeof buf && memcmp(buf, NT_BIG, sizeof buf) == 0);
            CHECK(vfs_read(np, 0, buf, 10) == -1 && vfs_last_error() == VFS_ERR_NOTREADABLE);
        }

        /* changes are refused, nothing is written (the read-only disks have no write function anyway) */
        CHECK(vfs_add(ex, "x", 1, 0) == -1 && vfs_last_error() == VFS_ERR_RO);               /* exFAT: locked */
        CHECK(vfs_add(nt, "x", 0, 0) == -1 && vfs_last_error() == VFS_ERR_RO);
        CHECK(vfs_rename(es, "y.txt") == -1 && vfs_last_error() == VFS_ERR_RO);
        CHECK(vfs_remove(ns) == -1 && vfs_last_error() == VFS_ERR_RO && vfs_node(ns));
        CHECK(vfs_remove(vfs_lookup("/EXVOL (nvme7)/Folder")) == -1 && vfs_last_error() == VFS_ERR_RO);
        CHECK(vfs_copy(docs, ex) == -1 && vfs_last_error() == VFS_ERR_RO);        /* a copy INTO a locked drive */
        CHECK(vfs_copy(docs, nt) == -1 && vfs_last_error() == VFS_ERR_RO);        /* ... or an NTFS one */
        CHECK(vfs_move(es, a) == -1 && vfs_last_error() == VFS_ERR_RO);                       /* a move OUT to a locked FAT32 drive */
        CHECK(vfs_move(ns, a) == -1 && vfs_last_error() == VFS_ERR_RO);                       /* NTFS -> a locked FAT32 drive */
        CHECK(vfs_lookup("/TESTVOL (sata0)/small.txt") == -1);                                /* ... and nothing was copied */

        /* a copy OUT to a FAT32 drive works (that drive is locked now: unlock it first) */
        CHECK(vfs_set_locked(1, 0) == 0);
        int f1 = vfs_copy(eb, a);
        CHECK(f1 > 0 && strcmp(vfs_node(f1)->name, "big.bin") == 0 && same_content_buf(f1, EX_DATA_BIG, sizeof EX_DATA_BIG));
        int fold = vfs_copy(vfs_lookup("/EXVOL (nvme7)/Folder"), a);                          /* a whole folder */
        CHECK(fold > 0 && vfs_subtree_count(fold) == 1 && same_content_buf(vfs_lookup("/TESTVOL (sata0)/Folder/big.bin"), EX_DATA_BIG, sizeof EX_DATA_BIG));
        int f2 = vfs_copy(nb, a);
        CHECK(f2 > 0 && same_content_buf(f2, NT_BIG, sizeof NT_BIG));
        int f3 = vfs_copy(ns, a);
        CHECK(f3 > 0 && same_content_buf(f3, NT_SMALL, sizeof NT_SMALL));
        CHECK(check_image(0) == 0);
        /* a compressed file cannot be copied: the copy fails and leaves nothing behind */
        int before = vfs_count();
        CHECK(vfs_copy(np, a) == -1 && vfs_last_error() == VFS_ERR_NOTREADABLE && vfs_count() == before);
        CHECK(vfs_lookup("/TESTVOL (sata0)/packed.dat") == -1 && check_image(0) == 0);
        /* the folder listing of a read-only drive can be refreshed */
        CHECK(vfs_refresh(ex) == 0 && vfs_lookup("/EXVOL (nvme7)/small.txt") == es);
        /* the RAM Disk still cannot take files from a drive */
        CHECK(vfs_copy(es, vfs_lookup("/RAM Disk/Documents")) == -1 && vfs_last_error() == VFS_ERR_UNSUPPORTED);

        /* ---- exFAT: unlock it, then everything works: create, copy in (from FAT32 and NTFS), rename, move, delete ---- */
        CHECK(xfsck(&EXX) == 0);
        CHECK(vfs_set_locked(3, 0) == 0 && vfs_volume_info(3, &vi) == 0 && vi.locked == 0);
        int ef = vfs_add(ex, "New Folder", 1, 0);
        CHECK(ef > 0 && vfs_node(ef)->vol == 3);
        int in1 = vfs_copy(f1, ef);                                                           /* FAT32 -> exFAT (a 70000 byte file) */
        CHECK(in1 > 0 && same_content_buf(in1, EX_DATA_BIG, sizeof EX_DATA_BIG));
        int in2 = vfs_copy(nb, ef);                                                           /* NTFS -> exFAT */
        CHECK(in2 > 0 && same_content_buf(in2, NT_BIG, sizeof NT_BIG));
        int in3 = vfs_copy(vfs_lookup("/EXVOL (nvme7)/Folder"), ef);                          /* exFAT -> exFAT, a folder */
        CHECK(in3 > 0 && vfs_subtree_count(in3) == 1);
        CHECK(xfsck(&EXX) == 0);
        CHECK(vfs_rename(in2, "renamed copy.bin") == 0 && same_content_buf(in2, NT_BIG, sizeof NT_BIG));
        CHECK(vfs_move(in2, ex) == in2 && vfs_lookup("/EXVOL (nvme7)/renamed copy.bin") == in2);
        int moved_out = vfs_move(in2, a);                                                     /* exFAT -> FAT32 (copy + delete) */
        CHECK(moved_out > 0 && vfs_node(moved_out)->vol == 1 && same_content_buf(moved_out, NT_BIG, sizeof NT_BIG));
        CHECK(vfs_lookup("/EXVOL (nvme7)/renamed copy.bin") == -1);
        CHECK(vfs_remove(ef) > 0 && vfs_lookup("/EXVOL (nvme7)/New Folder") == -1);
        CHECK(xfsck(&EXX) == 0 && check_image(0) == 0);
        vfs_set_locked(3, 1);
    }

    /* ---- a writable NTFS drive: unlock it, then everything works, and the NTFS checker (which shares no code with
     * ntfs.c) finds the volume perfect after each step ---- */
    {
        static const unsigned long long RES[1][2] = {{0, 4}};
        nk_upcase = WB_UPC;
        int nw = vfs_lookup("/NTTEST (nvme8)");
        CHECK(nw > 0 && vfs_volume_of(nw) == 5);
        CHECK(vfs_volume_info(5, &vi) == 0 && strcmp(vi.fs_name, "NTFS") == 0 && vi.writable == 1 && vi.lockable == 1 && vi.locked == 1);
        CHECK(vfs_add(nw, "x", 0, 0) == -1 && vfs_last_error() == VFS_ERR_RO);              /* locked */
        CHECK(vfs_rename(vfs_lookup("/NTTEST (nvme8)/hello.txt"), "y.txt") == -1 && vfs_last_error() == VFS_ERR_RO);
        CHECK(nck_check(NTWIMG, NT_CLUSTERS - 1, RES, 1, 1) == 0);                            /* nothing was written */
        CHECK(vfs_set_locked(5, 0) == 0 && vfs_volume_info(5, &vi) == 0 && vi.locked == 0);

        int eb2 = vfs_lookup("/EXVOL (nvme7)/Folder/big.bin"), nsm = vfs_lookup("/NTVOL (sata9)/small.txt"), nbg = vfs_lookup("/NTVOL (sata9)/big.bin");
        CHECK(eb2 > 0 && nsm > 0 && nbg > 0);
        unsigned char hello[300];
        for (unsigned i = 0; i < sizeof hello; i++) hello[i] = wb_pat(1, i);
        CHECK(same_content_buf(vfs_lookup("/NTTEST (nvme8)/hello.txt"), hello, sizeof hello));   /* what was there before */

        int nf = vfs_add(nw, "New Folder", 1, 0);
        CHECK(nf > 0 && vfs_node(nf)->vol == 5 && vfs_is_under(nf, nw));
        CHECK(vfs_add(nw, "new folder", 1, 0) == -1 && vfs_last_error() == VFS_ERR_EXISTS);
        CHECK(vfs_add(nw, "bad:name", 0, 0) == -1 && vfs_last_error() == VFS_ERR_BADNAME);
        CHECK(nck_check(NTWIMG, NT_CLUSTERS - 1, RES, 1, 1) == 0);
        int c1 = vfs_copy(eb2, nf);                                                          /* exFAT -> NTFS: 70000 bytes */
        CHECK(c1 > 0 && vfs_node(c1)->vol == 5 && same_content_buf(c1, EX_DATA_BIG, sizeof EX_DATA_BIG));
        int c2 = vfs_copy(nsm, nf);                                                          /* NTFS -> NTFS: a resident file */
        CHECK(c2 > 0 && same_content_buf(c2, NT_SMALL, sizeof NT_SMALL));
        int c3 = vfs_copy(nbg, nf);
        CHECK(c3 > 0 && same_content_buf(c3, NT_BIG, sizeof NT_BIG));
        CHECK(nck_check(NTWIMG, NT_CLUSTERS - 1, RES, 1, 1) == 0);
        int cd = vfs_copy(docs, nw);                                                         /* FAT32 -> NTFS: a folder with its files */
        CHECK(cd > 0 && vfs_subtree_count(cd) == vfs_subtree_count(docs));
        CHECK(nck_check(NTWIMG, NT_CLUSTERS - 1, RES, 1, 1) == 0);
        CHECK(vfs_rename(c1, "renamed.bin") == 0 && same_content_buf(c1, EX_DATA_BIG, sizeof EX_DATA_BIG));
        CHECK(vfs_rename(c1, "RENAMED.BIN") == 0 && vfs_lookup("/NTTEST (nvme8)/New Folder/RENAMED.BIN") == c1);
        CHECK(vfs_move(c3, nw) == c3 && vfs_lookup("/NTTEST (nvme8)/big (2).bin") == c3 && same_content_buf(c3, NT_BIG, sizeof NT_BIG));
        CHECK(vfs_node(c3)->parent == nw);
        CHECK(nck_check(NTWIMG, NT_CLUSTERS - 1, RES, 1, 1) == 0);
        CHECK(vfs_refresh(nw) == 0 && vfs_lookup("/NTTEST (nvme8)/big (2).bin") == c3);          /* the folder on disk agrees */
        int out = vfs_move(c2, a);                                                           /* NTFS -> FAT32: copy, then delete */
        CHECK(out > 0 && vfs_node(out)->vol == 1 && same_content_buf(out, NT_SMALL, sizeof NT_SMALL));
        CHECK(vfs_lookup("/NTTEST (nvme8)/New Folder/small.txt") == -1);
        int back = vfs_copy(c1, a);                                                          /* NTFS -> FAT32 */
        CHECK(back > 0 && same_content_buf(back, EX_DATA_BIG, sizeof EX_DATA_BIG));
        CHECK(vfs_remove(nf) > 0 && vfs_lookup("/NTTEST (nvme8)/New Folder") == -1);
        CHECK(vfs_remove(c3) > 0 && vfs_remove(cd) > 0);
        CHECK(nck_check(NTWIMG, NT_CLUSTERS - 1, RES, 1, 1) == 0 && check_image(0) == 0);
        CHECK(vfs_refresh(nw) == 0 && vfs_lookup("/NTTEST (nvme8)/hello.txt") > 0);
        CHECK(vfs_remove(vfs_lookup("/NTTEST (nvme8)/attrlist.bin")) == -1 && vfs_last_error() == VFS_ERR_UNSUPPORTED);   /* not supported yet */
        vfs_set_locked(5, 1);
    }

    /* ---- a folder with more entries than the node table can hold: the rest is left out and flagged ---- */
    {
        Fat g;
        FatEntry e, many;
        CHECK(fat_mount(&g, 1) == 0);
        CHECK(fat_create(&g, 2, "Many", 1, &many) == 0);
        fat_batch(&g, 1);
        for (int i = 0; i < 2100; i++) {
            char nm[16];
            snprintf(nm, sizeof nm, "F%04d.TXT", i);
            if (fat_create(&g, many.first_cluster, nm, 0, &e) != 0) { CHECK(0); break; }
        }
        fat_batch(&g, 0);
        CHECK(vfs_refresh(b) == 0);
        int md = vfs_lookup("/TESTVOL (sata1)/Many");
        CHECK(md > 0 && !vfs_node(md)->truncated);
        static int big[VFS_MAX_NODES];
        int listed = vfs_list(md, big, VFS_MAX_NODES);
        CHECK(listed > 1500 && listed < 2100 && vfs_node(md)->truncated);
        CHECK(vfs_count() == VFS_MAX_NODES);
        CHECK(vfs_add(md, "one more", 0, 0) == -1 && vfs_last_error() == VFS_ERR_FULL);   /* no node left to show it */
        CHECK(check_image(1) == 0);
    }

    free(IMG[0]);
    free(IMG[1]);
    free(EXIMG); free(EXBITMAP); free(NTI.img); free(NTI.mft); free(NTWIMG);
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("volume tests passed\n");
    return 0;
}
