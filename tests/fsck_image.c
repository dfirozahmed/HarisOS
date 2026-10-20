/* Host tool: checks the FAT32 volumes in a disk image (for example one a QEMU run wrote to).
 *   usage: fsck_image <image>     - prints each partition's problems and its root folder; exit status 1 if any
 * It uses the kernel's own partition code and the test checker from fatimg.h, which does not use fat32.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "fat32.h"
#include "fatimg.h"

static Fat *SHOWFAT;

/* FNV-1a of a file's content, to compare with the host file */
static unsigned hash_file(const FatEntry *e)
{
    unsigned h = 2166136261u;
    unsigned char buf[4096];
    for (unsigned off = 0; off < e->size;) {
        int r = fat_read(SHOWFAT, e, off, buf, sizeof buf);
        if (r <= 0) return 0;
        for (int i = 0; i < r; i++) h = (h ^ buf[i]) * 16777619u;
        off += (unsigned)r;
    }
    return h;
}

static int show(const char *name, const FatEntry *e, void *user)
{
    (void)user;
    if (e->attr & FAT_ATTR_DIR) printf("    %s/\n", name);
    else printf("    %s  %u bytes  fnv1a %08x\n", name, e->size, hash_file(e));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: fsck_image <image>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    DISK = malloc((size_t)len);
    if (fread(DISK, 1, (size_t)len, f) != (size_t)len) { printf("read error\n"); return 2; }
    fclose(f);
    NSEC = (unsigned long long)len / SS;
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, "img0"); d.kind = BLK_RAM; d.parent = -1; d.sector_size = SS; d.sectors = NSEC;
    d.writable = 0; d.read = rd; d.ctx = DISK;
    int di = blk_register(&d);
    int parts = part_scan(di);
    int bad = 0;
    for (int i = 1; i <= parts; i++) {
        BlkDev *p = blk_get(i);
        int e = fsck_volume_at(p->first_lba);
        printf("%s (%s, %llu sectors): %s\n", p->name, part_kind_name(p->part_kind), p->sectors, e ? "PROBLEMS" : "ok");
        bad += e;
        Fat fat;
        if (fat_mount(&fat, i) == 0) {
            unsigned long long total, freeb;
            fat_space(&fat, &total, &freeb);
            printf("  FAT32 '%s': %llu KiB, %llu KiB free; root folder:\n", fat.label, total / 1024, freeb / 1024);
            SHOWFAT = &fat;
            fat_list(&fat, fat_root(&fat), show, 0);
        }
    }
    if (!parts) printf("no partitions found\n");
    return bad ? 1 : 0;
}
