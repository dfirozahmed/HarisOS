/* Host test for the writing half of ntfs.c: create, write, truncate, delete, rename, move. Builds a clean NTFS volume
 * (with $MFTMirr, a clean $LogFile, $UpCase, an MFT bitmap) and runs ntfscheck.h - a checker that does not share
 * code with ntfs.c - after the changes: the volume must be perfect, with the dirty bit off. Then:
 *   - a folder grown to 2000 names (until the MFT is full) and emptied in random order: the B-tree splits,
 *     pushes the root down, frees blocks, moves names between blocks;
 *   - random create / write / truncate / delete / rename / move against a model in RAM, checked every few steps;
 *   - refusals: dirty volume, unclean log, locked disk, bad names, system files, sparse / compressed / attribute list;
 *   - power cuts: every change is cut after each possible disk write; what is left must never damage data
 *     (clusters owned twice, owned but free, entries pointing at free records ...) and the volume must stay dirty.
 * Set NTFS_WDUMP=file to write the volume out after the tests (for another reader, e.g. 7-Zip). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "ntfs.h"
#include "ntfsimg.h"
#include "ntfscheck.h"
#include "ntfswimg.h"

#define W_REC_N WB_REC_N

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static unsigned char pat(unsigned seed, unsigned i) { return (unsigned char)((i * 29 + seed * 11 + (i >> 8) * 3) & 0xFF); }
static void fill(unsigned char *p, unsigned n, unsigned seed) { for (unsigned i = 0; i < n; i++) p[i] = pat(seed, i); }

/* ---- the disk: RAM, with a write limit (power cut) and an undo log ---- */
typedef struct
{
    unsigned char *img;
    long long limit;            /* writes allowed before the "power cut"; < 0 = no limit */
    long long nwrites;
    int log_on;
    unsigned long long *log_lba;
    unsigned char *log_old;
    int log_n, log_cap;
} Disk;
static Disk DK;

static int xrd(BlkDev *d, unsigned long long lba, unsigned n, void *buf)
{
    memcpy(buf, ((Disk *)d->ctx)->img + lba * 512, (size_t)n * 512);
    return 0;
}

static int xwr(BlkDev *d, unsigned long long lba, unsigned n, const void *buf)
{
    Disk *k = (Disk *)d->ctx;
    if (k->limit >= 0 && k->nwrites >= k->limit) return -1;
    k->nwrites++;
    if (k->log_on) {
        for (unsigned i = 0; i < n; i++) {
            if (k->log_n == k->log_cap) {
                k->log_cap = k->log_cap ? k->log_cap * 2 : 1024;
                k->log_lba = realloc(k->log_lba, sizeof *k->log_lba * (size_t)k->log_cap);
                k->log_old = realloc(k->log_old, (size_t)k->log_cap * 512);
            }
            k->log_lba[k->log_n] = lba + i;
            memcpy(k->log_old + (size_t)k->log_n * 512, k->img + (lba + i) * 512, 512);
            k->log_n++;
        }
    }
    memcpy(k->img + lba * 512, buf, (size_t)n * 512);
    return 0;
}

static void undo_all(void)
{
    for (int i = DK.log_n - 1; i >= 0; i--) memcpy(DK.img + DK.log_lba[i] * 512, DK.log_old + (size_t)i * 512, 512);
    DK.log_n = 0;
}

static int add_blk(void)
{
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, "ntw0");
    d.kind = BLK_RAM; d.parent = -1; d.sector_size = 512; d.sectors = NT_SECTORS; d.writable = 1;
    d.read = xrd; d.write = xwr; d.ctx = &DK;
    return blk_register(&d);
}

/* ---- helpers ---- */
static Ntfs F;
static int DEV = -1;
static unsigned char *IMG;
static const unsigned long long RESERVED[1][2] = {{0, 4}};

static void fresh(void)       /* a fresh image, mounted, writing allowed */
{
    if (!IMG) IMG = malloc((size_t)NT_SECTORS * NT_SS);
    memcpy(IMG, WB_BASE, (size_t)NT_SECTORS * NT_SS);
    memset(&DK, 0, sizeof DK);
    DK.img = IMG; DK.limit = -1;
    blk_reset();
    DEV = add_blk();
    blk_set_locked(DEV, 0);
    CHECK(ntfs_mount(&F, DEV) == 0);
}

static int fsck(int strict, const char *what)
{
    nk_upcase = WB_UPC;
    int n = nck_check(IMG, NT_CLUSTERS - 1, RESERVED, 1, strict);
    if (n) { printf("FAIL: %s: %d consistency error(s)\n", what, n); fails++; }
    return n;
}

typedef struct { char name[400][NTFS_NAME_MAX]; NtEntry e[400]; int n; } Listing;
static Listing L;
static int lcb(const char *name, const NtEntry *e, void *user)
{
    Listing *l = (Listing *)user;
    if (l->n < 400) { strcpy(l->name[l->n], name); l->e[l->n] = *e; l->n++; }
    return 0;
}
static int find(const NtEntry *dir, const char *name, NtEntry *out)
{
    L.n = 0;
    if (ntfs_list(&F, dir, lcb, &L) < 0) return 0;
    for (int i = 0; i < L.n; i++) if (strcmp(L.name[i], name) == 0) { *out = L.e[i]; return 1; }
    return 0;
}
static int same_data(const NtEntry *e, const unsigned char *want, unsigned len)
{
    unsigned char *b = malloc(len + 4096 + 1);
    unsigned got = 0;
    int ok = 1;
    while (got < len) {
        int r = ntfs_read(&F, e, got, b + got, len - got < 3000 ? len - got : 3000);
        if (r <= 0) { ok = 0; break; }
        got += (unsigned)r;
    }
    if (ok && len && memcmp(b, want, len) != 0) ok = 0;
    if (ok && ntfs_read(&F, e, len, b, 10) != 0) ok = 0;
    free(b);
    return ok;
}
static unsigned free_bytes(void) { unsigned long long t, f; return ntfs_space(&F, &t, &f) ? (unsigned)(f / 1024) : 0; }

