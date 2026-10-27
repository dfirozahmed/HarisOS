/* FAT32: listing a folder. */
#include "fat32_internal.h"

/* ---- listing ---------------------------------------------------- */
/* A folder is read in chunks of up to LIST_BUF_BYTES: one disk command per chunk, not one per sector
 * (every command is a polled round trip, which is slow on a real drive). */
#define LIST_BUF_BYTES 16384
static u8 LIST_BUF[LIST_BUF_BYTES];

int fat_list(Fat *f, unsigned dir, FatListFn cb, void *user)
{
    char lfn[FAT_NAME_MAX], name[FAT_NAME_MAX];
    u32 per_read = LIST_BUF_BYTES / f->ss;
    if (per_read < 1)
        per_read = 1;
    int lfn_ok = 0, expect = 0, nlfn = 0;
    if (!f->fat_dirty) /* listing is how a folder is read (again): do not trust what was cached earlier */
    {
        f->fat_buf_lba = ~0ULL;
        if (!f->batch && f->fsinfo_sector && f32_rd_sectors(f, f->fsinfo_sector, 1, f->sec) == 0 && rd32(f->sec) == 0x41615252u)
        {
            u32 fc = rd32(f->sec + 488);
            f->free_count = fc <= f->clusters ? fc : 0xFFFFFFFFu;
        }
    }
    f32_forget_cursor(f);
    u8 lfn_sum = 0;
    int reported = 0;
    u32 index = 0, cl = dir, guard = 0;
    while (valid_cluster(f, cl) && guard++ <= f->clusters)
    {
        for (u32 s = 0; s < f->spc; s += per_read)
        {
            u32 n = f->spc - s < per_read ? f->spc - s : per_read;
            if (f32_rd_sectors(f, cluster_lba(f, cl) + s, n, LIST_BUF))
                return FAT_ERR_IO;
            for (u32 o = 0; o < n * f->ss; o += 32, index++)
            {
                u8 e[32];
                copy(e, LIST_BUF + o, 32);
                if (e[0] == 0x00)
                    return reported; /* end of the folder */
                if (e[0] == 0xE5)
                {
                    lfn_ok = 0;
                    continue;
                }
                if (e[11] == 0x0F) /* a long-name piece */
                {
                    int seq = e[0] & 0x1F;
                    if (e[0] & 0x40)
                    {
                        lfn_ok = seq >= 1 && seq <= 20;
                        expect = seq;
                        nlfn = 1;
                        lfn_sum = e[13];
                        for (int i = 0; i < FAT_NAME_MAX; i++)
                            lfn[i] = 0;
                    }
                    else if (lfn_ok && seq == expect - 1 && e[13] == lfn_sum)
                    {
                        expect = seq;
                        nlfn++;
                    }
                    else
                        lfn_ok = 0;
                    if (lfn_ok)
                        for (int k = 0; k < 13; k++)
                        {
                            u32 ch = rd16(e + LFN_OFFS[k]);
                            u32 pos = (u32)(seq - 1) * 13 + (u32)k;
                            if (pos < 255)
                                lfn[pos] = ch == 0 || ch == 0xFFFF ? 0 : (char)(ch >= 0x20 && ch < 0x7F ? ch : '_');
                        }
                    continue;
                }
                int has_lfn = lfn_ok && expect == 1 && f32_sfn_checksum(e) == lfn_sum;
                int used_lfn = nlfn;
                lfn_ok = 0;
                if ((e[11] & FAT_ATTR_VOLUME) || e[0] == '.')
                    continue; /* the volume label, "." and ".." */
                if (has_lfn)
                {
                    int n = 0;
                    while (n < 255 && lfn[n])
                        name[n] = lfn[n], n++;
                    name[n] = 0;
                }
                else
                {
                    f32_sfn_to_name(e, name);
                    used_lfn = 0;
                }
                FatEntry fe;
                fe.first_cluster = (u32)rd16(e + 20) << 16 | rd16(e + 26);
                fe.size = (e[11] & FAT_ATTR_DIR) ? 0 : rd32(e + 28);
                fe.dir_cluster = dir;
                fe.index = index;
                fe.attr = e[11];
                fe.nlfn = (u8)used_lfn;
                reported++;
                if (cb(name, &fe, user))
                    return reported;
            }
        }
        u32 next;
        int r = f32_fat_get(f, cl, &next);
        if (r)
            return r;
        if (next >= EOC)
            break;
        cl = next;
    }
    return reported;
}
