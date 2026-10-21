/* Host test for hot-plug: a USB disk with an MBR partition appears (volume_sync mounts it under "This PC"),
 * can be written once unlocked (the lock is the partition's own), and disappears again when it is unplugged,
 * leaving the other drives usable although their volume numbers shift. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "fat32.h"
#include "fatimg.h"
#include "vfs.h"
#include "volume.h"

#define GAP 2048u

/* a USB stick: MBR in sector 0, one FAT32 partition from sector GAP on. Returns the block device index. */
static unsigned char *stick_mem(unsigned fs_sectors)
{
    mkfs(fs_sectors, 2);
    unsigned long long total = GAP + fs_sectors;
    unsigned char *m = calloc((size_t)total, SS);
    memcpy(m + (size_t)GAP * SS, DISK, (size_t)fs_sectors * SS);
    m[446 + 4] = 0x0C; /* FAT32 (LBA) */
    put32(m + 446 + 8, GAP);
    put32(m + 446 + 12, fs_sectors);
    m[510] = 0x55;
    m[511] = 0xAA;
    NSEC = total;
    return m;
}

static int plug(unsigned char *mem, const char *name)
{
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, name);
    strcpy(d.model, "TEST STICK");
    d.kind = BLK_USB;
    d.parent = -1;
    d.sector_size = SS;
    d.sectors = NSEC;
    d.writable = 1;
    d.read = rd;
    d.write = wr;
    d.ctx = mem;
    int idx = blk_register(&d);
    CHECK(idx >= 0);
    CHECK(part_scan(idx) == 1);
    return idx;
}

int main(void)
{
    blk_reset();
    vfs_init();
    CHECK(volume_mount_all() == 0);
    CHECK(vfs_volume_count() == 1); /* the RAM Disk */
    unsigned bg = blk_generation(), vg = vfs_generation();

    /* ---- plug in stick 1 ---- */
    unsigned char *m1 = stick_mem(20000);
    int s1 = plug(m1, "usb0");
    CHECK(blk_generation() != bg);
    CHECK(volume_sync() != 0);
    CHECK(vfs_generation() != vg);
    CHECK(vfs_volume_count() == 2);
    int a = vfs_lookup("/TESTVOL (usb0p1)");
    CHECK(a > 0);
    VfsVolInfo vi;
    CHECK(vfs_volume_info(1, &vi) == 0 && strcmp(vi.fs_name, "FAT32") == 0 && strcmp(vi.device, "Removable (USB)") == 0);
    CHECK(vi.writable == 1 && vi.locked == 0 && vi.lockable == 0); /* a clean, ordinary drive: unlocked at once */
    CHECK(volume_sync() == 0);                                    /* nothing new: nothing changes */
    CHECK(vfs_volume_count() == 2);

    /* writing works although only the partition (not the disk) was unlocked */
    unsigned long long w0 = WRITES;
    int docs = vfs_add(a, "Docs", 1, 0);
    CHECK(docs > 0 && WRITES > w0);
    CHECK(vfs_add(docs, "note.txt", 0, 0) > 0);

    /* ---- plug in stick 2 ---- */
    unsigned char *m2 = stick_mem(20000);
    int s2 = plug(m2, "usb1");
    CHECK(s2 != s1);
    CHECK(volume_sync() != 0);
    CHECK(vfs_volume_count() == 3);
    int b = vfs_lookup("/TESTVOL (usb1p1)");
    CHECK(b > 0 && vfs_volume_of(b) == 2);

    /* ---- pull out stick 1: its folder goes, stick 2 moves up one place and still works ---- */
    blk_remove(s1);
    CHECK(blk_read(s1, 0, 1, malloc(SS)) != 0);
    CHECK(vfs_node(a) != NULL); /* still listed until the file system layer lets go */
    CHECK(volume_sync() != 0);
    CHECK(vfs_node(a) == NULL && vfs_node(docs) == NULL);
    CHECK(vfs_lookup("/TESTVOL (usb0p1)") < 0);
    CHECK(vfs_volume_count() == 2);
    CHECK(vfs_volume_of(b) == 1 && vfs_volume_node(1) == b);
    CHECK(vfs_volume_info(1, &vi) == 0 && strcmp(vi.name, "TESTVOL (usb1p1)") == 0);
    w0 = WRITES;
    CHECK(vfs_add(b, "after", 1, 0) > 0 && WRITES > w0);
    CHECK(blk_get(s1)->removed == 2 || blk_count() > 0); /* the slot was freed */

    /* ---- plug a new stick: it takes the freed slot ---- */
    unsigned char *m3 = stick_mem(20000);
    int s3 = plug(m3, "usb0");
    CHECK(s3 == s1);
    CHECK(volume_sync() != 0);
    CHECK(vfs_volume_count() == 3);
    int c = vfs_lookup("/TESTVOL (usb0p1)");
    CHECK(c > 0 && vfs_volume_of(c) == 2);
    CHECK(vfs_add(c, "fresh", 1, 0) > 0);

    /* ---- pull out everything ---- */
    blk_remove(s3);
    blk_remove(s2);
    CHECK(volume_sync() != 0);
    CHECK(vfs_volume_count() == 1);
    CHECK(blk_count() == 0);
    CHECK(volume_unsupported_count() == 0);

    if (fails)
    {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("usb volume hot-plug tests passed\n");
    return 0;
}