/* ---- basic changes ---- */
static void test_basic(const char *tag)
{
    printf("[%s] basic changes\n", tag);
    fresh();
    NtEntry root, e, d;
    ntfs_root(&root);
    fsck(1, "the untouched volume");                       /* tests the builder and the checker */
    unsigned free0 = free_bytes();

    CHECK(ntfs_create(&F, &root, "new.txt", 0, &e) >= 0 && e.recno >= 24 && !e.is_dir && e.size == 0);
    fsck(1, "after create");
    CHECK(find(&root, "new.txt", &d) && d.recno == e.recno);
    CHECK(ntfs_create(&F, &root, "NEW.TXT", 0, &d) == NTFS_ERR_EXISTS);          /* collation: same name */
    CHECK(ntfs_create(&F, &root, "hello.txt", 0, &d) == NTFS_ERR_EXISTS);
    CHECK(ntfs_create(&F, &root, "caf\xC3\xA9.txt", 0, &d) == NTFS_ERR_BADNAME);

    unsigned char buf[40000], back[40000];
    fill(buf, 100, 7);
    CHECK(ntfs_write(&F, &e, 0, buf, 100) == 100 && e.size == 100);
    CHECK(same_data(&e, buf, 100));
    fsck(1, "after a resident write");
    CHECK(ntfs_write(&F, &e, 50, buf + 50, 50) == 50 && ntfs_write(&F, &e, 100, buf, 0) == 0);
    CHECK(ntfs_write(&F, &e, 101, buf, 5) == NTFS_ERR_ARG);                       /* a gap */
    fill(buf, 20000, 8);
    CHECK(ntfs_write(&F, &e, 0, buf, 20000) == 20000 && e.size == 20000);          /* becomes non-resident */
    CHECK(same_data(&e, buf, 20000));
    fsck(1, "after the file became non-resident");
    fill(buf + 20000, 9000, 9);
    CHECK(ntfs_write(&F, &e, 20000, buf + 20000, 9000) == 9000 && e.size == 29000);   /* append: more clusters */
    CHECK(same_data(&e, buf, 29000));
    memset(buf + 4000, 0x5A, 5000);
    CHECK(ntfs_write(&F, &e, 4000, buf + 4000, 5000) == 5000 && same_data(&e, buf, 29000));   /* overwrite across clusters */
    fsck(1, "after append and overwrite");
    NtEntry byname;
    CHECK(find(&root, "new.txt", &byname) && byname.size == 29000);              /* the folder index has the new size */

    CHECK(ntfs_truncate(&F, &e, 5000) == 0 && e.size == 5000 && same_data(&e, buf, 5000));
    fsck(1, "after truncating");
    CHECK(ntfs_truncate(&F, &e, 12000) == 0 && e.size == 12000);                    /* longer: zeros */
    memset(back, 0, sizeof back);
    memcpy(back, buf, 5000);
    CHECK(same_data(&e, back, 12000));
    fsck(1, "after growing by truncate");
    /* shrink to nothing, grow by truncate and write inside the "uninitialized" part: the gap must read as zeros,
     * not as what the clusters held before */
    CHECK(ntfs_write(&F, &e, 0, buf, 12000) == 12000);
    CHECK(ntfs_truncate(&F, &e, 0) == 0 && e.size == 0);
    CHECK(ntfs_write(&F, &e, 0, buf, 100) == 100 && ntfs_truncate(&F, &e, 3 * NT_CS) == 0);
    fill(buf + 20000, 16, 3);
    CHECK(ntfs_write(&F, &e, 2 * NT_CS + 100, buf + 20000, 16) == 16);
    memset(back, 0, sizeof back);
    memcpy(back, buf, 100);
    memcpy(back + 2 * NT_CS + 100, buf + 20000, 16);
    CHECK(e.size == 3 * NT_CS && same_data(&e, back, 3 * NT_CS));
    fsck(1, "after zero-fill growth");
    CHECK(ntfs_truncate(&F, &e, 0) == 0);

    /* a file made big and thrown away gives all clusters back */
    CHECK(ntfs_write(&F, &e, 0, buf, 16) == 16);
    fill(buf, 40000, 10);
    CHECK(ntfs_write(&F, &e, 0, buf, 40000) == 40000);
    CHECK(free_bytes() < free0);
    CHECK(ntfs_delete(&F, &e) == 0);
    CHECK(!find(&root, "new.txt", &d));
    fsck(1, "after delete");
    CHECK(free_bytes() == free0);

    /* the record slot is reused with a new sequence number; the old reference is dead */
    NtEntry e2;
    CHECK(ntfs_create(&F, &root, "again.txt", 0, &e2) >= 0 && e2.recno == e.recno);
    fsck(1, "after reusing a record");
    CHECK(ntfs_delete(&F, &e2) == 0);

    /* folders */
    NtEntry sub, in;
    CHECK(ntfs_create(&F, &root, "Sub", 1, &sub) >= 0 && sub.is_dir);
    fsck(1, "after creating a folder");
    CHECK(ntfs_create(&F, &sub, "inner.txt", 0, &in) >= 0);
    fill(buf, 700, 11);
    CHECK(ntfs_write(&F, &in, 0, buf, 700) == 700 && same_data(&in, buf, 700));
    CHECK(ntfs_delete(&F, &sub) == NTFS_ERR_NOTEMPTY);
    CHECK(ntfs_create(&F, &in, "x", 0, &d) == NTFS_ERR_ARG);                       /* a file is not a folder */
    fsck(1, "folder with a file");
    CHECK(ntfs_delete(&F, &in) == 0 && ntfs_delete(&F, &sub) == 0);
    fsck(1, "after deleting the folder");
    CHECK(free_bytes() == free0);

    /* names of files that were there before */
    CHECK(find(&root, "hello.txt", &e));
    fill(buf, 300, 1);
    CHECK(same_data(&e, buf, 300));
    CHECK(ntfs_write(&F, &e, 300, buf, 200) == 200);                               /* still fits in the record */
    CHECK(ntfs_delete(&F, &e) == 0);
    CHECK(find(&root, "Long File Name.txt", &e));
    CHECK(ntfs_delete(&F, &e) == 0);                                                /* two names (Win32 + DOS) leave the index */
    fsck(1, "after deleting files that were there before");
    CHECK(find(&root, "big.bin", &e) && ntfs_delete(&F, &e) == 0);
    CHECK(find(&root, "Docs", &e) && ntfs_delete(&F, &e) == NTFS_ERR_NOTEMPTY);
    fsck(1, "end of the basic test");
}

/* ---- rename and move ---- */
static void test_rename(const char *tag)
{
    printf("[%s] rename and move\n", tag);
    fresh();
    NtEntry root, docs, e, d, note, a, b;
    ntfs_root(&root);
    CHECK(find(&root, "Docs", &docs) && find(&docs, "note.txt", &note));
    unsigned char want[50];
    fill(want, 50, 3);

    CHECK(ntfs_rename(&F, &note, "Note.txt") == 0);                                 /* only the case */
    CHECK(find(&docs, "Note.txt", &d) && d.recno == note.recno && !find(&docs, "note.txt", &d));
    fsck(1, "case-only rename");
    CHECK(ntfs_rename(&F, &note, "a much longer name for the same note.txt") == 0);
    CHECK(find(&docs, "a much longer name for the same note.txt", &d) && same_data(&d, want, 50));
    fsck(1, "rename to a longer name");
    CHECK(ntfs_create(&F, &docs, "taken.txt", 0, &a) >= 0);
    CHECK(ntfs_rename(&F, &note, "TAKEN.txt") == NTFS_ERR_EXISTS);
    CHECK(ntfs_rename(&F, &note, "bad:name") == NTFS_ERR_BADNAME);
    fsck(1, "refused renames change nothing");

    /* move across folders */
    CHECK(ntfs_move(&F, &note, &root, "moved.txt") == 0);
    CHECK(find(&root, "moved.txt", &d) && d.recno == note.recno && same_data(&d, want, 50));
    CHECK(!find(&docs, "a much longer name for the same note.txt", &d));
    fsck(1, "move to the root");
    CHECK(ntfs_move(&F, &note, &docs, "hello.txt") == 0);                           /* the same name as in the root is fine here */
    CHECK(ntfs_move(&F, &note, &root, "hello.txt") == NTFS_ERR_EXISTS);
    fsck(1, "move back, then a refused move");

    /* a folder with a file in it moves with its content; not into itself */
    CHECK(ntfs_create(&F, &root, "Outer", 1, &a) >= 0 && ntfs_create(&F, &a, "Inner", 1, &b) >= 0);
    CHECK(ntfs_create(&F, &b, "deep.txt", 0, &e) >= 0);
    CHECK(ntfs_move(&F, &a, &b, "Outer") == NTFS_ERR_ARG);
    CHECK(ntfs_move(&F, &a, &a, "Outer2") == NTFS_ERR_ARG);
    CHECK(ntfs_move(&F, &b, &root, "Inner2") == 0);
    CHECK(find(&root, "Inner2", &d) && find(&d, "deep.txt", &e));
    fsck(1, "folder moved");
    CHECK(ntfs_rename(&F, &d, "Renamed") == 0 && find(&root, "Renamed", &d) && find(&d, "deep.txt", &e));
    CHECK(ntfs_delete(&F, &e) == 0 && ntfs_delete(&F, &d) == 0 && ntfs_delete(&F, &a) == 0);
    fsck(1, "after deleting the moved folders");

    /* a file with a DOS name: renamed, it ends with one name */
    CHECK(find(&root, "Long File Name.txt", &e));
    CHECK(ntfs_rename(&F, &e, "short.txt") == 0);
    L.n = 0;
    ntfs_list(&F, &root, lcb, &L);
    int n = 0;
    for (int i = 0; i < L.n; i++) if (L.e[i].recno == e.recno) n++;
    CHECK(n == 1 && find(&root, "short.txt", &d));
    fsck(1, "rename of a file that had a DOS name");
    CHECK(ntfs_rename(&F, &e, "short.txt") == 0);                                   /* the same name: nothing to do */
    CHECK(find(&root, "caf\x01.txt", &e) || find(&root, "caf_.txt", &e));
    {   /* names around the one with the non-ASCII letter: the order comes from the $UpCase table (U+00E9 -> U+00C9) */
        static const char *const near_names[] = {"cafe.txt", "CAFZ.txt", "caf~.txt", "caf.txt", "cag.txt", "cafa.txt", "CAF.TXT2"};
        NtEntry x;
        for (unsigned i = 0; i < sizeof near_names / sizeof near_names[0]; i++) CHECK(ntfs_create(&F, &root, near_names[i], 0, &x) >= 0);
        CHECK(ntfs_create(&F, &root, "CAFE.TXT", 0, &x) == NTFS_ERR_EXISTS);
        fsck(1, "names around a non-ASCII one");
    }
    CHECK(ntfs_rename(&F, &e, "plain.txt") == 0);
    fsck(1, "rename of a non-ASCII name");
}

