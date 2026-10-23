/*
 * mkimg - builds a bootable UEFI disk image (haris.img) with no external tools.
 *
 *   mkimg <out.img> <BOOTX64.EFI> <KERNEL.EFI> [file ...]
 *
 * Layout (512-byte sectors, 64 MiB):
 *   LBA 0            protective MBR
 *   LBA 1            GPT header            LBA 2..33  partition entries
 *   LBA 2048..       one EFI System Partition (FAT32)
 *   last 33 sectors  backup GPT
 *
 * FAT32 contents:  /EFI/BOOT/BOOTX64.EFI   /KERNEL.EFI   and every extra file in the root folder (user programs such as
 *                  HELLO.ELF; the name is the file's own, upper case, at most 8 + 3 characters, at most 14 files)
 *
 * Write it to a USB stick (this ERASES the stick) with Rufus in "DD Image" mode,
 * balenaEtcher, or `dd`. The PC must boot in UEFI mode (Secure Boot off).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512u
#define TOTAL_SECTORS 131072u /* 64 MiB */
#define PART_FIRST 2048u
#define PART_SECTORS (TOTAL_SECTORS - PART_FIRST - 2048u)
#define PART_LAST (PART_FIRST + PART_SECTORS - 1u)
#define RESERVED 32u
#define FAT_COUNT 2u
#define FAT_EOC 0x0FFFFFFFu

