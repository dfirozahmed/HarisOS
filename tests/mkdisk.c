/* Host tool: puts volume images into one GPT disk image, to try the drivers on a disk with several partitions.
 *   usage: mkdisk out.img esp:fat.img data:ntfs.img data:exfat.img ...
 * "esp" gets the EFI System Partition type, "data" the Microsoft basic data type (FAT, exFAT, NTFS). Partitions start
 * on 1 MiB boundaries. The primary and the backup GPT are written with their CRCs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SS 512

static unsigned crc32(unsigned crc, const unsigned char *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (unsigned)-(int)(crc & 1));
    }
    return ~crc;
}

static void p32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void p64(unsigned char *p, unsigned long long v) { p32(p, (unsigned)v); p32(p + 4, (unsigned)(v >> 32)); }

static const unsigned char T_ESP[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
static const unsigned char T_DATA[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: mkdisk out.img esp:file data:file ...\n"); return 2; }
    int np = argc - 2;
    if (np > 100) return 2;
    unsigned char *vol[100];
    unsigned long long len[100], first[100], count[100], pos = 2048;
    const unsigned char *type[100];
    for (int i = 0; i < np; i++) {
        const char *a = argv[2 + i];
        type[i] = strncmp(a, "esp:", 4) == 0 ? T_ESP : T_DATA;
        const char *path = strchr(a, ':') ? strchr(a, ':') + 1 : a;
        FILE *f = fopen(path, "rb");
        if (!f) { printf("cannot open %s\n", path); return 2; }
        fseek(f, 0, SEEK_END);
        len[i] = (unsigned long long)ftell(f);
        fseek(f, 0, SEEK_SET);
        vol[i] = malloc((size_t)len[i]);
        if (fread(vol[i], 1, (size_t)len[i], f) != (size_t)len[i]) return 2;
        fclose(f);
        first[i] = pos;
        count[i] = len[i] / SS;
        pos += (count[i] + 2047) / 2048 * 2048;
    }
    unsigned long long total = pos + 2048;
    unsigned char *d = calloc((size_t)total, SS);
    for (int i = 0; i < np; i++) memcpy(d + first[i] * SS, vol[i], (size_t)len[i]);

    /* protective MBR */
    d[446 + 4] = 0xEE; p32(d + 446 + 8, 1); p32(d + 446 + 12, (unsigned)(total - 1));
    d[510] = 0x55; d[511] = 0xAA;

    unsigned char ents[128 * 128];
    memset(ents, 0, sizeof ents);
    for (int i = 0; i < np; i++) {
        unsigned char *e = ents + 128 * i;
        memcpy(e, type[i], 16);
        e[16] = (unsigned char)(0x40 + i); e[17] = 0x77; e[31] = 1; /* a unique id */
        p64(e + 32, first[i]);
        p64(e + 40, first[i] + count[i] - 1);
        char nm[32];
        snprintf(nm, sizeof nm, "Volume %d", i + 1);
        for (int k = 0; nm[k]; k++) e[56 + 2 * k] = (unsigned char)nm[k];
    }
    /* primary: header at LBA 1, entries at LBA 2; backup: entries at total-33, header at total-1 */
    for (int copy = 0; copy < 2; copy++) {
        unsigned long long hdr_lba = copy ? total - 1 : 1, ent_lba = copy ? total - 33 : 2;
        unsigned char *h = d + hdr_lba * SS;
        memcpy(h, "EFI PART", 8);
        p32(h + 8, 0x00010000); p32(h + 12, 92);
        p64(h + 24, hdr_lba); p64(h + 32, copy ? 1 : total - 1);
        p64(h + 40, 34); p64(h + 48, total - 34);
        h[56] = 0xAB; h[57] = (unsigned char)copy; h[63] = 0x11;      /* disk guid */
        p64(h + 72, ent_lba); p32(h + 80, 128); p32(h + 84, 128);
        p32(h + 88, crc32(0, ents, sizeof ents));
        p32(h + 16, crc32(0, h, 92));
        memcpy(d + ent_lba * SS, ents, sizeof ents);
    }
    FILE *o = fopen(argv[1], "wb");
    if (!o) { printf("cannot write %s\n", argv[1]); return 2; }
    fwrite(d, SS, (size_t)total, o);
    fclose(o);
    printf("wrote %s: %llu sectors, %d partition(s)\n", argv[1], total, np);
    return 0;
}
