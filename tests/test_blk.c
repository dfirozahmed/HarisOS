/* Host test for blk.c / part.c: sector I/O limits, the write lock, GPT and MBR parsing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define SS 512
static unsigned char *DISK;
static unsigned long long NSEC;
static int writes_done;

static int rd(BlkDev *d, unsigned long long lba, unsigned n, void *buf)
{
    (void)d;
    memcpy(buf, DISK + lba * SS, (size_t)n * SS);
    return 0;
}

static int wr(BlkDev *d, unsigned long long lba, unsigned n, const void *buf)
{
    (void)d;
    memcpy(DISK + lba * SS, buf, (size_t)n * SS);
    writes_done++;
    return 0;
}

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

static void put32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(unsigned char *p, unsigned long long v) { put32(p, (unsigned)v); put32(p + 4, (unsigned)(v >> 32)); }

static int add_disk(const char *name)
{
    BlkDev d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, name);
    strcpy(d.model, "TEST DISK");
    d.kind = BLK_RAM;
    d.parent = -1;
    d.sector_size = SS;
    d.sectors = NSEC;
    d.writable = 1;
    d.read = rd;
    d.write = wr;
    return blk_register(&d);
}

static void fresh_disk(unsigned long long sectors)
{
    free(DISK);
    NSEC = sectors;
    DISK = calloc((size_t)sectors, SS);
    blk_reset();
}

/* protective MBR + GPT with the given entries */
static void make_gpt(int nparts, const unsigned char types[][16], const unsigned long long *first,
                     const unsigned long long *last, const char *const *names)
{
    unsigned char *mbr = DISK;
    mbr[446 + 4] = 0xEE;
    put32(mbr + 446 + 8, 1);
    put32(mbr + 446 + 12, (unsigned)(NSEC - 1));
    mbr[510] = 0x55; mbr[511] = 0xAA;
    unsigned char *ents = DISK + 2 * SS; /* 128 entries x 128 bytes = 32 sectors */
    for (int i = 0; i < nparts; i++) {
        unsigned char *e = ents + 128 * i;
        memcpy(e, types[i], 16);
        e[16] = 0x11 + i; /* unique guid: anything non-zero */
        put64(e + 32, first[i]);
        put64(e + 40, last[i]);
        for (int k = 0; names[i][k]; k++) e[56 + 2 * k] = names[i][k];
    }
    unsigned char *h = DISK + SS;
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);
    put32(h + 12, 92);
    put64(h + 24, 1);
    put64(h + 32, NSEC - 1);
    put64(h + 40, 34);
    put64(h + 48, NSEC - 34);
    put64(h + 72, 2);
    put32(h + 80, 128);
    put32(h + 84, 128);
    put32(h + 88, crc32(0, ents, 128 * 128));
    put32(h + 16, crc32(0, h, 92));
}

