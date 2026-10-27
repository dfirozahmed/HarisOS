/* NTFS: names, mount, folder listing, file data (the read side). */
#include "ntfs_internal.h"

/* ---- names -------------------------------------------------------- */
/* a $FILE_NAME key (parent ref, times, sizes, flags, name length, namespace, name) -> ASCII name */
static u32 key_name(const u8 *key, u32 key_len, char *out)
{
    if (key_len < 66)
        return 0;
    u32 n = key[64];
    if (66 + n * 2 > key_len)
        return 0;
    u32 o = 0;
    for (u32 i = 0; i < n && o < NTFS_NAME_MAX - 1; i++)
    {
        u32 ch = rd16(key + 66 + 2 * i);
        out[o++] = (char)(ch >= 0x20 && ch < 0x7F ? ch : '_');
    }
    out[o] = 0;
    return o;
}

/* ---- mount -------------------------------------------------------- */
void ntfs_root(NtEntry *out)
{
    zero(out, sizeof *out);
    out->recno = 5;
    out->is_dir = 1;
}

int ntfs_mount(Ntfs *nt, int blk)
{
    BlkDev *d = blk_get(blk);
    if (!d || d->sector_size < 512 || d->sector_size > NTFS_SECTOR_MAX)
        return -1;
    zero(nt, sizeof *nt);
    nt->blk = blk;
    nt->ss = d->sector_size;
    nt->sec_lba = NONE;
    nt->free_clusters = NONE;
    S_FILE_NT = 0; /* anything cached about a file is from another volume or mount */
    if (blk_read(blk, 0, 1, nt->sec) != 0)
        return -1;
    nt->sec_lba = 0;
    const u8 *b = nt->sec;
    static const u8 NAME[8] = {'N', 'T', 'F', 'S', ' ', ' ', ' ', ' '};
    for (int i = 0; i < 8; i++)
        if (b[3 + i] != NAME[i])
            return -1;
    if (b[510] != 0x55 || b[511] != 0xAA || rd16(b + 11) != nt->ss)
        return -1;
    u32 spc = b[13];
    if (spc >= 0x80)
        spc = 1u << (256 - spc); /* big clusters: 2^(256 - n) */
    if (spc == 0 || (spc & (spc - 1)) || spc > 4096)
        return -1;
    u64 total_sectors = rd64(b + 40);
    nt->csize = spc * nt->ss;
    if (total_sectors == 0 || total_sectors > d->sectors)
        return -1;
    nt->clusters = total_sectors / spc;
    nt->mft_lcn = rd64(b + 48);
    nt->mirr_lcn = rd64(b + 56);
    int rec = (signed char)b[64], idx = (signed char)b[68];
    nt->rec_size = rec > 0 ? (u32)rec * nt->csize : 1u << (-rec);
    nt->idx_size = idx > 0 ? (u32)idx * nt->csize : 1u << (-idx);
    if (rec == 0 || idx == 0 || rec < -20 || idx < -20 || (nt->rec_size != 1024 && nt->rec_size != 2048 && nt->rec_size != 4096) ||
        nt->idx_size < 512 || nt->idx_size > sizeof IDXBUF || nt->mft_lcn >= nt->clusters)
        return -1;

    /* record 0 is $MFT itself: its position is in the boot sector; its $DATA tells where all the records are */
    nt->mft.n = 1;
    nt->mft.r[0].vcn = 0;
    nt->mft.r[0].len = (nt->rec_size + nt->csize - 1) / nt->csize;
    nt->mft.r[0].lcn = (s64)nt->mft_lcn;
    if (load_stream(nt, 0, AT_DATA, 0, &S_MAIN) != 0 || !S_MAIN.found || S_MAIN.resident || S_MAIN.runs.n == 0)
        return -1;
    copy(&nt->mft, &S_MAIN.runs, sizeof nt->mft); /* the complete list (also when $MFT has an attribute list) */
    if (S_MAIN.size < (u64)nt->rec_size * 6) /* at least up to the root folder */
        return -1;
    nt->mft_size = S_MAIN.size;

    /* $MFTMirr (record 1) holds copies of the first records; writing one of them must update the copy too */
    if (nt->mirr_lcn && nt->mirr_lcn < nt->clusters && load_stream(nt, 1, AT_DATA, 0, &S_MAIN) == 0 && S_MAIN.found &&
        !S_MAIN.resident && S_MAIN.runs.n == 1 && S_MAIN.runs.r[0].lcn == (s64)nt->mirr_lcn && S_MAIN.size / nt->rec_size <= 16)
        nt->mirr_recs = (unsigned)(S_MAIN.size / nt->rec_size);

    /* volume label: $Volume (record 3), attribute $VOLUME_NAME */
    if (load_stream(nt, 3, AT_VOLUME_NAME, 0, &S_MAIN) == 0 && S_MAIN.found && S_MAIN.resident)
    {
        u32 n = 0;
        for (u32 i = 0; i + 1 < S_MAIN.res_len && n < sizeof nt->label - 1; i += 2)
        {
            u32 ch = rd16(S_MAIN.res + i);
            nt->label[n++] = (char)(ch >= 0x20 && ch < 0x7F ? ch : '_');
        }
        nt->label[n] = 0;
    }
    /* the root folder must be there */
    if (record_read(nt, 5, REC))
        return -1;
    return 0;
}

