/* Host test for volume.c + vfs.c: FAT32 images mounted as drives under "This PC", and every file operation
 * of the File Explorer done through the VFS (lazy loading, refresh, create, copy, move, rename, delete, the
 * write lock, errors), each time checked against the independent FAT consistency checker. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "fat32.h"
#include "fatimg.h"
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

int main(void)
{
    blk_reset();
    make_image(0, "sata0");
    make_image(1, "sata1");
    vfs_init();
    CHECK(volume_mount_all() == 2);
    CHECK(vfs_volume_count() == 3);                      /* the RAM Disk and two drives */
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
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("volume tests passed\n");
    return 0;
}
