/* Host test for vfs.c: add rules, sorting, paths, lookup. */
#include <stdio.h>
#include <string.h>
#include "vfs.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- a tiny in-memory drive, to test vfs_write / vfs_save ---- */
typedef struct
{
    int used;
    char name[VFS_NAME_MAX];
    unsigned char data[256];
    unsigned size;
} FFile;
static FFile FF[8];
static int FF_FULL; /* 1 = every write fails with "drive full" */

static int ff_list(void *ctx, int dir)
{
    (void)ctx;
    for (int i = 0; i < 8; i++)
        if (FF[i].used)
        {
            unsigned char fs[VFS_FS_BYTES] = {(unsigned char)i};
            vfs_attach(dir, FF[i].name, 0, FF[i].size, fs);
        }
    return 0;
}
static int ff_create(void *ctx, VfsNode *parent, const char *name, int is_dir, unsigned char *fs_out)
{
    (void)ctx; (void)parent;
    if (is_dir)
        return VFS_ERR_UNSUPPORTED;
    for (int i = 0; i < 8; i++)
        if (!FF[i].used)
        {
            memset(&FF[i], 0, sizeof FF[i]);
            FF[i].used = 1;
            strcpy(FF[i].name, name);
            fs_out[0] = (unsigned char)i;
            return 0;
        }
    return VFS_ERR_FULL;
}
static int FF_NODELETE, FF_NOTRUNC; /* the drive cannot delete ordinary files / cannot shorten files */
static int ff_remove(void *ctx, const VfsNode *n)
{
    (void)ctx;
    const char *nm = FF[n->fs[0]].name;
    if (FF_NODELETE && strlen(nm) > 4 && strcmp(nm + strlen(nm) - 4, ".tmp") != 0)
        return VFS_ERR_UNSUPPORTED;
    FF[n->fs[0]].used = 0;
    return 0;
}
static int ff_truncate(void *ctx, VfsNode *n, unsigned size)
{
    (void)ctx;
    if (FF_NOTRUNC)
        return VFS_ERR_UNSUPPORTED;
    FF[n->fs[0]].size = size;
    n->size = size;
    return 0;
}
static int ff_rename(void *ctx, VfsNode *n, const char *name) { (void)ctx; strcpy(FF[n->fs[0]].name, name); return 0; }
static int ff_read(void *ctx, const VfsNode *n, unsigned off, void *buf, unsigned len)
{
    (void)ctx;
    const FFile *f = &FF[n->fs[0]];
    if (off >= f->size)
        return 0;
    if (len > f->size - off)
        len = f->size - off;
    memcpy(buf, f->data + off, len);
    return (int)len;
}
static int ff_write(void *ctx, VfsNode *n, unsigned off, const void *buf, unsigned len)
{
    (void)ctx;
    FFile *f = &FF[n->fs[0]];
    if (FF_FULL || off + len > sizeof f->data)
        return VFS_ERR_FULL;
    memcpy(f->data + off, buf, len);
    if (off + len > f->size)
        f->size = off + len;
    n->size = f->size;
    return (int)len;
}
static void ff_info(void *ctx, VfsVolInfo *out) { (void)ctx; memset(out, 0, sizeof *out); out->writable = 1; }
static const VfsBackend FAKE = {ff_list, ff_create, ff_remove, ff_rename, 0, ff_read, ff_write, ff_info, 0, 0, ff_truncate};

static int ff_count(void)
{
    int n = 0;
    for (int i = 0; i < 8; i++)
        n += FF[i].used;
    return n;
}

