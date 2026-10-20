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

    /* demo content */
    vfs_init();
    CHECK(vfs_lookup("/System/EFI/BOOT/BOOTX64.EFI") > 0);
    CHECK(vfs_lookup("/Documents/Projects/haris.c") > 0);
    CHECK(vfs_count() < VFS_MAX_NODES);

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("vfs tests passed\n");
    return 0;
}
