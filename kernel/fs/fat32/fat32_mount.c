/* FAT32: mount, folder entries by index. */
#include "fat32_internal.h"

/* ---- mount ------------------------------------------------------ */
unsigned fat_root(const Fat *f) { return f->root_cluster; }

/* FAT entry 1 keeps two flags: bit 27 "shut down cleanly" and bit 26 "no disk error"; both must be set. */
int fat_is_clean(Fat *f)
{
    if (f32_rd_sectors(f, f->fat_lba, 1, f->sec))
        return 0;
    u32 e1 = rd32(f->sec + 4);
    return (e1 & 0x08000000u) && (e1 & 0x04000000u);
}

int fat_mount(Fat *f, int blk)
{
    BlkDev *d = blk_get(blk);
    if (!d || d->sector_size < 512 || d->sector_size > FAT_SECTOR_MAX)
        return -1;
    zero(f, sizeof *f);
    f->blk = blk;
    f->ss = d->sector_size;
    f->fat_buf_lba = ~0ULL;
    if (blk_read(blk, 0, 1, f->sec) != 0)
        return -1;
    const u8 *b = f->sec;
    if (b[510] != 0x55 || b[511] != 0xAA)
        return -1;
    u32 bps = rd16(b + 11), spc = b[13], reserved = rd16(b + 14), nfats = b[16];
    u32 root_entries = rd16(b + 17), tot16 = rd16(b + 19), fatsz16 = rd16(b + 22);
    u32 tot32 = rd32(b + 32), fatsz32 = rd32(b + 36), flags = rd16(b + 40), version = rd16(b + 42);
    u32 root = rd32(b + 44), fsinfo = rd16(b + 48);
    if (bps != f->ss || spc == 0 || (spc & (spc - 1)) || spc > 128 || reserved == 0 || nfats == 0 || nfats > 4)
        return -1;
    if (root_entries != 0 || tot16 != 0 || fatsz16 != 0 || fatsz32 == 0 || tot32 == 0 || version != 0 || root < 2)
        return -1; /* FAT12 / FAT16 / not FAT */
    if ((u64)tot32 > d->sectors)
        return -1;
    u64 data = (u64)reserved + (u64)nfats * fatsz32;
    if (data >= tot32)
        return -1;
    u64 clusters = (tot32 - data) / spc;
    if (clusters < 1 || clusters > 0x0FFFFFF5u || (u64)fatsz32 * f->ss / 4 < clusters + 2 || root >= clusters + 2)
        return -1;

    f->spc = spc;
    f->nfats = nfats;
    f->fat_sectors = fatsz32;
    f->fat0_lba = reserved;
    f->mirror = !(flags & 0x80);
    f->fat_lba = reserved;
    if (!f->mirror)
    {
        unsigned active = flags & 0xF;
        if (active >= nfats)
            return -1;
        f->fat_lba += (u64)active * fatsz32;
    }
    f->data_lba = data;
    f->clusters = (u32)clusters;
    f->root_cluster = root;
    f->next_free = 2;
    f->free_count = 0xFFFFFFFFu;
    f->fsinfo_sector = 0;
    if (b[66] == 0x29)
    {
        int n = 0;
        for (int i = 0; i < 11; i++)
            f->label[n++] = (char)b[71 + i];
        while (n > 0 && f->label[n - 1] == ' ')
            n--;
        f->label[n] = 0;
        if (same_name(f->label, "NO NAME"))
            f->label[0] = 0;
    }
    if (fsinfo > 0 && fsinfo < reserved && blk_read(blk, fsinfo, 1, f->sec) == 0)
    {
        const u8 *s = f->sec;
        if (rd32(s) == 0x41615252u && rd32(s + 484) == 0x61417272u && rd32(s + 508) == 0xAA550000u)
        {
            f->fsinfo_sector = fsinfo;
            u32 fc = rd32(s + 488), nf = rd32(s + 492);
            if (fc <= f->clusters)
                f->free_count = fc;
            if (nf >= 2 && nf < f->clusters + 2)
                f->next_free = nf;
        }
    }
    return 0;
}

int fat_space(Fat *f, unsigned long long *total, unsigned long long *free_bytes)
{
    u64 csize = (u64)f->spc * f->ss;
    if (total)
        *total = f->clusters * csize;
    if (f->free_count == 0xFFFFFFFFu && (u64)f->fat_sectors * f->ss > (64ULL << 20))
    {
        if (free_bytes)
            *free_bytes = 0;
        return 0;
    }
    if (f->free_count == 0xFFFFFFFFu)
    {
        u32 n = 0;
        for (u32 c = 2; c < f->clusters + 2; c++)
        {
            u32 v;
            if (f32_fat_get(f, c, &v))
                break;
            n += v == 0;
        }
        f->free_count = n;
    }
    if (free_bytes)
        *free_bytes = f->free_count * csize;
    return 1;
}

/* ---- folder entries by index ------------------------------------ */
u32 f32_per_cluster(const Fat *f) { return f->spc * f->ss / 32; }

/* where slot `index` of the folder lives: sector and byte offset. 1 = found, 0 = past the end of the folder. */
static int slot_loc(Fat *f, u32 dir, u32 index, u64 *lba, u32 *off)
{
    u32 per = f32_per_cluster(f), cl;
    int r = f32_chain_at(f, dir, index / per, &cl);
    if (r <= 0)
        return r;
    u32 byte = (index % per) * 32;
    *lba = cluster_lba(f, cl) + byte / f->ss;
    *off = byte % f->ss;
    return 1;
}

int f32_slot_read(Fat *f, u32 dir, u32 index, u8 out[32])
{
    u64 lba;
    u32 off;
    int r = slot_loc(f, dir, index, &lba, &off);
    if (r <= 0)
        return r;
    if (f32_rd_sectors(f, lba, 1, f->sec))
        return FAT_ERR_IO;
    copy(out, f->sec + off, 32);
    return 1;
}

int f32_slot_write(Fat *f, u32 dir, u32 index, const u8 in[32])
{
    u64 lba;
    u32 off;
    int r = slot_loc(f, dir, index, &lba, &off);
    if (r < 0)
        return r;
    if (r == 0)
        return FAT_ERR_IO;
    if (f32_rd_sectors(f, lba, 1, f->sec))
        return FAT_ERR_IO;
    copy(f->sec + off, in, 32);
    return f32_wr_sectors(f, lba, 1, f->sec);
}