static uint8_t *img;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t crc32(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
    {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "mkimg: cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n ? (size_t)n : 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n)
    {
        fprintf(stderr, "mkimg: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

/* ---- FAT32 --------------------------------------------------------------- */
static uint8_t *part; /* start of the partition inside img */
static uint32_t fat_sectors;
static uint32_t data_start; /* first data sector, relative to the partition */
static uint32_t next_cluster = 2;

static uint8_t *fat_entry(uint32_t cluster) { return part + RESERVED * SECTOR + cluster * 4; }
static uint8_t *cluster_data(uint32_t c) { return part + (size_t)(data_start + (c - 2)) * SECTOR; }

/* Allocates a contiguous chain of 1-sector clusters; returns the first cluster. */
static uint32_t alloc_chain(uint32_t count)
{
    uint32_t first = next_cluster;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t c = next_cluster++;
        put32(fat_entry(c), i + 1 == count ? FAT_EOC : c + 1);
    }
    return first;
}

static void dir_entry(uint8_t *e, const char *name83, uint8_t attr, uint32_t cluster, uint32_t size)
{
    uint16_t date = (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1);
    memcpy(e, name83, 11);
    e[11] = attr;
    put16(e + 16, date); /* created */
    put16(e + 18, date); /* accessed */
    put16(e + 20, (uint16_t)(cluster >> 16));
    put16(e + 24, date); /* written */
    put16(e + 26, (uint16_t)cluster);
    put32(e + 28, size);
}

static uint32_t add_file(const uint8_t *data, uint32_t size)
{
    uint32_t n = (size + SECTOR - 1) / SECTOR;
    uint32_t c = alloc_chain(n ? n : 1);
    memcpy(cluster_data(c), data, size);
    return c;
}

static void format_fat32(void)
{
    uint32_t n = PART_SECTORS - RESERVED;
    fat_sectors = 1;
    while ((n - FAT_COUNT * fat_sectors) + 2 > fat_sectors * 128u)
        fat_sectors++;
    data_start = RESERVED + FAT_COUNT * fat_sectors;
    uint32_t clusters = PART_SECTORS - data_start;
    if (clusters < 65525u)
    {
        fprintf(stderr, "mkimg: image too small for FAT32\n");
        exit(1);
    }

    uint8_t *b = part; /* boot sector */
    b[0] = 0xEB;
    b[1] = 0x58;
    b[2] = 0x90;
    memcpy(b + 3, "HARISOS ", 8);
    put16(b + 11, SECTOR);
    b[13] = 1; /* sectors per cluster */
    put16(b + 14, RESERVED);
    b[16] = FAT_COUNT;
    b[21] = 0xF8; /* fixed disk */
    put16(b + 24, 32);
    put16(b + 26, 64);
    put32(b + 28, PART_FIRST); /* hidden sectors */
    put32(b + 32, PART_SECTORS);
    put32(b + 36, fat_sectors);
    put32(b + 44, 2); /* root cluster */
    put16(b + 48, 1); /* FSInfo */
    put16(b + 50, 6); /* backup boot sector */
    b[64] = 0x80;
    b[66] = 0x29;
    put32(b + 67, 0x48415249); /* volume id */
    memcpy(b + 71, "HARISOS    ", 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55;
    b[511] = 0xAA;

    uint8_t *fi = part + SECTOR; /* FSInfo */
    put32(fi, 0x41615252);
    put32(fi + 484, 0x61417272);
    put32(fi + 488, 0xFFFFFFFFu);
    put32(fi + 492, 0xFFFFFFFFu);
    fi[510] = 0x55;
    fi[511] = 0xAA;

    put32(fat_entry(0), 0x0FFFFFF8u);
    put32(fat_entry(1), FAT_EOC);
}

/* "dir/hello.elf" -> "HELLO   ELF": the 11 characters of a short FAT name (upper case, padded with spaces) */
static void name83(const char *path, char out[11])
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    const char *dot = strrchr(base, '.');
    size_t name_len = dot ? (size_t)(dot - base) : strlen(base);
    const char *ext = dot ? dot + 1 : "";
    if (name_len == 0 || name_len > 8 || strlen(ext) > 3)
    {
        fprintf(stderr, "mkimg: '%s' does not fit an 8.3 file name\n", base);
        exit(1);
    }
    memset(out, ' ', 11);
    for (size_t i = 0; i < name_len; i++)
        out[i] = (char)((base[i] >= 'a' && base[i] <= 'z') ? base[i] - 32 : base[i]);
    for (size_t i = 0; ext[i]; i++)
        out[8 + i] = (char)((ext[i] >= 'a' && ext[i] <= 'z') ? ext[i] - 32 : ext[i]);
}

#define MAX_EXTRA 14 /* the root folder is one 512-byte cluster: 16 entries, two are used */

int main(int argc, char **argv)
{
    if (argc < 4 || argc - 4 > MAX_EXTRA)
    {
        fprintf(stderr, "usage: mkimg <out.img> <BOOTX64.EFI> <KERNEL.EFI> [file ... (at most %d)]\n", MAX_EXTRA);
        return 1;
    }

    uint32_t loader_size, kernel_size;
    uint8_t *loader = read_file(argv[2], &loader_size);
    uint8_t *kernel = read_file(argv[3], &kernel_size);

    img = calloc(TOTAL_SECTORS, SECTOR);
    if (!img)
    {
        fprintf(stderr, "mkimg: out of memory\n");
        return 1;
    }
    part = img + (size_t)PART_FIRST * SECTOR;
    format_fat32();

    uint32_t root = alloc_chain(1), efi = alloc_chain(1), boot = alloc_chain(1);
    uint32_t c_loader = add_file(loader, loader_size);
    uint32_t c_kernel = add_file(kernel, kernel_size);

    uint8_t *d = cluster_data(root);
    dir_entry(d, "EFI        ", 0x10, efi, 0);
    dir_entry(d + 32, "KERNEL  EFI", 0x20, c_kernel, kernel_size);
    for (int i = 4; i < argc; i++) /* extra files: user programs */
    {
        uint32_t size;
        uint8_t *data = read_file(argv[i], &size);
        char n83[11];
        name83(argv[i], n83);
        dir_entry(d + 32 * (size_t)(i - 2), n83, 0x20, add_file(data, size), size);
        free(data);
    }
    d = cluster_data(efi);
    dir_entry(d, ".          ", 0x10, efi, 0);
    dir_entry(d + 32, "..         ", 0x10, 0, 0);
    dir_entry(d + 64, "BOOT       ", 0x10, boot, 0);
    d = cluster_data(boot);
    dir_entry(d, ".          ", 0x10, boot, 0);
    dir_entry(d + 32, "..         ", 0x10, efi, 0);
    dir_entry(d + 64, "BOOTX64 EFI", 0x20, c_loader, loader_size);

    /* second FAT + backup boot sector */
    memcpy(part + (RESERVED + fat_sectors) * SECTOR, part + RESERVED * SECTOR, (size_t)fat_sectors * SECTOR);
    memcpy(part + 6 * SECTOR, part, SECTOR);
    memcpy(part + 7 * SECTOR, part + SECTOR, SECTOR);

    /* protective MBR */
    uint8_t *m = img + 446;
    m[2] = 0x02;
    m[4] = 0xEE;
    m[5] = m[6] = m[7] = 0xFF;
    put32(m + 8, 1);
    put32(m + 12, TOTAL_SECTORS - 1);
    img[510] = 0x55;
    img[511] = 0xAA;

    /* GPT: one entry, ESP */
    static const uint8_t esp_type[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
    static const uint8_t disk_guid[16] = {'H', 'a', 'r', 'i', 's', 'O', 'S', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    static const uint8_t part_guid[16] = {'H', 'a', 'r', 'i', 's', 'O', 'S', 0, 0, 0, 0, 0, 0, 0, 0, 2};
    uint32_t entries_bytes = 128 * 128;
    uint8_t *pe = img + 2 * SECTOR;
    memcpy(pe, esp_type, 16);
    memcpy(pe + 16, part_guid, 16);
    put64(pe + 32, PART_FIRST);
    put64(pe + 40, PART_LAST);
    const char *label = "EFI System";
    for (int i = 0; label[i]; i++)
        pe[56 + i * 2] = (uint8_t)label[i];
    uint32_t entries_crc = crc32(pe, entries_bytes);

    uint64_t backup_entries = TOTAL_SECTORS - 33, backup_hdr = TOTAL_SECTORS - 1;
    memcpy(img + backup_entries * SECTOR, pe, entries_bytes);

    for (int pass = 0; pass < 2; pass++)
    {
        uint8_t *h = img + (pass ? backup_hdr : 1) * SECTOR;
        memcpy(h, "EFI PART", 8);
        put32(h + 8, 0x00010000);
        put32(h + 12, 92);
        put64(h + 24, pass ? backup_hdr : 1);
        put64(h + 32, pass ? 1 : backup_hdr);
        put64(h + 40, 34);
        put64(h + 48, TOTAL_SECTORS - 34);
        memcpy(h + 56, disk_guid, 16);
        put64(h + 72, pass ? backup_entries : 2);
        put32(h + 80, 128);
        put32(h + 84, 128);
        put32(h + 88, entries_crc);
        put32(h + 16, crc32(h, 92));
    }

    FILE *f = fopen(argv[1], "wb");
    if (!f || fwrite(img, SECTOR, TOTAL_SECTORS, f) != TOTAL_SECTORS)
    {
        fprintf(stderr, "mkimg: cannot write %s\n", argv[1]);
        return 1;
    }
    fclose(f);
    printf("mkimg: wrote %s (%u MiB)\n", argv[1], TOTAL_SECTORS * SECTOR / (1024 * 1024));
    return 0;
}
