/* exFAT: folders. */
#include "exfat_internal.h"

/* ---- folders ------------------------------------------------------ */
/* the index-th 32-byte entry of a folder: 1 = got it, 0 = past the end, -1 = error */
int exf_get_entry(ExFat *f, const ExEntry *dir, u32 index, u8 out[32])
{
    u64 byte = (u64)index * 32;
    if (!dir->is_root && byte >= dir->size)
        return 0;
    u32 csize = f->spc * f->ss, cl;
    int r = exf_cluster_at(f, dir->first_cluster, dir->nofat, (u32)(byte / csize), &cl);
    if (r <= 0)
        return r;
    u32 pos = (u32)(byte % csize);
    if (exf_load_sec(f, cluster_lba(f, cl) + pos / f->ss))
        return -1;
    copy(out, f->sec + pos % f->ss, 32);
    return 1;
}

/* VolumeFlags (boot sector byte 106): bit 1 = volume dirty, bit 2 = media failure */
int exfat_is_clean(ExFat *f)
{
    if (blk_read(f->blk, 0, 1, f->sec) != 0)
    {
        f->sec_lba = NONE;
        return 0;
    }
    f->sec_lba = NONE; /* f->sec no longer holds the sector it was caching */
    return (rd16(f->sec + 106) & 0x0006u) == 0;
}

void exfat_root(const ExFat *f, ExEntry *out)
{
    zero(out, sizeof *out);
    out->first_cluster = f->root_cluster;
    out->attr = EXFAT_ATTR_DIR;
    out->is_root = 1;
}

int exfat_list(ExFat *f, const ExEntry *dir, ExFatListFn cb, void *user)
{
    int reported = 0;
    u32 index = 0;
    for (;;)
    {
        u8 e[32];
        int r = exf_get_entry(f, dir, index, e);
        if (r < 0)
            return -1;
        if (r == 0 || e[0] == 0x00) /* end of the folder */
            break;
        if (e[0] != 0x85) /* bitmap, up-case table, label, GUID ... or an unused entry */
        {
            index++;
            continue;
        }
        u32 secondary = e[1];
        u8 attr = (u8)rd16(e + 4);
        u8 st[32];
        r = secondary >= 2 ? exf_get_entry(f, dir, index + 1, st) : 0;
        if (r < 0)
            return -1;
        if (r == 0 || st[0] != 0xC0)
        {
            index += secondary + 1; /* not a proper set: skip it */
            continue;
        }
        char name[EXFAT_NAME_MAX];
        u32 nlen = st[3], got = 0;
        for (u32 k = 2; k <= secondary && got < nlen; k++)
        {
            u8 ne[32];
            r = exf_get_entry(f, dir, index + k, ne);
            if (r < 0)
                return -1;
            if (r == 0 || ne[0] != 0xC1)
                break;
            for (int c = 0; c < 15 && got < nlen; c++, got++)
            {
                u32 ch = rd16(ne + 2 + 2 * c);
                name[got] = (char)(ch >= 0x20 && ch < 0x7F ? ch : '_'); /* other characters: '_' */
            }
        }
        name[got] = 0;
        ExEntry x;
        zero(&x, sizeof x);
        x.first_cluster = rd32(st + 20);
        u64 size = rd64(st + 24), valid = rd64(st + 8);
        x.size = clamp32(size);
        x.valid = clamp32(valid < size ? valid : size);
        x.attr = attr;
        x.nofat = (st[1] & 0x02) != 0;
        x.set_index = index;
        x.secondary = (u8)secondary;
        index += secondary + 1;
        if (got == 0 || got < nlen)
            continue; /* broken name */
        if (attr & EXFAT_ATTR_DIR)
            x.valid = x.size;
        reported++;
        if (cb(name, &x, user))
            break;
    }
    return reported;
}