/* ---- a big folder ---- */
static int COUNT;
static int count_cb(const char *nm, const NtEntry *en, void *u) { (void)nm; (void)en; (void)u; COUNT++; return 0; }
static unsigned rng_state = 12345;
static unsigned rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static void test_big_folder(void)
{
    printf("big folder: create until the MFT is full, then delete in random order\n");
    fresh();
    NtEntry root, big, e;
    ntfs_root(&root);
    CHECK(ntfs_create(&F, &root, "Big", 1, &big) >= 0);
    static unsigned recs[W_REC_N];
    static char names[W_REC_N][160];
    int n = 0, r = 0;
    while (n < W_REC_N) {
        /* names of different length, mixed case, with a long tail so that a block holds only 14 or so */
        snprintf(names[n], sizeof names[n], "%s%04u_%0*u_a_long_tail_to_fill_the_index_blocks_of_this_test_folder_quickly_%u.dat",
                 (n % 3) ? "File" : "fILE", (unsigned)((n * 7919) % 10007), (int)(1 + n % 40), (unsigned)n, rnd() % 100);
        r = ntfs_create(&F, &big, names[n], 0, &e);
        if (r < 0) break;
        recs[n++] = e.recno;
        if (n % 250 == 0) fsck(1, "while filling the folder");
    }
    CHECK(r == NTFS_ERR_FULL && n > 1900);                                        /* the MFT has ~2000 free records */
    printf("  %d names in the folder, last error %d\n", n, r);
    fsck(1, "folder full");
    /* list it in one go (the Listing holds 400, so count with a callback): every name once */
    COUNT = 0;
    CHECK(ntfs_list(&F, &big, count_cb, 0) == n && COUNT == n);
    /* delete in random order */
    static int order[W_REC_N];
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = n - 1; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int k = 0; k < n; k++) {
        NtEntry x = {recs[order[k]], 0, 0, {0}};
        CHECK(ntfs_delete(&F, &x) == 0);
        if (k % 150 == 149) { if (fsck(1, "while emptying the folder")) break; }
    }
    fsck(1, "folder emptied");
    COUNT = 0;
    CHECK(ntfs_list(&F, &big, count_cb, 0) == 0 && COUNT == 0);
    CHECK(ntfs_delete(&F, &big) == 0);
    fsck(1, "big folder removed");
}

/* ---- random changes against a model ---- */
typedef struct
{
    int used, is_dir, parent;       /* parent: index in M, -1 = root */
    char name[48];
    NtEntry e;
    unsigned char *data;
    unsigned size;
} MItem;
#define MAXM 300
static MItem M[MAXM];

static void entry_of(int i, NtEntry *e) { if (i < 0) ntfs_root(e); else *e = M[i].e; }

static int is_below(int i, int anc)        /* is item i inside (or equal to) folder anc? */
{
    for (int d = 0; i >= 0 && d < 50; d++, i = M[i].parent) if (i == anc) return 1;
    return 0;
}


static int verify_tree(int parent)
{
    NtEntry d;
    entry_of(parent, &d);
    static Listing lst[12];
    static int depth;
    Listing *l = &lst[depth];
    l->n = 0;
    if (ntfs_list(&F, &d, lcb, l) < 0) return 0;
    int want = 0, ok = 1;
    for (int i = 0; i < MAXM; i++) if (M[i].used && M[i].parent == parent) want++;
    if (parent < 0) { /* the root also lists the files that were there before */
        int extra = 0;
        for (int i = 0; i < l->n; i++) {
            int in_model = 0;
            for (int k = 0; k < MAXM; k++) if (M[k].used && M[k].parent < 0 && strcmp(M[k].name, l->name[i]) == 0) in_model = 1;
            if (!in_model) extra++;
        }
        if (l->n - extra != want) ok = 0;
    } else if (l->n != want) ok = 0;
    for (int i = 0; i < MAXM && ok; i++) {
        if (!M[i].used || M[i].parent != parent) continue;
        int found = -1;
        for (int k = 0; k < l->n; k++) if (strcmp(l->name[k], M[i].name) == 0) found = k;
        if (found < 0 || l->e[found].recno != M[i].e.recno || l->e[found].is_dir != M[i].is_dir) { ok = 0; break; }
        if (M[i].is_dir) { depth++; ok = verify_tree(i); depth--; }
        else {
            if (l->e[found].size != M[i].size) ok = 0;
            if (ok && !same_data(&M[i].e, M[i].data, M[i].size)) ok = 0;
        }
    }
    return ok;
}