static void test_save(void)
{
    vfs_init();
    int ram = vfs_volume_node(0);
    CHECK(vfs_mount(&FAKE, 0, "Fake") == 1);
    int root = vfs_volume_node(1);
    char buf[300];

    /* a new file */
    int a = vfs_save(root, "a.txt", "hello", 5);
    CHECK(a > 0 && vfs_node(a)->size == 5 && ff_count() == 1);
    CHECK(vfs_read(a, 0, buf, sizeof buf) == 5 && memcmp(buf, "hello", 5) == 0);

    /* replacing it with shorter text: no leftover bytes, no leftover temporary file */
    int b = vfs_save(root, "a.txt", "hi", 2);
    CHECK(b > 0 && vfs_node(b)->size == 2 && ff_count() == 1 && vfs_lookup("/Fake/a.txt") == b);
    CHECK(vfs_read(b, 0, buf, sizeof buf) == 2 && memcmp(buf, "hi", 2) == 0);
    CHECK(vfs_lookup("/Fake/a.txt.tmp") < 0);

    /* longer text, and an empty file */
    int c = vfs_save(root, "a.txt", "0123456789", 10);
    CHECK(c > 0 && vfs_node(c)->size == 10 && ff_count() == 1);
    int e = vfs_save(root, "empty.txt", "", 0);
    CHECK(e > 0 && vfs_node(e)->size == 0 && ff_count() == 2);

    /* a drive that is full: the old file survives, a new file is not left half-written */
    FF_FULL = 1;
    CHECK(vfs_save(root, "a.txt", "zzzz", 4) == -1 && vfs_last_error() == VFS_ERR_FULL);
    int keep = vfs_lookup("/Fake/a.txt");
    CHECK(keep > 0 && vfs_node(keep)->size == 10 && vfs_lookup("/Fake/a.txt.tmp") < 0 && ff_count() == 2);
    CHECK(vfs_read(keep, 0, buf, sizeof buf) == 10 && memcmp(buf, "0123456789", 10) == 0);
    CHECK(vfs_save(root, "b.txt", "data", 4) == -1 && vfs_lookup("/Fake/b.txt") < 0 && ff_count() == 2);
    FF_FULL = 0;

    /* a text bigger than the drive allows */
    CHECK(vfs_save(root, "big.txt", buf, 300) == -1 && vfs_lookup("/Fake/big.txt") < 0 && ff_count() == 2);

    /* a leftover .tmp from an earlier cut-short save is replaced */
    int stale = vfs_add(root, "a.txt.tmp", 0, 0);
    CHECK(stale > 0 && vfs_write(stale, 0, "old", 3) == 3);
    int f = vfs_save(root, "a.txt", "fresh", 5);
    CHECK(f > 0 && vfs_lookup("/Fake/a.txt.tmp") < 0 && ff_count() == 2);

    /* a drive that cannot delete that file (NTFS: hard links ...) but can shorten it: the text goes over the old file */
    FF_NODELETE = 1;
    int old = vfs_lookup("/Fake/a.txt");
    int g1 = vfs_save(root, "a.txt", "tiny", 4);                              /* shorter: written, then cut */
    CHECK(g1 == old && vfs_node(g1)->size == 4 && vfs_lookup("/Fake/a.txt.tmp") < 0 && ff_count() == 2);
    CHECK(vfs_read(g1, 0, buf, sizeof buf) == 4 && memcmp(buf, "tiny", 4) == 0);
    int g2 = vfs_save(root, "a.txt", "a longer text than before", 25);       /* longer */
    CHECK(g2 == old && vfs_node(g2)->size == 25 && ff_count() == 2);
    CHECK(vfs_read(g2, 0, buf, sizeof buf) == 25 && memcmp(buf, "a longer text than before", 25) == 0);
    CHECK(vfs_truncate(g2, 25) == -1 && vfs_last_error() == VFS_ERR_ARG);     /* only shortens */
    /* ... and one that can neither delete nor shorten: a longer text still fits over the old one, a shorter one is
     * refused before anything is touched, and the new text stays safe in the .tmp file */
    FF_NOTRUNC = 1;
    CHECK(vfs_save(root, "a.txt", "a longer text than before, longer still", 39) == old);
    CHECK(vfs_save(root, "a.txt", "short", 5) == -1 && vfs_last_error() == VFS_ERR_UNSUPPORTED);
    CHECK(strcmp(vfs_save_step(), "shorten the old file") == 0);
    CHECK(vfs_node(old)->size == 39 && vfs_read(old, 0, buf, sizeof buf) == 39 &&
          memcmp(buf, "a longer text than before, longer still", 39) == 0);   /* the old file is intact */
    int tmp = vfs_lookup("/Fake/a.txt.tmp");
    CHECK(tmp > 0 && vfs_read(tmp, 0, buf, sizeof buf) == 5 && memcmp(buf, "short", 5) == 0);
    FF_NOTRUNC = 0;
    FF_NODELETE = 0;
    CHECK(vfs_save(root, "a.txt", "ok", 2) > 0 && vfs_lookup("/Fake/a.txt.tmp") < 0 && ff_count() == 2);

    /* refused: the RAM Disk has no file content, a file is not a folder, a bad name, a name that is a folder */
    CHECK(vfs_save(ram, "x.txt", "x", 1) == -1 && vfs_last_error() == VFS_ERR_UNSUPPORTED);
    CHECK(vfs_save(f, "x.txt", "x", 1) == -1 && vfs_last_error() == VFS_ERR_ARG);
    CHECK(vfs_save(root, "a/b.txt", "x", 1) == -1 && vfs_last_error() == VFS_ERR_BADNAME);
    CHECK(vfs_write(vfs_lookup("/RAM Disk/readme.txt"), 0, "x", 1) == -1 && vfs_last_error() == VFS_ERR_UNSUPPORTED);
    CHECK(vfs_write(root, 0, "x", 1) == -1 && vfs_last_error() == VFS_ERR_ARG);
}