int ntfs_space(Ntfs *nt, unsigned long long *total, unsigned long long *free_bytes)
{
    if (total)
        *total = nt->clusters * nt->csize;
    if (nt->free_clusters == NONE)
    {
        /* $Bitmap (record 6): one bit per cluster */
        if (load_stream(nt, 6, AT_DATA, 0, &S_AUX) == 0 && S_AUX.found && S_AUX.size * 8 >= nt->clusters &&
            S_AUX.size <= (16ULL << 20))
        {
            u64 used = 0, done = 0, pos = 0;
            int ok = 1;
            /* the bitmap can be megabytes: read it in big pieces (one disk command each), not sector by sector.
             * IDXBUF is free here: the code is not re-entered. */
            u8 *buf = IDXBUF;
            while (ok && done < nt->clusters)
            {
                int r = stream_read(nt, &S_AUX, pos, buf, sizeof IDXBUF);
                if (r <= 0)
                {
                    ok = 0;
                    break;
                }
                for (int i = 0; i < r && done < nt->clusters; i++)
                {
                    if (done + 8 <= nt->clusters)
                    {
                        used += (u64)__builtin_popcount(buf[i]);
                        done += 8;
                    }
                    else
                        for (int bit = 0; bit < 8 && done < nt->clusters; bit++, done++)
                            used += (buf[i] >> bit) & 1;
                }
                pos += (u64)r;
            }
            if (ok)
                nt->free_clusters = nt->clusters - used;
        }
    }
    if (nt->free_clusters == NONE)
    {
        if (free_bytes)
            *free_bytes = 0;
        return 0;
    }
    if (free_bytes)
        *free_bytes = nt->free_clusters * nt->csize;
    return 1;
}

/* ---- folders ------------------------------------------------------ */
typedef struct
{
    NtfsListFn cb;
    void *user;
    int skip_system; /* the root folder: hide records 0..23 */
    int reported;
    int stop;
} ListCtx;

/* the index entries in [base + start, base + end): report each one that has a key */
static void walk_entries(const u8 *base, u32 start, u32 end, ListCtx *c)
{
    u32 o = start;
    while (!c->stop && o + 16 <= end)
    {
        const u8 *e = base + o;
        u32 elen = rd16(e + 8), klen = rd16(e + 10), flags = rd16(e + 12);
        if (elen < 16 || o + elen > end)
            return;
        if (flags & 2) /* the last entry has no key (only maybe a sub-node) */
            return;
        if (klen >= 66 && 16 + klen <= elen)
        {
            u64 ref = rd64(e);
            u32 recno = (u32)(ref & 0xFFFFFFFFull);
            const u8 *key = e + 16;
            u32 ns = key[65];
            if (ns != 2 && !(c->skip_system && recno < 24)) /* DOS-only names repeat the Win32 name */
            {
                if (key_name(key, klen, (char *)NAMEBUF))
                {
                    NtEntry x;
                    zero(&x, sizeof x);
                    x.recno = recno;
                    x.size = clamp32(rd64(key + 48));
                    x.is_dir = (rd32(key + 56) & 0x10000000u) != 0;
                    c->reported++;
                    if (c->cb((const char *)NAMEBUF, &x, c->user))
                    {
                        c->stop = 1;
                        return;
                    }
                }
            }
        }
        o += elen;
    }
}