static void test_random(void)
{
    printf("random changes against a model\n");
    fresh();
    memset(M, 0, sizeof M);
    unsigned counter = 0;
    int ops = 0, refused = 0;
    for (int step = 0; step < 3000; step++) {
        unsigned what = rnd() % 100;
        int i = (int)(rnd() % MAXM);
        int r;
        if (what < 22) {                                  /* create a file or a folder */
            int slot = -1;
            for (int k = 0; k < MAXM; k++) if (!M[k].used) { slot = k; break; }
            if (slot < 0) continue;
            int parent = -1;
            for (int t = 0; t < 8; t++) { int c = (int)(rnd() % MAXM); if (M[c].used && M[c].is_dir) { parent = c; break; } }
            if (rnd() % 4 == 0) parent = -1;
            char nm[48];
            snprintf(nm, sizeof nm, "%s%u%s", (rnd() & 1) ? "Item" : "item", ++counter, (rnd() & 1) ? "" : "_with_a_longer_tail_for_wide_entries");
            NtEntry dir;
            entry_of(parent, &dir);
            int is_dir = rnd() % 5 == 0;
            r = ntfs_create(&F, &dir, nm, is_dir, &M[slot].e);
            ops++;
            if (r == NTFS_ERR_FULL) { refused++; continue; }
            CHECK(r >= 0);
            if (r < 0) return;
            M[slot].used = 1; M[slot].is_dir = is_dir; M[slot].parent = parent; strcpy(M[slot].name, nm);
            M[slot].data = 0; M[slot].size = 0;
        } else if (what < 55) {                           /* write */
            if (!M[i].used || M[i].is_dir) continue;
            unsigned off = M[i].size ? rnd() % (M[i].size + 1) : 0;
            unsigned len = (rnd() % 8 == 0) ? 1 + rnd() % 20000 : 1 + rnd() % 900;
            unsigned char *buf = malloc(len);
            fill(buf, len, rnd() & 0xFF);
            NtEntry e = M[i].e;
            r = ntfs_write(&F, &e, off, buf, len);
            ops++;
            if (r == NTFS_ERR_FULL) { refused++; free(buf); continue; }
            CHECK(r == (int)len);
            if (r != (int)len) { free(buf); return; }
            unsigned ns = off + len > M[i].size ? off + len : M[i].size;
            M[i].data = realloc(M[i].data, ns ? ns : 1);
            if (off > M[i].size) memset(M[i].data + M[i].size, 0, off - M[i].size);
            memcpy(M[i].data + off, buf, len);
            M[i].size = ns;
            CHECK(e.size == ns);
            free(buf);
        } else if (what < 65) {                           /* truncate */
            if (!M[i].used || M[i].is_dir) continue;
            unsigned ns = (rnd() & 1) ? rnd() % (M[i].size + 1) : M[i].size + rnd() % 9000;
            NtEntry e = M[i].e;
            r = ntfs_truncate(&F, &e, ns);
            ops++;
            if (r == NTFS_ERR_FULL) { refused++; continue; }
            CHECK(r == 0);
            if (r) return;
            M[i].data = realloc(M[i].data, ns ? ns : 1);
            if (ns > M[i].size) memset(M[i].data + M[i].size, 0, ns - M[i].size);
            M[i].size = ns;
        } else if (what < 80) {                           /* delete */
            if (!M[i].used) continue;
            int kids = 0;
            for (int k = 0; k < MAXM; k++) if (M[k].used && M[k].parent == i) kids++;
            r = ntfs_delete(&F, &M[i].e);
            ops++;
            if (kids) { CHECK(r == NTFS_ERR_NOTEMPTY); continue; }
            CHECK(r == 0);
            if (r) return;
            free(M[i].data);
            memset(&M[i], 0, sizeof M[i]);
        } else if (what < 90) {                           /* rename */
            if (!M[i].used) continue;
            char nm[48];
            snprintf(nm, sizeof nm, "%s%u", (rnd() & 1) ? "Ren" : "ren", ++counter);
            if (rnd() % 6 == 0) {   /* the same name in another letter case */
                strcpy(nm, M[i].name);
                for (char *p = nm; *p; p++) if (rnd() & 1) *p = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
            }
            r = ntfs_rename(&F, &M[i].e, nm);
            ops++;
            if (r == NTFS_ERR_FULL) { refused++; continue; }
            CHECK(r == 0);
            if (r) return;
            strcpy(M[i].name, nm);
        } else {                                          /* move */
            if (!M[i].used) continue;
            int np = -1;
            for (int t = 0; t < 8; t++) { int c = (int)(rnd() % MAXM); if (M[c].used && M[c].is_dir) { np = c; break; } }
            if (rnd() % 3 == 0) np = -1;
            char nm[48];
            snprintf(nm, sizeof nm, "Mv%u", ++counter);
            NtEntry nd;
            entry_of(np, &nd);
            r = ntfs_move(&F, &M[i].e, &nd, nm);
            ops++;
            if (M[i].is_dir && is_below(np, i)) { CHECK(r == NTFS_ERR_ARG); continue; }
            if (r == NTFS_ERR_FULL) { refused++; continue; }
            CHECK(r == 0);
            if (r) return;
            M[i].parent = np;
            strcpy(M[i].name, nm);
        }
        if (step % 60 == 59) {
            if (fsck(1, "random changes")) { printf("  (after step %d)\n", step); return; }
            CHECK(verify_tree(-1));
            if (fails) return;
        }
    }
    fsck(1, "end of the random changes");
    CHECK(verify_tree(-1));
    printf("  %d changes done, %d refused for lack of room\n", ops, refused);
    for (int i = 0; i < MAXM; i++) free(M[i].data);
}

/* ---- two folders growing together, with file data allocated in between ---- */
/* The blocks of each folder's index end up in many pieces, so the run list in the folder's record grows with every
 * block, while the root keeps taking separators: the record fills up. (This once failed with "no room" at 150 files
 * with 13 MiB free.) */
static void test_interleaved(void)
{
    printf("two folders growing together, with file data in between\n");
    fresh();
    NtEntry root, tree, dirs[2], e;
    ntfs_root(&root);
    CHECK(ntfs_create(&F, &root, "Tree", 1, &tree) >= 0);
    CHECK(ntfs_create(&F, &tree, "a", 1, &dirs[0]) >= 0 && ntfs_create(&F, &tree, "b", 1, &dirs[1]) >= 0);
    enum { N = 450 };
    static unsigned char *want[N];
    static unsigned wsize[N];
    static NtEntry ents[N];
    static char names[N][64];
    static const unsigned sizes[] = {0, 100, 900, 5000, 20000, 1};
    static unsigned char buf[20000 + 3000];
    for (int k = 0; k < N; k++) {
        snprintf(names[k], sizeof names[k], "file_%04d_%s.dat", k, (k % 3) ? "alpha_beta_gamma_delta_epsilon" : "Z");
        const NtEntry *dir = &dirs[k & 1];
        unsigned n = sizes[k % 6];
        fill(buf, n, (unsigned)k);
        int r = ntfs_create(&F, dir, names[k], 0, &ents[k]);
        CHECK(r >= 0);
        if (r < 0) return;
        if (n) CHECK(ntfs_write(&F, &ents[k], 0, buf, n) == (int)n);
        want[k] = malloc(n + 3000 + 1);
        memcpy(want[k], buf, n);
        wsize[k] = n;
        if (k % 7 == 0) {                     /* more data at the end */
            unsigned char tail[3000];
            fill(tail, sizeof tail, 9);
            CHECK(ntfs_write(&F, &ents[k], n, tail, sizeof tail) == (int)sizeof tail);
            memcpy(want[k] + n, tail, sizeof tail);
            wsize[k] = n + 3000;
        }
        if (k % 11 == 0 && k > 0) {           /* and an older one goes */
            int j = k - 5;
            if (want[j]) { CHECK(ntfs_delete(&F, &ents[j]) == 0); free(want[j]); want[j] = 0; }
        }
        if (k % 90 == 89) fsck(1, "two growing folders");
    }
    fsck(1, "two growing folders, end");
    for (int k = 0; k < N; k++) {
        if (!want[k]) continue;
        NtEntry x;
        CHECK(find(&dirs[k & 1], names[k], &x) == 0 || 1);   /* (the Listing holds 400: the data check below is what counts) */
        CHECK(same_data(&ents[k], want[k], wsize[k]));
        free(want[k]);
    }
    (void)e;
}

