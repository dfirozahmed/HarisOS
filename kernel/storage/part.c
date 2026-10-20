/* Partition tables: GPT and MBR (primary partitions). See include/blk.h. */
#include "blk.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 le64(const u8 *p) { return (u64)le32(p) | (u64)le32(p + 4) << 32; }
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* CRC-32 (the one GPT uses), computed bit by bit: the tables are small enough that speed does not matter */
static u32 crc32_update(u32 crc, const u8 *p, u32 n)
{
    crc = ~crc;
    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32)-(int)(crc & 1));
    }
    return ~crc;
}

static u8 SEC[BLK_SECTOR_MAX];
static u8 HDR[BLK_SECTOR_MAX];

/* GPT type GUIDs in their on-disk byte order */
static const u8 GUID_ESP[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
static const u8 GUID_BASIC[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
static const u8 GUID_LINUX[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0x47};

static int same16(const u8 *a, const u8 *b)
{
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int is_zero16(const u8 *a)
{
    for (int i = 0; i < 16; i++)
        if (a[i])
            return 0;
    return 1;
}

/* Does sector 0 look like a FAT boot sector? (then the whole disk is one volume, not a partition table) */
static int looks_like_fat_boot(const u8 *s, unsigned sector_size)
{
    if (s[0] != 0xEB && s[0] != 0xE9)
        return 0;
    u32 bps = le16(s + 11);
    u32 spc = s[13];
    u32 fats = s[16];
    return bps == sector_size && spc != 0 && (spc & (spc - 1)) == 0 && (fats == 1 || fats == 2) && le16(s + 14) != 0;
}

static void add_part(int disk, int no, u64 first, u64 count, int kind, const char *label)
{
    BlkDev *d = blk_get(disk);
    BlkDev p;
    blk_copy(&p, d);
    int n = 0;
    for (; d->name[n] && n < 10; n++)
        p.name[n] = d->name[n];
    p.name[n++] = 'p';
    if (no >= 10)
        p.name[n++] = (char)('0' + no / 10);
    p.name[n++] = (char)('0' + no % 10);
    p.name[n] = 0;
    p.kind = BLK_PART;
    p.parent = disk;
    p.sectors = count;
    p.first_lba = first;
    p.part_kind = kind;
    p.part_no = no;
    p.read = 0;
    p.write = 0;
    p.flush = 0;
    p.ctx = 0;
    int i = 0;
    for (; label[i] && i < (int)sizeof p.part_label - 1; i++)
        p.part_label[i] = label[i];
    p.part_label[i] = 0;
    int idx = blk_register(&p);
    if (idx >= 0)
        blk_get(idx)->locked = d->locked;
}

static int scan_gpt(int disk)
{
    BlkDev *d = blk_get(disk);
    unsigned ss = d->sector_size;
    if (ss > BLK_SECTOR_MAX || d->sectors < 3)
        return 0;
    if (blk_read(disk, 1, 1, HDR) != 0)
        return 0;
    static const u8 SIG[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (int i = 0; i < 8; i++)
        if (HDR[i] != SIG[i])
            return 0;
    u32 hsize = le32(HDR + 12);
    if (hsize < 92 || hsize > ss)
        return 0;
    u32 hcrc = le32(HDR + 16);
    HDR[16] = HDR[17] = HDR[18] = HDR[19] = 0;
    if (crc32_update(0, HDR, hsize) != hcrc)
        return 0;
    u64 entries_lba = le64(HDR + 72);
    u32 count = le32(HDR + 80);
    u32 esize = le32(HDR + 84);
    u32 ecrc = le32(HDR + 88);
    u64 first_usable = le64(HDR + 40), last_usable = le64(HDR + 48);
    if (esize < 128 || esize > 1024 || (esize & 127) || count == 0 || count > 256 || entries_lba < 2 ||
        last_usable >= d->sectors || first_usable > last_usable)
        return 0;
    u64 total = (u64)count * esize;
    u64 nsec = (total + ss - 1) / ss;
    if (entries_lba + nsec > d->sectors)
        return 0;

    /* the CRC covers the whole entry array: check it first, then read the entries again to use them */
    u32 crc = 0;
    u64 left = total;
    for (u64 s = 0; s < nsec; s++)
    {
        if (blk_read(disk, entries_lba + s, 1, SEC) != 0)
            return 0;
        u32 n = left < ss ? (u32)left : ss;
        crc = crc32_update(crc, SEC, n);
        left -= n;
    }
    if (crc != ecrc)
        return 0;

    int added = 0;
    for (u32 i = 0; i < count; i++)
    {
        u64 byte = (u64)i * esize;
        if (blk_read(disk, entries_lba + byte / ss, 1, SEC) != 0)
            return added;
        const u8 *e = SEC + byte % ss;
        if (is_zero16(e))
            continue; /* unused slot */
        u64 first = le64(e + 32), last = le64(e + 40);
        if (first < first_usable || last > last_usable || last < first)
            continue;
        int kind = same16(e, GUID_ESP)     ? PART_ESP
                   : same16(e, GUID_BASIC) ? PART_FAT
                   : same16(e, GUID_LINUX) ? PART_LINUX
                                           : PART_OTHER;
        char label[40];
        int n = 0;
        for (int k = 0; k < 36 && n < (int)sizeof label - 1; k++)
        {
            u16 ch = le16(e + 56 + 2 * k);
            if (!ch)
                break;
            label[n++] = ch < 128 && ch >= 32 ? (char)ch : '?';
        }
        label[n] = 0;
        add_part(disk, added + 1, first, last - first + 1, kind, label);
        added++;
    }
    return added;
}

static int scan_mbr(int disk, const u8 *s)
{
    BlkDev *d = blk_get(disk);
    int added = 0;
    for (int i = 0; i < 4; i++)
    {
        const u8 *e = s + 446 + 16 * i;
        u8 type = e[4];
        u64 first = le32(e + 8), count = le32(e + 12);
        if (type == 0 || count == 0 || first + count > d->sectors)
            continue;
        if (type == 0x05 || type == 0x0F || type == 0x85) /* extended partitions: not followed yet */
            continue;
        int kind = (type == 0xEF)                                                      ? PART_ESP
                   : (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C || type == 0x0E) ? PART_FAT
                   : (type == 0x07)                                                    ? PART_NTFS_EXFAT
                   : (type == 0x83)                                                    ? PART_LINUX
                                                                                       : PART_OTHER;
        add_part(disk, added + 1, first, count, kind, "");
        added++;
    }
    return added;
}

int part_scan(int disk)
{
    BlkDev *d = blk_get(disk);
    if (!d || d->parent >= 0 || d->sector_size < 512 || d->sector_size > BLK_SECTOR_MAX)
        return 0;
    if (blk_read(disk, 0, 1, SEC) != 0)
        return 0;
    if (SEC[510] != 0x55 || SEC[511] != 0xAA)
        return 0;
    if (looks_like_fat_boot(SEC, d->sector_size))
        return 0; /* a file system from sector 0 */
    int protective = 0;
    for (int i = 0; i < 4; i++)
        if (SEC[446 + 16 * i + 4] == 0xEE)
            protective = 1;
    if (protective)
        return scan_gpt(disk);
    u8 mbr[512];
    for (int i = 0; i < 512; i++) /* scan_mbr only needs the first 512 bytes; SEC may be reused */
        mbr[i] = SEC[i];
    return scan_mbr(disk, mbr);
}
