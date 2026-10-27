/* exFAT: mount. */
#include "exfat_internal.h"

/* ---- mount -------------------------------------------------------- */
/* the root folder's special entries: the allocation bitmap and the volume label */
static void scan_root(ExFat *f)
{
    ExEntry root;
    exfat_root(f, &root);
    for (u32 index = 0; index < 4096; index++)
    {
        u8 e[32];
        int r = exf_get_entry(f, &root, index, e);
        if (r <= 0 || e[0] == 0x00)
            return;
        if (e[0] == 0x81 && !f->bitmap_cluster)
        {
            f->bitmap_cluster = rd32(e + 20);
            f->bitmap_bytes = rd64(e + 24);
        }
        else if (e[0] == 0x82 && !f->upcase_cluster)
        {
            f->upcase_cluster = rd32(e + 20);
            f->upcase_bytes = clamp32(rd64(e + 24));
        }
        else if (e[0] == 0x83)
        {
            int n = 0;
            for (int i = 0; i < e[1] && i < 11; i++)
            {
                u32 ch = rd16(e + 2 + 2 * i);
                f->label[n++] = (char)(ch >= 0x20 && ch < 0x7F ? ch : '_');
            }
            f->label[n] = 0;
        }
    }
}

int exfat_mount(ExFat *f, int blk)
{
    BlkDev *d = blk_get(blk);
    if (!d || d->sector_size < 512 || d->sector_size > EXFAT_SECTOR_MAX)
        return -1;
    zero(f, sizeof *f);
    f->blk = blk;
    f->ss = d->sector_size;
    f->sec_lba = NONE;
    f->fat_buf_lba = NONE;
    f->bm_lba = NONE;
    f->alloc_hint = 2;
    if (blk_read(blk, 0, 1, f->sec) != 0)
        return -1;
    const u8 *b = f->sec;
    static const u8 NAME[8] = {'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};
    for (int i = 0; i < 8; i++)
        if (b[3 + i] != NAME[i])
            return -1;
    for (int i = 11; i < 64; i++)
        if (b[i] != 0)
            return -1; /* "must be zero": this is how an exFAT boot sector differs from FAT / NTFS ones */
    if (b[510] != 0x55 || b[511] != 0xAA)
        return -1;
    u32 bps_shift = b[108], spc_shift = b[109], nfats = b[110];
    u64 vol_len = rd64(b + 72);
    u32 fat_off = rd32(b + 80), fat_len = rd32(b + 84), heap_off = rd32(b + 88), count = rd32(b + 92), root = rd32(b + 96);
    u32 flags = rd16(b + 106);
    if (rd16(b + 104) >> 8 != 1) /* file system revision 1.x */
        return -1;
    if (bps_shift < 9 || bps_shift > 12 || (1u << bps_shift) != f->ss || spc_shift > 25 - bps_shift)
        return -1;
    if (nfats < 1 || nfats > 2 || count == 0 || count > 0xFFFFFFF5u || fat_off < 24 || vol_len > d->sectors)
        return -1;
    if ((u64)fat_len * f->ss / 4 < (u64)count + 2 || (u64)fat_off + (u64)nfats * fat_len > heap_off)
        return -1;
    if ((u64)heap_off + (u64)count * (1u << spc_shift) > vol_len || root < 2 || root >= count + 2)
        return -1;
    f->spc = 1u << spc_shift;
    f->fat_lba = fat_off;
    if (nfats == 2 && (flags & 1)) /* the second FAT is the active one (TexFAT) */
        f->fat_lba += fat_len;
    f->fat_sectors = fat_len;
    f->heap_lba = heap_off;
    f->clusters = count;
    f->root_cluster = root;
    f->free_clusters = NONE;
    scan_root(f);
    return 0;
}

int exfat_space(ExFat *f, unsigned long long *total, unsigned long long *free_bytes)
{
    u64 csize = (u64)f->spc * f->ss;
    if (total)
        *total = (u64)f->clusters * csize;
    if (f->free_clusters == NONE && !f->bm_dirty && f->bitmap_cluster && f->bitmap_bytes && f->bitmap_bytes <= (32ULL << 20) &&
        valid_cluster(f, f->bitmap_cluster) && f->bitmap_bytes * 8 >= f->clusters)
    {
        u64 used = 0, done = 0;
        u64 lba = cluster_lba(f, f->bitmap_cluster);
        int ok = 1;
        for (u64 s = 0; ok && done < f->clusters; s++)
        {
            if (exf_load_sec(f, lba + s))
            {
                ok = 0;
                break;
            }
            for (u32 i = 0; i < f->ss && done < f->clusters; i++)
                for (int bit = 0; bit < 8 && done < f->clusters; bit++, done++)
                    used += (f->sec[i] >> bit) & 1;
        }
        if (ok)
            f->free_clusters = f->clusters - used;
    }
    if (f->free_clusters == NONE)
    {
        if (free_bytes)
            *free_bytes = 0;
        return 0;
    }
    if (free_bytes)
        *free_bytes = f->free_clusters * csize;
    return 1;
}