int ntfs_list(Ntfs *nt, const NtEntry *dir, NtfsListFn cb, void *user)
{
    ListCtx c = {cb, user, dir->recno == 5, 0, 0};
    if (record_read(nt, dir->recno, REC))
        return -1;
    if (!(rd16(REC + 22) & 2)) /* not a folder */
        return -1;
    /* the index root: copy its content out of the record buffer first */
    u32 root_len = 0;
    for (const u8 *a = attr_first(REC, nt->rec_size); a && rd32(a) != AT_END; a = attr_next(REC, nt->rec_size, a))
        if (attr_is(a, AT_INDEX_ROOT, "$I30") && a[8] == 0)
        {
            u32 n = rd32(a + 16), off = rd16(a + 20);
            if (n < 32 || n > sizeof ROOTBUF || off + n > rd32(a + 4))
                return -1;
            copy(ROOTBUF, a + off, n);
            root_len = n;
            break;
        }
    if (!root_len)
        return -1;
    u32 block = rd32(ROOTBUF + 8); /* bytes per index block */
    u32 ent_off = rd32(ROOTBUF + 16), ent_total = rd32(ROOTBUF + 20), root_flags = ROOTBUF[28];
    if (16 + ent_total > root_len)
        return -1;
    walk_entries(ROOTBUF, 16 + ent_off, 16 + ent_total, &c);
    if (c.stop || !(root_flags & 1)) /* no sub-nodes: all entries were in the root */
        return c.reported;
    if (block != nt->idx_size)
        return -1;

    /* the allocated index blocks: only the ones the bitmap marks as used */
    if (load_stream(nt, dir->recno, AT_INDEX_ALLOCATION, "$I30", &S_MAIN) != 0 || !S_MAIN.found || S_MAIN.resident)
        return -1;
    if (load_stream(nt, dir->recno, AT_BITMAP, "$I30", &S_AUX) != 0 || !S_AUX.found)
        return -1;
    u64 nblocks = S_MAIN.size / block;
    /* several blocks per disk command: every command is a polled round trip, slow on a real drive */
    u32 group = block <= sizeof IDXBUF ? (u32)(sizeof IDXBUF / block) : 1;
    for (u64 first = 0; first < nblocks && !c.stop; first += group)
    {
        u32 cnt = nblocks - first < group ? (u32)(nblocks - first) : group;
        u32 used = 0, lo = cnt, hi = 0; /* used blocks, and the span of them inside this group */
        for (u32 i = 0; i < cnt; i++)
        {
            u8 bits;
            u64 n = first + i;
            if (stream_read(nt, &S_AUX, n / 8, &bits, 1) <= 0)
                return -1;
            if ((bits >> (n % 8)) & 1) /* a free block may still hold old entries: skip it */
            {
                used++;
                if (i < lo)
                    lo = i;
                hi = i + 1;
            }
        }
        if (!used)
            continue;
        if (stream_read(nt, &S_MAIN, (first + lo) * block, IDXBUF + (u64)lo * block, (hi - lo) * block) != (int)((hi - lo) * block))
            return -1;
        for (u32 i = lo; i < hi && !c.stop; i++)
        {
            u64 n = first + i;
            u8 bits;
            if (stream_read(nt, &S_AUX, n / 8, &bits, 1) <= 0)
                return -1;
            if (!((bits >> (n % 8)) & 1))
                continue;
            u8 *b = IDXBUF + (u64)i * block;
            if (b[0] != 'I' || b[1] != 'N' || b[2] != 'D' || b[3] != 'X' || apply_fixups(b, block))
                return -1;
            u32 eo = rd32(b + 24), et = rd32(b + 28);
            if (24 + et > block)
                return -1;
            walk_entries(b, 24 + eo, 24 + et, &c);
        }
    }
    return c.reported;
}

/* ---- file data ---------------------------------------------------- */
int ntfs_read(Ntfs *nt, const NtEntry *e, unsigned offset, void *buf, unsigned len)
{
    if (len == 0)
        return 0;
    if (S_FILE_NT != nt || S_FILE_REC != e->recno)
    {
        S_FILE_NT = 0;
        if (load_stream(nt, e->recno, AT_DATA, 0, &S_FILE) != 0)
            return -1;
        S_FILE_NT = nt;
        S_FILE_REC = e->recno;
    }
    if (!S_FILE.found)
        return 0; /* no unnamed data: an empty file */
    if (S_FILE.flags & (FL_COMPRESSED | FL_ENCRYPTED))
        return -2;
    int r = stream_read(nt, &S_FILE, offset, (u8 *)buf, len);
    if (r < 0)
        S_FILE_NT = 0;
    return r;
}