static const unsigned char T_ESP[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
static const unsigned char T_BASIC[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};

int main(void)
{
    CHECK(crc32(0, (const unsigned char *)"123456789", 9) == 0xCBF43926u); /* the test's own CRC is right */

    /* ---- registry, ranges, lock ---- */
    fresh_disk(1000);
    int d0 = add_disk("test0");
    CHECK(d0 == 0 && blk_count() == 1 && blk_get(0)->locked == 1);
    unsigned char sec[SS], out[SS * 2];
    memset(sec, 0xAB, SS);
    CHECK(blk_write(d0, 5, 1, sec) == -1 && writes_done == 0);          /* locked by default */
    blk_set_locked(d0, 0);
    CHECK(blk_write(d0, 5, 1, sec) == 0 && writes_done == 1);
    CHECK(blk_read(d0, 5, 1, out) == 0 && out[0] == 0xAB);
    CHECK(blk_read(d0, 999, 1, out) == 0);                              /* last sector */
    CHECK(blk_read(d0, 999, 2, out) == -1 && blk_read(d0, 1000, 1, out) == -1);
    CHECK(blk_read(d0, 0, 0, out) == -1 && blk_read(5, 0, 1, out) == -1 && blk_read(d0, 0, 1, 0) == -1);
    CHECK(blk_write(d0, 999, 2, out) == -1);
    BlkStats st;                                                        /* only successful I/O is counted */
    blk_get_stats(&st);
    CHECK(st.read_ops == 2 && st.read_bytes == 2 * SS && st.write_ops == 1 && st.write_bytes == SS);
    blk_set_locked(d0, 1);
    CHECK(blk_write(d0, 5, 1, sec) == -1);
    BlkDev *ro = blk_get(d0); ro->writable = 0; blk_set_locked(d0, 0);
    CHECK(blk_write(d0, 5, 1, sec) == -1);                              /* a read-only driver stays read-only */
    CHECK(blk_flush(d0) == 0);

    /* ---- GPT ---- */
    fresh_disk(200000);
    d0 = add_disk("nvme0");
    unsigned char types[2][16];
    memcpy(types[0], T_ESP, 16); memcpy(types[1], T_BASIC, 16);
    unsigned long long first[2] = {2048, 100000}, last[2] = {99999, 190000};
    const char *names[2] = {"EFI System", "Data"};
    make_gpt(2, types, first, last, names);
    CHECK(part_scan(d0) == 2 && blk_count() == 3);
    BlkDev *p1 = blk_get(1), *p2 = blk_get(2);
    CHECK(strcmp(p1->name, "nvme0p1") == 0 && p1->kind == BLK_PART && p1->parent == 0);
    CHECK(p1->part_kind == PART_ESP && p1->first_lba == 2048 && p1->sectors == 97952 && strcmp(p1->part_label, "EFI System") == 0);
    CHECK(p2->part_kind == PART_FAT && p2->sectors == 90001 && p2->part_no == 2);
    /* a partition's sector 0 is its first sector; its end is enforced */
    memset(DISK + 2048 * SS, 0x5A, SS);
    CHECK(blk_read(1, 0, 1, out) == 0 && out[0] == 0x5A);
    CHECK(blk_read(1, 97951, 1, out) == 0 && blk_read(1, 97952, 1, out) == -1 && blk_read(1, 97951, 2, out) == -1);
    CHECK(blk_write(1, 0, 1, sec) == -1);                               /* locked, like every device */
    blk_set_locked(1, 0);                                               /* a partition's lock is its own */
    CHECK(blk_get(1)->locked == 0 && blk_get(0)->locked == 1 && blk_get(2)->locked == 1);
    CHECK(blk_write(1, 3, 1, sec) == 0 && DISK[(2048 + 3) * SS] == 0xAB);
    CHECK(blk_write(2, 90000, 1, sec) == -1 && blk_write(0, 5, 1, sec) == -1); /* the others stay locked */
    blk_set_locked(2, 0);
    CHECK(blk_write(2, 90000, 1, sec) == 0 && DISK[(100000 + 90000) * SS] == 0xAB);
    blk_set_locked(0, 1);                                               /* the whole disk: its partitions follow */
    CHECK(blk_get(1)->locked == 1 && blk_get(2)->locked == 1 && blk_write(1, 3, 1, sec) == -1);
    blk_set_locked(0, 0);
    CHECK(blk_get(1)->locked == 0 && blk_get(2)->locked == 0);
    CHECK(part_scan(1) == 0);                                           /* partitions have no table of their own */

    /* a damaged table is refused: wrong entry CRC, wrong header CRC, bad signature */
    blk_reset(); d0 = add_disk("nvme0");
    DISK[2 * SS + 3] ^= 0xFF;
    CHECK(part_scan(d0) == 0 && blk_count() == 1);
    DISK[2 * SS + 3] ^= 0xFF;
    DISK[SS + 40] ^= 1;
    CHECK(part_scan(d0) == 0);
    DISK[SS + 40] ^= 1;
    DISK[SS] = 'X';
    CHECK(part_scan(d0) == 0);
    DISK[SS] = 'E';
    CHECK(part_scan(d0) == 2);                                          /* ... and the repaired one works */

    /* a partition that sticks out of the usable area is skipped */
    fresh_disk(200000); d0 = add_disk("sata0");
    unsigned long long first3[2] = {2048, 100000}, last3[2] = {99999, 199999};
    make_gpt(2, types, first3, last3, names);
    CHECK(part_scan(d0) == 1);

    /* ---- MBR ---- */
    fresh_disk(50000); d0 = add_disk("sata0");
    unsigned char *e = DISK + 446;
    e[4] = 0x0C; put32(e + 8, 2048); put32(e + 12, 20000);
    e = DISK + 446 + 16; e[4] = 0x07; put32(e + 8, 30000); put32(e + 12, 10000);
    e = DISK + 446 + 32; e[4] = 0x05; put32(e + 8, 41000); put32(e + 12, 5000);      /* extended: skipped */
    e = DISK + 446 + 48; e[4] = 0x83; put32(e + 8, 49000); put32(e + 12, 5000);      /* ends past the disk */
    DISK[510] = 0x55; DISK[511] = 0xAA;
    CHECK(part_scan(d0) == 2);
    CHECK(blk_get(1)->part_kind == PART_FAT && blk_get(1)->first_lba == 2048 && strcmp(blk_get(1)->name, "sata0p1") == 0);
    CHECK(blk_get(2)->part_kind == PART_NTFS_EXFAT);

    /* a FAT volume from sector 0 (no partition table) is not mistaken for an MBR */
    fresh_disk(50000); d0 = add_disk("sata0");
    DISK[0] = 0xEB; DISK[1] = 0x58; DISK[2] = 0x90;
    DISK[11] = 0x00; DISK[12] = 0x02; DISK[13] = 8; DISK[14] = 32; DISK[16] = 2;
    DISK[510] = 0x55; DISK[511] = 0xAA;
    DISK[446 + 4] = 0x0B; put32(DISK + 446 + 8, 100); put32(DISK + 446 + 12, 100); /* boot code garbage that looks like an entry */
    CHECK(part_scan(d0) == 0);
    /* the same for NTFS and exFAT, which are recognised by the name at byte 3 */
    fresh_disk(50000); d0 = add_disk("sata0");
    memcpy(DISK + 3, "NTFS    ", 8);
    DISK[510] = 0x55; DISK[511] = 0xAA;
    DISK[446 + 4] = 0x07; put32(DISK + 446 + 8, 100); put32(DISK + 446 + 12, 100);
    CHECK(part_scan(d0) == 0);
    memcpy(DISK + 3, "EXFAT   ", 8);
    CHECK(part_scan(d0) == 0);
    fresh_disk(50000); d0 = add_disk("sata0");                          /* an empty disk */
    CHECK(part_scan(d0) == 0);

    /* ---- the real thing: the image the build makes (GPT with one EFI System Partition) ---- */
    FILE *f = fopen("build/haris.img", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        fresh_disk((unsigned long long)len / SS);
        if (fread(DISK, 1, (size_t)len, f) != (size_t)len) { printf("read error\n"); return 1; }
        d0 = add_disk("sata0");
        int n = part_scan(d0);
        CHECK(n >= 1 && blk_get(1)->part_kind == PART_ESP);
        printf("haris.img: %d partition(s), first %s %llu sectors\n", n, part_kind_name(blk_get(1)->part_kind), blk_get(1)->sectors);
        fclose(f);
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("blk tests passed\n");
    return 0;
}
