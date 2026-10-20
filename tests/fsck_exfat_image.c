/* Host tool: checks the exFAT partitions of a disk image (for example one a QEMU run wrote to) with the independent
 * checker from exfatcheck.h.   usage: fsck_exfat_image <image> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"
#include "exfatimg.h"
#include "exfatcheck.h"

static int rd_(BlkDev *d, unsigned long long lba, unsigned n, void *buf)
{
    memcpy(buf, (unsigned char *)d->ctx + lba * 512, (size_t)n * 512);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: fsck_exfat_image <image>"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { puts("cannot open the image"); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *img = malloc((size_t)len);
    if (fread(img, 1, (size_t)len, f) != (size_t)len) return 2;
    fclose(f);
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, "img0"); d.kind = BLK_RAM; d.parent = -1; d.sector_size = 512; d.sectors = (unsigned long long)len / 512; d.read = rd_; d.ctx = img;
    int di = blk_register(&d);
    int parts = part_scan(di);
    int bad = 0, found = 0;
    for (int i = 1; i <= parts; i++) {
        BlkDev *p = blk_get(i);
        unsigned char *v = img + p->first_lba * 512;
        if (memcmp(v + 3, "EXFAT   ", 8) != 0) continue;
        XImg x;
        memset(&x, 0, sizeof x);
        x.img = v;
        x.ss = 512;
        x.spc_shift = v[109];
        x.fat_off = xc_rd32(v + 80); x.fat_len = xc_rd32(v + 84); x.heap_off = xc_rd32(v + 88); x.clusters = xc_rd32(v + 92);
        int e = xfsck(&x);
        printf("%s: exFAT, %s\n", p->name, e ? "PROBLEMS" : "consistent");
        bad += e;
        found++;
    }
    if (!found) puts("no exFAT partition");
    return bad ? 1 : 0;
}
