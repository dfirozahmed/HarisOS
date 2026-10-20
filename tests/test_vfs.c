/* Host test for vfs.c: add rules, sorting, paths, lookup. */
#include <stdio.h>
#include <string.h>
#include "vfs.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

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
    char longname[64];
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

    CHECK(vfs_move(cf, cdst) == 0);                    /* same id, new place */
    CHECK(vfs_node(cf)->parent == cdst && vfs_lookup("/dst/a.txt") == cf && vfs_lookup("/src/a.txt") == -1);
    int mf = vfs_add(cd, "a.txt", 0, 1);
    CHECK(vfs_move(mf, cdst) == 0 && strcmp(vfs_node(mf)->name, "a (2).txt") == 0); /* clash: renamed */
    CHECK(vfs_move(mf, cdst) == 0);                    /* already there: nothing changes */
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
    CHECK(vfs_lookup("/System/EFI/BOOT/BOOTX64.EFI") > 0);
    CHECK(vfs_lookup("/Documents/Projects/haris.c") > 0);
    CHECK(vfs_count() < VFS_MAX_NODES);

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("vfs tests passed\n");
    return 0;
}