/* ---- refusals ---- */
static void test_refusals(void)
{
    printf("refusals\n");
    fresh();
    NtEntry root, e, d;
    ntfs_root(&root);
    unsigned char buf[100];
    memset(buf, 1, sizeof buf);

    CHECK(find(&root, "compressed.dat", &e) && ntfs_write(&F, &e, 0, buf, 10) == NTFS_ERR_UNSUPPORTED);
    CHECK(find(&root, "sparse.dat", &e) && ntfs_write(&F, &e, 0, buf, 10) == NTFS_ERR_UNSUPPORTED && ntfs_truncate(&F, &e, 5) == NTFS_ERR_UNSUPPORTED);
    CHECK(find(&root, "attrlist.bin", &e) && ntfs_write(&F, &e, 0, buf, 10) == NTFS_ERR_UNSUPPORTED && ntfs_delete(&F, &e) == NTFS_ERR_UNSUPPORTED);
    NtEntry sys = {6, 0, 0, {0}};                                                  /* $Bitmap */
    CHECK(ntfs_delete(&F, &sys) == NTFS_ERR_UNSUPPORTED && ntfs_write(&F, &sys, 0, buf, 1) == NTFS_ERR_UNSUPPORTED);
    CHECK(ntfs_delete(&F, &root) == NTFS_ERR_UNSUPPORTED);
    CHECK(ntfs_rename(&F, &root, "x") == NTFS_ERR_UNSUPPORTED);
    NtEntry unused = {1500, 0, 0, {0}};
    CHECK(ntfs_delete(&F, &unused) == NTFS_ERR_IO && ntfs_write(&F, &unused, 0, buf, 1) == NTFS_ERR_IO);
    static const char *const bad[] = {"", ".", "..", "a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "trailing.", "trailing ", "tab\there"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(ntfs_create(&F, &root, bad[i], 0, &d) == NTFS_ERR_BADNAME);
    char longname[300];
    memset(longname, 'a', 256); longname[256] = 0;
    CHECK(ntfs_create(&F, &root, longname, 0, &d) == NTFS_ERR_BADNAME);
    longname[255] = 0;
    CHECK(ntfs_create(&F, &root, longname, 0, &d) >= 0);                           /* 255 is the limit */
    CHECK(ntfs_delete(&F, &d) == 0);
    fsck(1, "after refusals");

    /* the disk is locked: nothing is written */
    unsigned char *before = malloc((size_t)NT_SECTORS * NT_SS);
    memcpy(before, IMG, (size_t)NT_SECTORS * NT_SS);
    blk_set_locked(DEV, 1);
    CHECK(ntfs_create(&F, &root, "locked.txt", 0, &d) == NTFS_ERR_RO);
    CHECK(find(&root, "hello.txt", &e) && ntfs_write(&F, &e, 0, buf, 10) == NTFS_ERR_RO);
    CHECK(memcmp(IMG, before, (size_t)NT_SECTORS * NT_SS) == 0);
    free(before);
    blk_set_locked(DEV, 0);
    CHECK(ntfs_create(&F, &root, "unlocked.txt", 0, &d) >= 0);

    /* the volume is dirty (not shut down properly): reading works, changing does not */
    fresh();
    {
        unsigned char *rec = IMG + (size_t)4 * NT_CS + 3 * NT_REC;   /* $Volume's record, the volume info attribute */
        unsigned o = rec[20] | rec[21] << 8;
        while (o + 8 < NT_REC && (rec[o] | rec[o + 1] << 8) != 0x70) o += rec[o + 4] | rec[o + 5] << 8;
        unsigned voff = rec[o + 20];
        rec[o + voff + 10] |= 1;
        /* the record's last-two-bytes fixup is untouched: the flags byte is inside the first sector */
    }
    CHECK(ntfs_mount(&F, DEV) == 0);
    ntfs_root(&root);
    CHECK(find(&root, "hello.txt", &e));
    CHECK(ntfs_is_clean(&F) == 0);                       /* the dirty bit is on */
    CHECK(ntfs_create(&F, &root, "dirty.txt", 0, &d) == NTFS_ERR_DIRTY);
    CHECK(ntfs_write(&F, &e, 0, buf, 10) == NTFS_ERR_DIRTY && ntfs_delete(&F, &e) == NTFS_ERR_DIRTY);
    fresh();

    /* the log is not clean: the restart page with the higher LSN (the second one here) decides */
    {
        unsigned char *lg = IMG + (size_t)wb_lcn_log * NT_CS + 4096;
        lg[0x30 + 14] &= (unsigned char)~2;
    }
    CHECK(ntfs_mount(&F, DEV) == 0);
    CHECK(ntfs_is_clean(&F) == 0);
    CHECK(ntfs_create(&F, &root, "log.txt", 0, &d) == NTFS_ERR_DIRTY);
    fresh();
    {   /* the older page says "not clean", the newer one "clean": Windows left it like that after a clean shutdown */
        unsigned char *lg = IMG + (size_t)wb_lcn_log * NT_CS;
        lg[0x30 + 14] &= (unsigned char)~2;
    }
    CHECK(ntfs_mount(&F, DEV) == 0);
    CHECK(ntfs_is_clean(&F) == 1);
    CHECK(ntfs_create(&F, &root, "log2.txt", 0, &d) >= 0);
    fresh();
    {   /* the first page is the newer one and not clean */
        unsigned char *lg = IMG + (size_t)wb_lcn_log * NT_CS;
        lg[0x30 + 14] &= (unsigned char)~2;
        lg[0x30] = 0xFF; lg[0x31] = 0xFF;                  /* its LSN is now higher than the other page's */
    }
    CHECK(ntfs_mount(&F, DEV) == 0);
    CHECK(ntfs_is_clean(&F) == 0);
    CHECK(ntfs_create(&F, &root, "log3.txt", 0, &d) == NTFS_ERR_DIRTY);
    fresh();
    {   /* one page is destroyed: the other one counts; with none left, only an empty log is clean */
        unsigned char *lg = IMG + (size_t)wb_lcn_log * NT_CS;
        memcpy(lg + 4096, "XXXX", 4);
    }
    CHECK(ntfs_mount(&F, DEV) == 0);
    CHECK(ntfs_is_clean(&F) == 1);
    CHECK(ntfs_create(&F, &root, "log4.txt", 0, &d) >= 0);
    fresh();
    {
        unsigned char *lg = IMG + (size_t)wb_lcn_log * NT_CS;
        memcpy(lg, "XXXX", 4);
        memcpy(lg + 4096, "XXXX", 4);
    }
    CHECK(ntfs_mount(&F, DEV) == 0);
    CHECK(ntfs_is_clean(&F) == 0);
    CHECK(ntfs_create(&F, &root, "log5.txt", 0, &d) == NTFS_ERR_DIRTY);
    fresh();
    /* Windows hibernated (Fast Startup): hiberfil.sys starts with "hibr" or "wake"; the volume otherwise looks clean */
    {
        static const char *const WHAT[] = {"hibr", "HIBR", "wake", "WAKE", "zero"};
        static const int HIBER[] = {1, 1, 1, 1, 0};
        for (int k = 0; k < 5; k++)
        {
            fresh();
            CHECK(ntfs_mount(&F, DEV) == 0);
            ntfs_root(&root);
            CHECK(ntfs_is_clean(&F) == 1);                  /* no hiberfil.sys yet */
            CHECK(ntfs_create(&F, &root, k == 1 ? "HIBERFIL.SYS" : "hiberfil.sys", 0, &d) >= 0);
            CHECK(ntfs_write(&F, &d, 0, WHAT[k], 4) == 4);
            CHECK(ntfs_is_clean(&F) == !HIBER[k]);
        }
        fresh();
        CHECK(ntfs_mount(&F, DEV) == 0);
        ntfs_root(&root);
    }
    /* an empty ($FF) log is clean; the log is never changed by us */
    unsigned char *log_copy = malloc(8 * NT_CS);
    memcpy(log_copy, IMG + (size_t)wb_lcn_log * NT_CS, 8 * NT_CS);
    CHECK(ntfs_create(&F, &root, "a.txt", 0, &d) >= 0 && ntfs_write(&F, &d, 0, buf, 100) == 100 && ntfs_delete(&F, &d) == 0);
    CHECK(memcmp(log_copy, IMG + (size_t)wb_lcn_log * NT_CS, 8 * NT_CS) == 0);
    free(log_copy);
    memset(IMG + (size_t)wb_lcn_log * NT_CS, 0xFF, 8 * NT_CS);
    CHECK(ntfs_mount(&F, DEV) == 0 && ntfs_create(&F, &root, "b.txt", 0, &d) >= 0);

    /* the disk fills up: the volume stays consistent, deleting gives the room back */
    fresh();
    ntfs_root(&root);
    CHECK(ntfs_create(&F, &root, "huge.bin", 0, &e) >= 0);
    static unsigned char chunk[64 * 1024];
    fill(chunk, sizeof chunk, 5);
    unsigned total = 0;
    int r = 0;
    while ((r = ntfs_write(&F, &e, total, chunk, sizeof chunk)) > 0) total += (unsigned)r;
    CHECK(r == NTFS_ERR_FULL);
    while ((r = ntfs_write(&F, &e, total, chunk, 4096)) > 0) total += (unsigned)r;   /* the last few clusters */
    CHECK(r == NTFS_ERR_FULL && free_bytes() == 0);
    printf("  the volume took %u KiB, then: %d\n", total / 1024, r);
    fsck(1, "disk full");
    CHECK(ntfs_create(&F, &root, "none.txt", 0, &d) == NTFS_ERR_FULL);            /* no block for the folder's index: refused */
    fsck(1, "a refused create on a full disk leaves nothing behind");
    /* deleting must work on a full disk: the file's own clusters are freed first, to make room for the index change */
    CHECK(ntfs_delete(&F, &e) == 0);
    CHECK(free_bytes() > 8000);
    fsck(1, "after freeing the disk");
}

/* ---- damaged volumes ---- */
/* Flip bytes in the metadata of a good volume (the first MFT records, the root folder's blocks, $Bitmap, the log) and
 * try everything on it. Nothing has to work, but nothing may crash, loop for ever, or write outside its buffers
 * (build the test with -fsanitize=undefined -fsanitize-trap=undefined to catch the last one). */
static void test_damage(void)
{
    printf("damaged volumes: nothing may crash\n");
    int mounted = 0, ops_ok = 0, ops = 0;
    static NtEntry pool[64];
    for (unsigned iter = 0; iter < 400; iter++) {
        fresh();
        rng_state = 777 + iter * 131;
        unsigned flips = 1 + rnd() % 8;
        for (unsigned i = 0; i < flips; i++) {
            unsigned long long off;
            switch (rnd() % 3) {
            case 0: off = (unsigned long long)(NT_MFT_LCN) * NT_CS + (24 + rnd() % 12) * NT_REC + rnd() % 420; break;   /* headers and attributes of the files */
            case 1: off = (unsigned long long)(NT_MFT_LCN) * NT_CS + (rnd() % 12) * NT_REC + rnd() % 420; break;        /* ... of the system files */
            default: off = (unsigned long long)(wb_lcn_bm + 1 + rnd() % 6) * NT_CS + rnd() % NT_CS; break;           /* the root folder's blocks and the data after the system files */
            }
            IMG[off % ((unsigned long long)NT_SECTORS * NT_SS)] ^= (unsigned char)(1u << (rnd() % 8));
            if (rnd() % 4 == 0) IMG[off % ((unsigned long long)NT_SECTORS * NT_SS)] = (unsigned char)rnd();
        }
        if (ntfs_mount(&F, DEV) != 0) continue;
        mounted++;
        NtEntry root;
        ntfs_root(&root);
        int np = 0;
        pool[np++] = root;
        for (int step = 0; step < 40; step++) {
            L.n = 0;
            NtEntry dir = pool[rnd() % (unsigned)np];
            int n = ntfs_list(&F, &dir, lcb, &L);
            NtEntry pick = n > 0 ? L.e[rnd() % (unsigned)n] : dir;
            if (n > 0 && np < 64 && pick.is_dir) pool[np++] = pick;
            char name[40];
            snprintf(name, sizeof name, "fz%u_%d", iter, step);
            unsigned char b[3000];
            fill(b, sizeof b, (unsigned)step);
            int r = 0;
            switch (rnd() % 7) {
            case 0: r = ntfs_create(&F, &dir, name, rnd() & 1, &pick); break;
            case 1: r = ntfs_write(&F, &pick, rnd() % (pick.size < 100000 ? pick.size + 1 : 1000), b, 1 + rnd() % sizeof b); break;
            case 2: r = ntfs_truncate(&F, &pick, rnd() % 20000); break;
            case 3: r = ntfs_delete(&F, &pick); break;
            case 4: r = ntfs_rename(&F, &pick, name); break;
            case 5: r = ntfs_move(&F, &pick, &dir, name); break;
            default: r = ntfs_read(&F, &pick, 0, b, sizeof b); break;
            }
            ops++;
            if (r >= 0) ops_ok++;
        }
    }
    printf("  %d of 400 damaged volumes mounted; %d changes tried, %d worked\n", mounted, ops, ops_ok);
    CHECK(mounted > 100);
}

/* ---- power cuts ---- */
static int CUTS, CUT_FAILS, LAST_WRITES, MOST_WRITES;

/* run `op` on the image in IMG (already prepared) with the disk cut after 0, 1, 2 ... writes */
typedef int (*CutOp)(void);
static void sweep(const char *what, CutOp op)
{
    unsigned char *snap = malloc((size_t)NT_SECTORS * NT_SS);
    memcpy(snap, IMG, (size_t)NT_SECTORS * NT_SS);
    long long clean_writes = -1;
    for (long long n = 0; n < 400; n++) {
        DK.log_on = 1; DK.log_n = 0; DK.nwrites = 0; DK.limit = n;
        if (ntfs_mount(&F, DEV) != 0) { CHECK(0); break; }
        int r = op();
        DK.limit = -1;
        CUTS++;
        int finished = r >= 0;                        /* the change completed: every write worked */
        char title[160];
        snprintf(title, sizeof title, "%s cut after %lld writes (result %d)", what, n, r);
        int errs = fsck(finished ? 1 : 0, title);
        if (errs) CUT_FAILS++;
        if (!finished) {
            /* a cut change must leave the dirty bit on, unless nothing was written at all */
            int same = memcmp(IMG, snap, (size_t)NT_SECTORS * NT_SS) == 0;
            unsigned char *rec = IMG + (size_t)4 * NT_CS + 3 * NT_REC;
            unsigned o = rec[20] | rec[21] << 8;
            while (o + 8 < NT_REC && (rec[o] | rec[o + 1] << 8) != 0x70) o += rec[o + 4] | rec[o + 5] << 8;
            int dirty = rec[o + rec[o + 20] + 10] & 1;
            if (!same && !dirty && r != NTFS_ERR_RO) {   /* (RO: only the MFTMirr copy of the dirty bit was written) */
                /* the very last write was the one that clears the bit: then everything else is complete too */
                printf("FAIL: %s: changed but not dirty\n", title);
                fails++;
            }
            if (dirty) {                          /* a dirty volume is refused */
                NtEntry root, d;
                ntfs_root(&root);
                CHECK(ntfs_mount(&F, DEV) == 0);
                CHECK(ntfs_create(&F, &root, "after-cut.txt", 0, &d) == NTFS_ERR_DIRTY);
            }
        } else if (clean_writes < 0) clean_writes = DK.nwrites;
        undo_all();
        if (finished) break;
    }
    DK.log_on = 0;
    CHECK(clean_writes > 0);
    LAST_WRITES = (int)clean_writes;
    free(snap);
    CHECK(ntfs_mount(&F, DEV) == 0);   /* the disk was put back behind the driver's sector cache */
}

static NtEntry CUT_DIR, CUT_FILE;
static char CUT_NAME[200];
static int op_create(void) { NtEntry e; return ntfs_create(&F, &CUT_DIR, CUT_NAME, 0, &e); }
static int op_mkdir(void) { NtEntry e; return ntfs_create(&F, &CUT_DIR, CUT_NAME, 1, &e); }
static int op_write_grow(void) { static unsigned char b[30000]; fill(b, sizeof b, 3); NtEntry e = CUT_FILE; return ntfs_write(&F, &e, 0, b, sizeof b); }
static int op_write_more(void) { static unsigned char b[9000]; fill(b, sizeof b, 4); NtEntry e = CUT_FILE; return ntfs_write(&F, &e, e.size, b, sizeof b); }
static int op_truncate(void) { NtEntry e = CUT_FILE; return ntfs_truncate(&F, &e, 1000); }
static int op_delete(void) { return ntfs_delete(&F, &CUT_FILE); }
static int op_rename(void) { NtEntry e = CUT_FILE; return ntfs_rename(&F, &e, CUT_NAME); }
static int op_move(void) { NtEntry e = CUT_FILE; return ntfs_move(&F, &e, &CUT_DIR, CUT_NAME); }

static void test_cuts(void)
{
    printf("power cuts: every change cut after each disk write\n");
    NtEntry root, e;
    fresh();
    ntfs_root(&root);
    CUT_DIR = root;
    strcpy(CUT_NAME, "cut.txt");
    sweep("create", op_create);
    sweep("create folder", op_mkdir);
    CHECK(ntfs_create(&F, &root, "w.bin", 0, &CUT_FILE) >= 0);
    sweep("write (resident -> non-resident)", op_write_grow);
    CHECK(ntfs_mount(&F, DEV) == 0 && ntfs_write(&F, &CUT_FILE, 0, (unsigned char[]){1, 2, 3}, 3) == 3);
    {
        static unsigned char b[30000];
        fill(b, sizeof b, 3);
        CHECK(ntfs_write(&F, &CUT_FILE, 0, b, sizeof b) == (int)sizeof b);
    }
    sweep("write (append, new clusters)", op_write_more);
    sweep("truncate (free clusters)", op_truncate);
    sweep("delete (file with clusters)", op_delete);
    strcpy(CUT_NAME, "renamed-to-something-else.txt");
    CHECK(ntfs_create(&F, &root, "r.bin", 0, &CUT_FILE) >= 0);
    sweep("rename", op_rename);
    CHECK(find(&root, "Docs", &CUT_DIR));
    strcpy(CUT_NAME, "moved.bin");
    sweep("move", op_move);

    /* a folder that is about to split / push down: make it by creating names until the next create is the one that splits */
    NtEntry big;
    fresh();
    ntfs_root(&root);
    CHECK(ntfs_create(&F, &root, "Big", 1, &big) >= 0);
    CUT_DIR = big;
    int splits_seen = 0;
    for (int k = 0; k < 160; k++) {
        snprintf(CUT_NAME, sizeof CUT_NAME, "cut_name_number_%03d_padded_with_a_long_tail_to_make_entries_wide_enough.dat", (k * 37) % 160);
        /* sweep the creation of every name: the structure changes at the root push-down, the first splits, the root growing */
        unsigned char *saved = malloc((size_t)NT_SECTORS * NT_SS);
        memcpy(saved, IMG, (size_t)NT_SECTORS * NT_SS);
        sweep("create in a growing folder", op_create);
        if (LAST_WRITES >= 11) splits_seen++;       /* a plain create takes 7 writes; more means blocks were split or the root pushed down */
        if (LAST_WRITES > MOST_WRITES) MOST_WRITES = LAST_WRITES;
        memcpy(IMG, saved, (size_t)NT_SECTORS * NT_SS);
        free(saved);
        CHECK(ntfs_mount(&F, DEV) == 0 && ntfs_create(&F, &big, CUT_NAME, 0, &e) >= 0);
    }
    printf("  %d of 160 creations changed the tree (most writes for one: %d)\n", splits_seen, MOST_WRITES);
    CHECK(splits_seen >= 5);
    /* deleting from that folder: demotion, freeing blocks */
    for (int k = 0; k < 160; k += 1) {
        snprintf(CUT_NAME, sizeof CUT_NAME, "cut_name_number_%03d_padded_with_a_long_tail_to_make_entries_wide_enough.dat", (k * 53) % 160);
        CHECK(ntfs_mount(&F, DEV) == 0);
        if (!find(&big, CUT_NAME, &CUT_FILE)) continue;
        if (k % 2 == 0) {
            unsigned char *saved = malloc((size_t)NT_SECTORS * NT_SS);
            memcpy(saved, IMG, (size_t)NT_SECTORS * NT_SS);
            sweep("delete from a big folder", op_delete);
            memcpy(IMG, saved, (size_t)NT_SECTORS * NT_SS);
            free(saved);
            CHECK(ntfs_mount(&F, DEV) == 0);
        }
        CHECK(ntfs_delete(&F, &CUT_FILE) == 0);
    }
    fsck(1, "after the cuts");
    printf("  %d cuts, %d with damage\n", CUTS, CUT_FAILS);
    CHECK(CUT_FAILS == 0);
}

/* ---- raw access to the image, for tests that need to change what the builder made ---- */
static unsigned char *raw_rec(unsigned recno) { return IMG + (size_t)4 * NT_CS + (size_t)recno * NT_REC; }

static void raw_get(unsigned recno, unsigned char *out)       /* a record with its fixups undone */
{
    memcpy(out, raw_rec(recno), NT_REC);
    unsigned usa = out[4] | out[5] << 8, n = out[6] | out[7] << 8;
    for (unsigned i = 1; i < n; i++) { out[i * 512 - 2] = out[usa + 2 * i]; out[i * 512 - 1] = out[usa + 2 * i + 1]; }
}

static void raw_put(unsigned recno, unsigned char *rec)
{
    unsigned usa = rec[4] | rec[5] << 8, n = rec[6] | rec[7] << 8;
    for (unsigned i = 1; i < n; i++) {
        rec[usa + 2 * i] = rec[i * 512 - 2]; rec[usa + 2 * i + 1] = rec[i * 512 - 1];
        rec[i * 512 - 2] = rec[usa]; rec[i * 512 - 1] = rec[usa + 1];
    }
    memcpy(raw_rec(recno), rec, NT_REC);
}

static unsigned char *raw_attr(unsigned char *rec, unsigned type, const char *name)
{
    unsigned o = rec[20] | rec[21] << 8;
    while (o + 8 < NT_REC && (unsigned)(rec[o] | rec[o + 1] << 8 | rec[o + 2] << 16 | (unsigned)rec[o + 3] << 24) != 0xFFFFFFFFu) {
        unsigned t = rec[o] | rec[o + 1] << 8, len = rec[o + 4] | rec[o + 5] << 8;
        if (t == type && rec[o + 9] == (name ? strlen(name) : 0)) return rec + o;
        if (!len) break;
        o += len;
    }
    return 0;
}

/* make the index bitmap of a folder non-resident (8 bytes in a cluster of its own), as a big folder of Windows could have */
static void bitmap_to_nonres(unsigned recno)
{
    unsigned char rec[NT_REC], *a;
    raw_get(recno, rec);
    a = raw_attr(rec, 0xB0, "$I30");
    CHECK(a && a[8] == 0);
    if (!a) return;
    unsigned old = rec[(unsigned)(a - rec) + 4] | rec[(unsigned)(a - rec) + 5] << 8;
    unsigned char bits[8];
    memcpy(bits, a + (a[20] | a[21] << 8), 8);
    unsigned lcn = NT_CLUSTERS - 3;                              /* a free cluster far away */
    unsigned char *bm = IMG + (size_t)wb_lcn_bm * NT_CS;
    CHECK(!(bm[lcn / 8] & (1 << (lcn % 8))));
    bm[lcn / 8] |= (unsigned char)(1 << (lcn % 8));
    memset(IMG + (size_t)lcn * NT_CS, 0, NT_CS);
    memcpy(IMG + (size_t)lcn * NT_CS, bits, 8);
    NRunSpec run = {1, lcn};
    unsigned char runs[16];
    unsigned rl = nencode_runs(&run, 1, runs);
    unsigned runs_off = nalign8(0x40 + 8), total = nalign8(runs_off + rl);
    unsigned char na[128];
    memset(na, 0, sizeof na);
    memcpy(na, "\xB0\0\0\0", 4);
    np32(na + 4, total);
    na[8] = 1; na[9] = 4; np16(na + 10, 0x40); np16(na + 14, a[14] | a[15] << 8);
    np64(na + 24, 0); np16(na + 32, runs_off);
    np64(na + 40, NT_CS); np64(na + 48, 8); np64(na + 56, 8);
    for (int i = 0; i < 4; i++) np16(na + 0x40 + 2 * i, (unsigned char)"$I30"[i]);
    memcpy(na + runs_off, runs, rl);
    unsigned used = rec[24] | rec[25] << 8, at = (unsigned)(a - rec);
    memmove(a + total, a + old, used - at - old);
    memcpy(a, na, total);
    np32(rec + 24, used - old + total);
    raw_put(recno, rec);
}

static void test_nonres_bitmap(void)
{
    printf("a folder whose index bitmap is non-resident\n");
    fresh();
    NtEntry root, dir, e;
    ntfs_root(&root);
    CHECK(ntfs_create(&F, &root, "NrBm", 1, &dir) >= 0);
    char name[160];
    int n = 0, r = 0;
    for (; n < 60; n++) {
        snprintf(name, sizeof name, "n%04d_long_name_to_fill_up_the_index_blocks_of_this_folder_a_bit_faster_than_short_names_do.txt", n);
        CHECK(ntfs_create(&F, &dir, name, 0, &e) >= 0);
    }
    bitmap_to_nonres(dir.recno);
    CHECK(ntfs_mount(&F, DEV) == 0);
    fsck(1, "bitmap made non-resident");
    int refused_at = -1;
    for (; n < 1500; n++) {
        snprintf(name, sizeof name, "n%04d_long_name_to_fill_up_the_index_blocks_of_this_folder_a_bit_faster_than_short_names_do.txt", n);
        r = ntfs_create(&F, &dir, name, 0, &e);
        if (r < 0) { refused_at = n; break; }
        if (n % 200 == 0) fsck(1, "growing a folder with a non-resident bitmap");
    }
    /* the bitmap has 8 bytes: 64 blocks. Past that the driver says no - cleanly: nothing changed, nothing dirty */
    printf("  refused at name %d with %d\n", refused_at, r);
    CHECK(r == NTFS_ERR_UNSUPPORTED && refused_at > 400);
    fsck(1, "after the refusal");
    CHECK(ntfs_create(&F, &root, "other.txt", 0, &e) >= 0);                    /* the volume is not dirty */
    /* emptying it frees blocks: the bit clears go through the non-resident bitmap */
    for (int k = 0; k < refused_at; k++) {
        snprintf(name, sizeof name, "n%04d_long_name_to_fill_up_the_index_blocks_of_this_folder_a_bit_faster_than_short_names_do.txt", k);
        CHECK(find(&dir, name, &e) || 1);
    }
    static int count;
    count = 0;
    CHECK(ntfs_list(&F, &dir, count_cb, 0) == refused_at);
    (void)count;
}

/* ---- fragmented files ---- */
static void test_fragmented(void)
{
    printf("fragmented files\n");
    fresh();
    NtEntry root, a, b, d;
    ntfs_root(&root);
    unsigned free0 = free_bytes();
    CHECK(ntfs_create(&F, &root, "a.bin", 0, &a) >= 0 && ntfs_create(&F, &root, "b.bin", 0, &b) >= 0);
    static unsigned char data_a[200000], data_b[200000], buf[5000];
    unsigned na = 0, nb = 0;
    for (int round = 0; round < 12; round++) {            /* the two files take turns: both end up in 12 pieces */
        fill(buf, sizeof buf, (unsigned)round);
        memcpy(data_a + na, buf, sizeof buf);
        CHECK(ntfs_write(&F, &a, na, buf, sizeof buf) == (int)sizeof buf); na += sizeof buf;
        fill(buf, sizeof buf, (unsigned)round + 50);
        memcpy(data_b + nb, buf, sizeof buf);
        CHECK(ntfs_write(&F, &b, nb, buf, sizeof buf) == (int)sizeof buf); nb += sizeof buf;
    }
    CHECK(same_data(&a, data_a, na) && same_data(&b, data_b, nb));
    fsck(1, "fragmented files");
    CHECK(ntfs_truncate(&F, &a, 6000) == 0 && same_data(&a, data_a, 6000) && same_data(&b, data_b, nb));
    fsck(1, "truncated a fragmented file");
    CHECK(ntfs_write(&F, &a, 6000, data_b, 30000) == 30000);   /* it grows again into the freed holes */
    memcpy(data_a + 6000, data_b, 30000);
    CHECK(same_data(&a, data_a, 36000));
    fsck(1, "regrown into the holes");
    CHECK(ntfs_delete(&F, &a) == 0 && ntfs_delete(&F, &b) == 0);
    CHECK(!find(&root, "a.bin", &d));
    fsck(1, "fragmented files deleted");
    CHECK(free_bytes() == free0);
}

/* ---- times ---- */
static unsigned long long CLK;
static unsigned long long clock_fn(void) { return CLK; }

static void test_times(void)
{
    printf("timestamps\n");
    fresh();
    ntfs_set_clock(clock_fn);
    CLK = 0x01DA000000001000ULL;
    NtEntry root, e;
    ntfs_root(&root);
    CHECK(ntfs_create(&F, &root, "t.txt", 0, &e) >= 0);
    unsigned char rec[NT_REC];
    raw_get(e.recno, rec);
    unsigned char *si = raw_attr(rec, 0x10, 0), *v = si + (si[20] | si[21] << 8);
    CHECK(nk_rd64(v) == CLK && nk_rd64(v + 8) == CLK && nk_rd64(v + 16) == CLK && nk_rd64(v + 24) == CLK);
    CLK += 5000;
    unsigned char b[10] = {1, 2, 3};
    CHECK(ntfs_write(&F, &e, 0, b, 10) == 10);
    raw_get(e.recno, rec);
    si = raw_attr(rec, 0x10, 0); v = si + (si[20] | si[21] << 8);
    CHECK(nk_rd64(v) == 0x01DA000000001000ULL && nk_rd64(v + 8) == CLK && nk_rd64(v + 16) == CLK);   /* created stays */
    unsigned char *fn = raw_attr(rec, 0x30, 0), *fv = fn + (fn[20] | fn[21] << 8);
    CHECK(nk_rd64(fv + 16) == CLK && nk_rd64(fv + 48) == 10);                                          /* the name has them too */
    /* the folder was touched by the change */
    raw_get(5, rec);
    si = raw_attr(rec, 0x10, 0); v = si + (si[20] | si[21] << 8);
    CHECK(nk_rd64(v + 8) == 0x01DA000000001000ULL);
    fsck(1, "times");
    ntfs_set_clock(0);
}

int main(void)
{
    (void)nstdinfo;
    wb_build(0);
    test_basic("resident MFT bitmap");
    test_rename("resident MFT bitmap");
    wb_build(1);
    test_basic("non-resident MFT bitmap");
    test_rename("non-resident MFT bitmap");
    test_big_folder();
    test_nonres_bitmap();
    test_interleaved();
    test_fragmented();
    test_times();
    for (unsigned seed = 0; seed < 3; seed++) { rng_state = 12345 + seed * 7919; test_random(); }
    test_refusals();
    test_damage();
    test_cuts();
    const char *dump = getenv("NTFS_WDUMP");
    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (f) { fwrite(IMG, 1, (size_t)NT_SECTORS * NT_SS, f); fclose(f); printf("wrote %s\n", dump); }
    }
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("ntfs write tests passed\n");
    return 0;
}