int main(void)
{
    vfs_reset();
    CHECK(vfs_count() == 1);
    CHECK(vfs_node(VFS_ROOT)->is_dir);

    int a = vfs_add(VFS_ROOT, "zeta.txt", 0, 10);
    int b = vfs_add(VFS_ROOT, "Alpha.txt", 0, 20);
    int d = vfs_add(VFS_ROOT, "docs", 1, 999);
    CHECK(a > 0 && b > 0 && d > 0);
    CHECK(vfs_node(d)->size == 0); /* folders have no size */

    /* rejected adds */
    CHECK(vfs_add(VFS_ROOT, "ALPHA.TXT", 0, 1) == -1); /* duplicate, ignoring case */
    CHECK(vfs_add(VFS_ROOT, "", 0, 1) == -1);
    CHECK(vfs_add(VFS_ROOT, "a/b", 0, 1) == -1);
    CHECK(vfs_add(a, "child", 0, 1) == -1);            /* parent is a file */
    CHECK(vfs_add(50, "x", 0, 1) == -1);               /* parent does not exist */
    char longname[VFS_NAME_MAX + 10];
    memset(longname, 'n', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    CHECK(vfs_add(VFS_ROOT, longname, 0, 1) == -1);
    longname[VFS_NAME_MAX - 1] = 0;                    /* longest allowed: VFS_NAME_MAX - 1 chars */
    CHECK(vfs_add(VFS_ROOT, longname, 0, 1) > 0);

    /* folders first, then names ignoring case */
    int ids[16];
    int n = vfs_list(VFS_ROOT, ids, 16);
    CHECK(n == 4);
    CHECK(ids[0] == d);
    CHECK(ids[1] == b);                                /* Alpha.txt before nnnn... before zeta.txt */
    CHECK(ids[3] == a);

    /* a short out[] still reports the full count and keeps the first entries */
    int two[2];
    CHECK(vfs_list(VFS_ROOT, two, 2) == 4);
    CHECK(two[0] == d && two[1] == b);
    CHECK(vfs_list(a, ids, 16) == 0);                  /* a file has no children */

    /* paths and lookup */
    int sub = vfs_add(d, "sub", 1, 0);
    int f = vfs_add(sub, "f.c", 0, 5);
    char p[64];
    vfs_path(f, p, sizeof p);
    CHECK(strcmp(p, "/docs/sub/f.c") == 0);
    vfs_path(VFS_ROOT, p, sizeof p);
    CHECK(strcmp(p, "/") == 0);
    char small[6];
    CHECK(vfs_path(f, small, sizeof small) == 5);      /* cut, still 0-terminated */
    CHECK(strcmp(small, "/docs") == 0);
    CHECK(vfs_lookup("/docs/sub/f.c") == f);
    CHECK(vfs_lookup("/DOCS//sub/") == sub);
    CHECK(vfs_lookup("/") == VFS_ROOT);
    CHECK(vfs_lookup("/docs/nope") == -1);
    CHECK(vfs_lookup("/zeta.txt/x") == -1);
    CHECK(vfs_lookup("docs") == -1);

    CHECK(vfs_is_under(f, d) && vfs_is_under(f, VFS_ROOT) && vfs_is_under(d, d));
    CHECK(!vfs_is_under(d, f));

    /* pool exhaustion */
    vfs_reset();
    int added = 0;
    char nm[8];
    for (int i = 0; i < VFS_MAX_NODES + 5; i++) {
        nm[0] = 'f'; nm[1] = (char)('a' + i / 26); nm[2] = (char)('a' + i % 26); nm[3] = 0;
        if (vfs_add(VFS_ROOT, nm, 0, 1) > 0) added++;
    }
    CHECK(added == VFS_MAX_NODES - 1);

    /* rename */
    vfs_reset();
    int r1 = vfs_add(VFS_ROOT, "one.txt", 0, 1);
    int r2 = vfs_add(VFS_ROOT, "two.txt", 0, 2);
    CHECK(vfs_rename(r1, "uno.txt") == 0 && strcmp(vfs_node(r1)->name, "uno.txt") == 0);
    CHECK(vfs_rename(r1, "TWO.txt") == -1);            /* sibling has it */
    CHECK(vfs_rename(r1, "UNO.TXT") == 0);             /* only the capitals change */
    CHECK(vfs_rename(r1, "") == -1 && vfs_rename(r1, "a/b") == -1);
    CHECK(vfs_rename(VFS_ROOT, "x") == -1 && vfs_rename(77, "x") == -1);
    CHECK(strcmp(vfs_node(r1)->name, "UNO.TXT") == 0); /* failed renames change nothing */
    CHECK(vfs_lookup("/uno.txt") == r1);

    /* remove: a folder takes everything below it along */
    int rd = vfs_add(VFS_ROOT, "dir", 1, 0);
    int rs = vfs_add(rd, "sub", 1, 0);
    vfs_add(rs, "deep.txt", 0, 3);
    vfs_add(rd, "x.txt", 0, 4);
    CHECK(vfs_subtree_count(rd) == 3 && vfs_subtree_count(r2) == 0);
    int before_n = vfs_count();
    CHECK(vfs_remove(rd) == 4);
    CHECK(vfs_count() == before_n - 4);
    CHECK(vfs_lookup("/dir") == -1 && vfs_lookup("/dir/sub/deep.txt") == -1);
    CHECK(vfs_remove(rd) == -1 && vfs_remove(VFS_ROOT) == -1);
    CHECK(vfs_add(VFS_ROOT, "dir", 1, 0) > 0);         /* the name and slots are free again */
    CHECK(vfs_list(vfs_lookup("/dir"), ids, 16) == 0); /* ... and nothing of the old content is back */

    /* copy / move */
    vfs_reset();
    int cd = vfs_add(VFS_ROOT, "src", 1, 0);
    int cf = vfs_add(cd, "a.txt", 0, 7);
    int cs = vfs_add(cd, "inner", 1, 0);
    vfs_add(cs, "b.txt", 0, 9);
    int cdst = vfs_add(VFS_ROOT, "dst", 1, 0);
    int c1 = vfs_copy(cf, cd);                         /* same folder: gets a new name */
    CHECK(c1 > 0 && strcmp(vfs_node(c1)->name, "a (2).txt") == 0 && vfs_node(c1)->size == 7);
    int c2 = vfs_copy(cf, cd);
    CHECK(c2 > 0 && strcmp(vfs_node(c2)->name, "a (3).txt") == 0);
    int c3 = vfs_copy(cd, cdst);                       /* a folder comes with everything in it */
    CHECK(c3 > 0 && vfs_lookup("/dst/src/inner/b.txt") > 0 && vfs_lookup("/dst/src/a (3).txt") > 0);
    CHECK(vfs_lookup("/src/inner/b.txt") > 0);         /* the original is untouched */
    CHECK(vfs_copy(cd, cd) == -1 && vfs_copy(cd, cs) == -1);   /* not into itself */
    CHECK(vfs_copy(VFS_ROOT, cdst) == -1 && vfs_copy(cf, cf) == -1 && vfs_copy(cf, 80) == -1);
    int c4 = vfs_copy(cd, cdst);                       /* the folder name is taken: "src (2)" */
    CHECK(c4 > 0 && strcmp(vfs_node(c4)->name, "src (2)") == 0);

    CHECK(vfs_move(cf, cdst) == cf);                   /* same id, new place */
    CHECK(vfs_node(cf)->parent == cdst && vfs_lookup("/dst/a.txt") == cf && vfs_lookup("/src/a.txt") == -1);
    int mf = vfs_add(cd, "a.txt", 0, 1);
    CHECK(vfs_move(mf, cdst) == mf && strcmp(vfs_node(mf)->name, "a (2).txt") == 0); /* clash: renamed */
    CHECK(vfs_move(mf, cdst) == mf);                   /* already there: nothing changes */
    CHECK(strcmp(vfs_node(mf)->name, "a (2).txt") == 0);
    CHECK(vfs_move(cd, cs) == -1 && vfs_move(cd, cd) == -1);   /* not into itself */
    CHECK(vfs_move(VFS_ROOT, cdst) == -1 && vfs_move(cf, mf) == -1);

    /* a copy that does not fit leaves nothing behind */
    vfs_reset();
    int bd = vfs_add(VFS_ROOT, "big", 1, 0);
    for (int i = 0; vfs_count() < VFS_MAX_NODES - 2; i++) {
        nm[0] = 'f'; nm[1] = (char)('a' + i / 26); nm[2] = (char)('a' + i % 26); nm[3] = 0;
        vfs_add(bd, nm, 0, 1);
    }
    int room = VFS_MAX_NODES - vfs_count();
    int dd = vfs_add(VFS_ROOT, "dd", 1, 0);            /* uses one of the 2 free slots */
    CHECK(room == 2 && dd > 0 && vfs_count() == VFS_MAX_NODES - 1);
    CHECK(vfs_copy(bd, dd) == -1 && vfs_count() == VFS_MAX_NODES - 1);

    /* sort orders */
    vfs_reset();
    int s_b = vfs_add(VFS_ROOT, "b.txt", 0, 5);
    int s_a = vfs_add(VFS_ROOT, "a.c", 0, 100);
    int s_c = vfs_add(VFS_ROOT, "c.png", 0, 50);
    int s_n = vfs_add(VFS_ROOT, "noext", 0, 5);
    int s_z = vfs_add(VFS_ROOT, "z", 1, 0);
    int s_y = vfs_add(VFS_ROOT, "y", 1, 0);
    int so[16];
    CHECK(vfs_list_by(VFS_ROOT, so, 16, VFS_SORT_NAME, 1) == 6);
    CHECK(so[0] == s_z && so[1] == s_y && so[2] == s_n && so[3] == s_c && so[4] == s_b && so[5] == s_a);
    vfs_list_by(VFS_ROOT, so, 16, VFS_SORT_SIZE, 0);   /* folders first (by name), then small to big; ties by name */
    CHECK(so[0] == s_y && so[1] == s_z && so[2] == s_b && so[3] == s_n && so[4] == s_c && so[5] == s_a);
    vfs_list_by(VFS_ROOT, so, 16, VFS_SORT_SIZE, 1);   /* big to small; folders stay by name ascending */
    CHECK(so[0] == s_y && so[1] == s_z && so[2] == s_a && so[3] == s_c && so[4] == s_b && so[5] == s_n);
    vfs_list_by(VFS_ROOT, so, 16, VFS_SORT_TYPE, 0);   /* "" < c < png < txt: here "", "c", "png", "txt" */
    CHECK(so[0] == s_y && so[1] == s_z && so[2] == s_n && so[3] == s_a && so[4] == s_c && so[5] == s_b);
    vfs_list_by(VFS_ROOT, so, 16, VFS_SORT_TYPE, 1);
    CHECK(so[2] == s_b && so[3] == s_c && so[4] == s_a && so[5] == s_n);
    vfs_list(VFS_ROOT, so, 16);                        /* the plain list is name ascending */
    CHECK(so[0] == s_y && so[1] == s_z && so[2] == s_a && so[5] == s_n);
    CHECK(vfs_list_by(VFS_ROOT, so, 2, VFS_SORT_SIZE, 1) == 6 && so[0] == s_y && so[1] == s_z);

    /* demo content */
    vfs_init();
    CHECK(vfs_lookup("/RAM Disk/System/EFI/BOOT/BOOTX64.EFI") > 0);
    CHECK(vfs_lookup("/RAM Disk/Documents/Projects/haris.c") > 0);
    CHECK(vfs_volume_count() == 1 && vfs_volume_node(0) == vfs_lookup("/RAM Disk"));
    VfsVolInfo vi;
    CHECK(vfs_volume_info(0, &vi) == 0 && strcmp(vi.fs_name, "RAM") == 0 && vi.node == vfs_volume_node(0));
    CHECK(vfs_rename(vfs_volume_node(0), "x") == -1 && vfs_remove(vfs_volume_node(0)) == -1);  /* a drive stays */
    CHECK(vfs_volume_of(vfs_lookup("/RAM Disk/Music/intro.wav")) == 0 && vfs_volume_of(VFS_ROOT) == -1);
    CHECK(vfs_count() < VFS_MAX_NODES);

    test_save();

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("vfs tests passed\n");
    return 0;
}
