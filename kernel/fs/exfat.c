/* exFAT file system, read only. See include/exfat.h.
 *
 * Layout (sectors counted from the start of the partition):
 *   boot region    main boot sector with all the sizes, 8 extended boot sectors, checksum; then a backup
 *   FAT            one 32-bit entry per cluster (only used by files that are not stored in one piece)
 *   cluster heap   cluster 2 on: the allocation bitmap, the up-case table, the root folder, all the data
 * A folder is a list of 32-byte entries. One file = an "entry set": a File entry (0x85) saying how many
 * secondary entries follow, then a Stream Extension (0xC0: size, first cluster, flags) and one or more File Name
 * entries (0xC1: 15 UTF-16 characters each). A file whose "NoFatChain" flag is set has its clusters one after
 * the other, without chain in the FAT. */
#include "blk.h"
#include "exfat.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define EOC 0xFFFFFFF8u
#define NONE (~0ULL)

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return (u64)rd32(p) | (u64)rd32(p + 4) << 32; }

static void zero(void *p, unsigned long n)
{
    volatile u8 *b = (volatile u8 *)p;
    for (unsigned long i = 0; i < n; i++)
        b[i] = 0;
}

static void copy(void *dst, const void *src, unsigned long n)
{
    volatile u8 *d = (volatile u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)src;
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
}

static u32 clamp32(u64 v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)v; }

static int valid_cluster(const ExFat *f, u32 cl) { return cl >= 2 && cl < f->clusters + 2; }
static u64 cluster_lba(const ExFat *f, u32 cl) { return f->heap_lba + (u64)(cl - 2) * f->spc; }

/* ---- sectors ------------------------------------------------------ */
static int load_sec(ExFat *f, u64 lba)
{
    if (f->sec_lba == lba)
        return 0;
    if (blk_read(f->blk, lba, 1, f->sec) != 0)
    {
        f->sec_lba = NONE;
        return -1;
    }
    f->sec_lba = lba;
    return 0;
}

static int fat_flush(ExFat *f);

static int fat_get(ExFat *f, u32 cl, u32 *out)
{
    u64 off = (u64)cl * 4;
    u64 lba = f->fat_lba + off / f->ss;
    if (f->fat_buf_lba != lba)
    {
        if (f->fat_dirty && fat_flush(f))
            return -1;
        if (blk_read(f->blk, lba, 1, f->fat_buf) != 0)
        {
            f->fat_buf_lba = NONE;
            return -1;
        }
        f->fat_buf_lba = lba;
    }
    *out = rd32(f->fat_buf + off % f->ss);
    return 0;
}

/* The index-th cluster of a file or folder. 1 and the cluster; 0 if the data ends before it; -1 on an error.
 * A cursor remembers the last chain position so that reading in order stays quick. */
static int cluster_at(ExFat *f, u32 first, int nofat, u32 index, u32 *out)
{
    if (!valid_cluster(f, first))
        return 0;
    if (nofat)
    {
        u64 c = (u64)first + index;
        if (c >= (u64)f->clusters + 2)
            return 0;
        *out = (u32)c;
        return 1;
    }
    u32 c = first, i = 0;
    if (f->cur_first == first && f->cur_index <= index && valid_cluster(f, f->cur_cluster))
    {
        c = f->cur_cluster;
        i = f->cur_index;
    }
    while (i < index)
    {
        u32 next;
        if (fat_get(f, c, &next))
            return -1;
        if (next >= EOC)
            return 0;
        if (!valid_cluster(f, next))
            return -1;
        c = next;
        i++;
    }
    f->cur_first = first;
    f->cur_index = i;
    f->cur_cluster = c;
    *out = c;
    return 1;
}

/* ---- reading data ------------------------------------------------- */
/* bytes [pos, pos+n) of one cluster: the partial first / last sector through the cache, whole sectors directly */
static int cluster_read(ExFat *f, u32 cl, u32 pos, u8 *buf, u32 n)
{
    u64 lba = cluster_lba(f, cl) + pos / f->ss;
    u32 head = pos % f->ss;
    while (n)
    {
        if (head == 0 && n >= f->ss)
        {
            u32 whole = n / f->ss;
            if (blk_read(f->blk, lba, whole, buf) != 0)
                return -1;
            buf += (u64)whole * f->ss;
            n -= whole * f->ss;
            lba += whole;
            continue;
        }
        if (load_sec(f, lba))
            return -1;
        u32 take = f->ss - head < n ? f->ss - head : n;
        copy(buf, f->sec + head, take);
        buf += take;
        n -= take;
        lba++;
        head = 0;
    }
    return 0;
}

/* read `len` bytes of a cluster run starting at byte `offset` of the data (the data must be long enough) */
static int data_read(ExFat *f, u32 first, int nofat, u32 offset, u8 *buf, u32 len)
{
    u32 csize = f->spc * f->ss;
    u32 cl, index = offset / csize, pos = offset % csize;
    int r = cluster_at(f, first, nofat, index, &cl);
    if (r <= 0)
        return -1;
    for (;;)
    {
        u32 n = csize - pos < len ? csize - pos : len;
        if (cluster_read(f, cl, pos, buf, n))
            return -1;
        buf += n;
        len -= n;
        if (!len)
            return 0;
        pos = 0;
        index++;
        r = cluster_at(f, first, nofat, index, &cl);
        if (r <= 0)
            return -1;
    }
}

int exfat_read(ExFat *f, const ExEntry *e, unsigned offset, void *buf, unsigned len)
{
    if (offset >= e->size || len == 0 || e->first_cluster < 2)
        return 0;
    if (len > e->size - offset)
        len = e->size - offset;
    u32 real = offset < e->valid ? (e->valid - offset < len ? e->valid - offset : len) : 0;
    if (real && data_read(f, e->first_cluster, e->nofat, offset, (u8 *)buf, real))
        return -1;
    zero((u8 *)buf + real, len - real); /* beyond ValidDataLength: zeros */
    return (int)len;
}

/* ---- folders ------------------------------------------------------ */
/* the index-th 32-byte entry of a folder: 1 = got it, 0 = past the end, -1 = error */
static int get_entry(ExFat *f, const ExEntry *dir, u32 index, u8 out[32])
{
    u64 byte = (u64)index * 32;
    if (!dir->is_root && byte >= dir->size)
        return 0;
    u32 csize = f->spc * f->ss, cl;
    int r = cluster_at(f, dir->first_cluster, dir->nofat, (u32)(byte / csize), &cl);
    if (r <= 0)
        return r;
    u32 pos = (u32)(byte % csize);
    if (load_sec(f, cluster_lba(f, cl) + pos / f->ss))
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
        int r = get_entry(f, dir, index, e);
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
        r = secondary >= 2 ? get_entry(f, dir, index + 1, st) : 0;
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
            r = get_entry(f, dir, index + k, ne);
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

/* ---- mount -------------------------------------------------------- */
/* the root folder's special entries: the allocation bitmap and the volume label */
static void scan_root(ExFat *f)
{
    ExEntry root;
    exfat_root(f, &root);
    for (u32 index = 0; index < 4096; index++)
    {
        u8 e[32];
        int r = get_entry(f, &root, index, e);
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
            if (load_sec(f, lba + s))
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

/* ====================== changing the volume ====================== */
/* Everything below writes. A changing call ends with finish(): the cached FAT and bitmap sectors are written
 * and the disk is flushed (unless a batch of changes runs). Clusters of new data come from the allocation bitmap
 * (bit n = cluster n + 2); a file gets a FAT chain unless it already is stored in one piece and the clusters after
 * it are free (then it just grows and stays "NoFatChain"). */
static ExFatClockFn CLOCK;
void exfat_set_clock(ExFatClockFn fn) { CLOCK = fn; }

static void wr16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
static void wr32(u8 *p, u32 v)
{
    wr16(p, v);
    wr16(p + 2, v >> 16);
}
static void wr64(u8 *p, u64 v)
{
    wr32(p, (u32)v);
    wr32(p + 4, (u32)(v >> 32));
}

static int writable(const ExFat *f)
{
    const BlkDev *d = blk_get(f->blk);
    if (d && d->parent >= 0)
        d = blk_get(d->parent);
    return d && d->writable && !d->locked;
}

static int wr_sector(ExFat *f, u64 lba, const void *buf)
{
    if (!writable(f))
        return EXFAT_ERR_RO;
    return blk_write(f->blk, lba, 1, buf) == 0 ? 0 : EXFAT_ERR_IO;
}

/* ---- the FAT and the allocation bitmap, cached by sector ---- */
static int fat_flush(ExFat *f)
{
    if (!f->fat_dirty)
        return 0;
    int r = wr_sector(f, f->fat_buf_lba, f->fat_buf);
    if (r)
        return r;
    f->fat_dirty = 0;
    return 0;
}

static int fat_put(ExFat *f, u32 cl, u32 value)
{
    u32 dummy;
    int r = fat_get(f, cl, &dummy); /* loads (and, if needed, flushes) the sector */
    if (r)
        return EXFAT_ERR_IO;
    wr32(f->fat_buf + ((u64)cl * 4) % f->ss, value);
    f->fat_dirty = 1;
    return 0;
}

static int bm_flush(ExFat *f)
{
    if (!f->bm_dirty)
        return 0;
    int r = wr_sector(f, f->bm_lba, f->bm_buf);
    if (r)
        return r;
    f->bm_dirty = 0;
    return 0;
}

static int bm_load(ExFat *f, u64 lba)
{
    if (f->bm_lba == lba)
        return 0;
    int r = bm_flush(f);
    if (r)
        return r;
    if (blk_read(f->blk, lba, 1, f->bm_buf) != 0)
    {
        f->bm_lba = NONE;
        return EXFAT_ERR_IO;
    }
    f->bm_lba = lba;
    return 0;
}

static int bm_get(ExFat *f, u32 cl, int *used)
{
    if (!f->bitmap_cluster)
        return EXFAT_ERR_IO;
    u64 bit = cl - 2, byte = bit / 8;
    int r = bm_load(f, cluster_lba(f, f->bitmap_cluster) + byte / f->ss);
    if (r)
        return r;
    *used = (f->bm_buf[byte % f->ss] >> (bit % 8)) & 1;
    return 0;
}

static int bm_put(ExFat *f, u32 cl, int used)
{
    u64 bit = cl - 2, byte = bit / 8;
    int r = bm_load(f, cluster_lba(f, f->bitmap_cluster) + byte / f->ss);
    if (r)
        return r;
    u8 *p = &f->bm_buf[byte % f->ss];
    int was = (*p >> (bit % 8)) & 1;
    if (used)
        *p |= (u8)(1u << (bit % 8));
    else
        *p &= (u8)~(1u << (bit % 8));
    f->bm_dirty = 1;
    if (f->free_clusters != NONE && was != used)
        f->free_clusters += used ? (u64)-1 : 1;
    return 0;
}

int exfat_sync(ExFat *f)
{
    int r = fat_flush(f);
    if (r)
        return r;
    r = bm_flush(f);
    if (r)
        return r;
    return blk_flush(f->blk) == 0 ? 0 : EXFAT_ERR_IO;
}

static int finish(ExFat *f) { return f->batch ? 0 : exfat_sync(f); }

void exfat_batch(ExFat *f, int on)
{
    f->batch = on != 0;
    if (!on)
        exfat_sync(f);
}

/* a free cluster: marked used in the bitmap, found from `hint` on (wrapping) */
static int alloc_cluster(ExFat *f, u32 hint, u32 *out)
{
    if (!valid_cluster(f, hint))
        hint = f->alloc_hint;
    if (!valid_cluster(f, hint))
        hint = 2;
    for (u32 i = 0; i < f->clusters; i++)
    {
        u32 c = 2 + (hint - 2 + i) % f->clusters;
        int used;
        int r = bm_get(f, c, &used);
        if (r)
            return r;
        if (!used)
        {
            r = bm_put(f, c, 1);
            if (r)
                return r;
            f->alloc_hint = c + 1 < f->clusters + 2 ? c + 1 : 2;
            *out = c;
            return 0;
        }
    }
    return EXFAT_ERR_FULL;
}

static void forget_cursor(ExFat *f) { f->cur_first = 0; }

/* ---- clusters of a file: count, chain, append, trim ---- */
static u32 clusters_of(const ExFat *f, u32 size) { return (u32)(((u64)size + f->spc * f->ss - 1) / (f->spc * f->ss)); }

/* give a "NoFatChain" run of n clusters a FAT chain (so that it can grow or shrink anywhere) */
static int to_chain(ExFat *f, u32 first, u32 n)
{
    for (u32 i = 0; i < n; i++)
    {
        int r = fat_put(f, first + i, i + 1 < n ? first + i + 1 : 0xFFFFFFFFu);
        if (r)
            return r;
    }
    forget_cursor(f);
    return 0;
}

static int zero_cluster(ExFat *f, u32 cl)
{
    static const u8 z[EXFAT_SECTOR_MAX]; /* all zeros (static: a 4 KiB array on the stack would need __chkstk) */
    for (u32 s = 0; s < f->spc; s++)
    {
        int r = wr_sector(f, cluster_lba(f, cl) + s, z);
        if (r)
            return r;
    }
    f->sec_lba = NONE;
    return 0;
}

/* One more cluster at the end of a stream of *n clusters. Updates *first / *nofat / *n. */
static int append_cluster(ExFat *f, u32 *first, u8 *nofat, u32 *n, int zero_it)
{
    u32 c;
    int r;
    if (*n == 0)
    {
        r = alloc_cluster(f, 0, &c);
        if (r)
            return r;
        r = fat_put(f, c, 0xFFFFFFFFu);
        if (r)
            return r;
        *first = c;
        *nofat = 0;
    }
    else
    {
        u32 last;
        r = cluster_at(f, *first, *nofat, *n - 1, &last);
        if (r <= 0)
            return EXFAT_ERR_IO;
        int next_used = 1;
        if (*nofat && valid_cluster(f, last + 1) && bm_get(f, last + 1, &next_used) == 0 && !next_used)
        {
            r = bm_put(f, last + 1, 1); /* the clusters after it are free: it grows in place */
            if (r)
                return r;
            c = last + 1;
        }
        else
        {
            if (*nofat)
            {
                r = to_chain(f, *first, *n);
                if (r)
                    return r;
                *nofat = 0;
            }
            r = alloc_cluster(f, last + 1, &c);
            if (r)
                return r;
            r = fat_put(f, last, c);
            if (r)
                return r;
            r = fat_put(f, c, 0xFFFFFFFFu);
            if (r)
                return r;
        }
    }
    (*n)++;
    forget_cursor(f);
    if (zero_it)
        return zero_cluster(f, c);
    return 0;
}

/* free the clusters of a stream from index `keep` on (keep = 0: all of them) */
static int trim_clusters(ExFat *f, u32 *first, u8 nofat, u32 n, u32 keep)
{
    if (n <= keep || !valid_cluster(f, *first))
        return 0;
    int r;
    if (nofat)
    {
        for (u32 i = keep; i < n; i++)
        {
            r = bm_put(f, *first + i, 0);
            if (r)
                return r;
        }
    }
    else
    {
        u32 c, tail = 0;
        if (keep)
        {
            r = cluster_at(f, *first, 0, keep - 1, &tail);
            if (r <= 0)
                return EXFAT_ERR_IO;
            if (fat_get(f, tail, &c))
                return EXFAT_ERR_IO;
            r = fat_put(f, tail, 0xFFFFFFFFu);
            if (r)
                return r;
        }
        else
            c = *first;
        for (u32 guard = 0; valid_cluster(f, c) && guard <= f->clusters; guard++)
        {
            u32 next;
            if (fat_get(f, c, &next))
                return EXFAT_ERR_IO;
            r = bm_put(f, c, 0);
            if (r)
                return r;
            r = fat_put(f, c, 0);
            if (r)
                return r;
            if (next >= EOC)
                break;
            c = next;
        }
    }
    if (keep == 0)
        *first = 0;
    forget_cursor(f);
    return 0;
}

/* ---- the up-case table (for the name hash) ---- */
static u16 UPCASE[65536];
static const ExFat *UPCASE_FOR;

static void load_upcase(ExFat *f)
{
    if (UPCASE_FOR == f)
        return;
    for (u32 i = 0; i < 65536; i++)
        UPCASE[i] = (u16)(i >= 'a' && i <= 'z' ? i - 32 : i); /* the ASCII fallback */
    UPCASE_FOR = f;
    if (!valid_cluster(f, f->upcase_cluster) || f->upcase_bytes < 4 || f->upcase_bytes > 131072 + 16)
        return;
    u32 csize = f->spc * f->ss, idx = 0, done = 0;
    u16 pending_skip = 0;
    int have_ff = 0;
    u8 buf[512];
    while (done < f->upcase_bytes && idx < 65536)
    {
        u32 n = f->upcase_bytes - done < sizeof buf ? f->upcase_bytes - done : (u32)sizeof buf;
        n &= ~1u;
        if (!n)
            break;
        u32 cl, ci = done / csize;
        if (cluster_at(f, f->upcase_cluster, 1, ci, &cl) <= 0 || cluster_read(f, cl, done % csize, buf, n))
            return;
        for (u32 i = 0; i + 1 < n && idx < 65536; i += 2)
        {
            u16 v = rd16(buf + i);
            if (have_ff)
            {
                have_ff = 0;
                pending_skip = v; /* that many characters map to themselves */
                idx += pending_skip;
            }
            else if (v == 0xFFFF)
                have_ff = 1;
            else
                UPCASE[idx++] = v;
        }
        done += n;
    }
}

static u16 name_hash(ExFat *f, const char *name)
{
    load_upcase(f);
    u32 h = 0;
    for (; *name; name++)
    {
        u16 u = UPCASE[(u8)*name];
        h = ((h << 15) | (h >> 1)) & 0xFFFF;
        h = (h + (u & 0xFF)) & 0xFFFF;
        h = ((h << 15) | (h >> 1)) & 0xFFFF;
        h = (h + (u >> 8)) & 0xFFFF;
    }
    return (u16)h;
}

static u16 set_checksum(const u8 *set, u32 n)
{
    u32 cs = 0;
    for (u32 i = 0; i < n * 32; i++)
    {
        if (i == 2 || i == 3)
            continue;
        cs = (((cs & 1) ? 0x8000 : 0) + (cs >> 1) + set[i]) & 0xFFFF;
    }
    return (u16)cs;
}

static void now(u32 *stamp)
{
    unsigned d = (1u << 5) | 1u, t = 0; /* 1980-01-01 00:00:00 */
    if (CLOCK)
        CLOCK(&d, &t);
    *stamp = ((u32)d << 16) | (t & 0xFFFF);
}

/* ---- names ---- */
static int name_valid(const char *name)
{
    u32 n = 0;
    while (name[n])
        n++;
    if (n == 0 || n > 255 || name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.')
        return 0;
    for (u32 i = 0; i < n; i++)
    {
        u8 c = (u8)name[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
            c == '\\' || c == '|')
            return 0;
    }
    return !(name[0] == '.' && (n == 1 || (n == 2 && name[1] == '.')));
}

static int same_name(const char *a, const char *b)
{
    while (*a && ((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a) == ((*b >= 'A' && *b <= 'Z') ? *b + 32 : *b))
        a++, b++;
    return *a == *b;
}

typedef struct
{
    const char *name;
    u32 skip;
    int found;
} NameCheck;

static int name_cb(const char *name, const ExEntry *e, void *user)
{
    NameCheck *c = (NameCheck *)user;
    if (e->set_index != c->skip && same_name(name, c->name))
        c->found = 1;
    return c->found;
}

static int name_exists(ExFat *f, const ExEntry *dir, const char *name, u32 skip)
{
    NameCheck c = {name, skip, 0};
    int r = exfat_list(f, dir, name_cb, &c);
    return r < 0 ? EXFAT_ERR_IO : c.found;
}

/* ---- entries of a folder ---- */
static int put_entry(ExFat *f, const ExEntry *dir, u32 index, const u8 e[32])
{
    u64 byte = (u64)index * 32;
    u32 csize = f->spc * f->ss, cl;
    int r = cluster_at(f, dir->first_cluster, dir->nofat, (u32)(byte / csize), &cl);
    if (r <= 0)
        return EXFAT_ERR_IO;
    u32 pos = (u32)(byte % csize);
    u64 lba = cluster_lba(f, cl) + pos / f->ss;
    if (load_sec(f, lba))
        return EXFAT_ERR_IO;
    copy(f->sec + pos % f->ss, e, 32);
    return wr_sector(f, lba, f->sec);
}

/* rewrite the stream fields (first cluster, sizes, NoFatChain) and modify time of an entry set, and its checksum */
static int update_set(ExFat *f, const ExEntry *container, const ExEntry *e)
{
    u8 set[32 * 20];
    u32 total = (u32)e->secondary + 1;
    if (total < 2 || total > 20)
        return EXFAT_ERR_ARG;
    for (u32 i = 0; i < total; i++)
    {
        int r = get_entry(f, container, e->set_index + i, set + 32 * i);
        if (r <= 0)
            return EXFAT_ERR_IO;
    }
    u32 stamp;
    now(&stamp);
    wr32(set + 12, stamp);
    set[1 * 32 + 1] = (u8)(1 | (e->nofat ? 2 : 0));
    wr64(set + 32 + 8, e->valid);
    wr32(set + 32 + 20, e->first_cluster);
    wr64(set + 32 + 24, e->size);
    wr16(set + 2, set_checksum(set, total));
    int r = put_entry(f, container, e->set_index, set);
    if (r)
        return r;
    return put_entry(f, container, e->set_index + 1, set + 32);
}

static u32 count_clusters(ExFat *f, const ExEntry *e)
{
    if (e->is_root)
    {
        u32 n = 0, cl;
        while (cluster_at(f, e->first_cluster, 0, n, &cl) > 0 && n < f->clusters)
            n++;
        return n;
    }
    return clusters_of(f, e->size);
}

/* a folder gets one more (empty) cluster; its own entry set (in dir_parent) learns the new length */
static int dir_grow(ExFat *f, ExEntry *dir, const ExEntry *dir_parent)
{
    u32 first = dir->first_cluster, n = count_clusters(f, dir);
    u8 nofat = dir->nofat;
    int r = append_cluster(f, &first, &nofat, &n, 1);
    if (r)
        return r;
    dir->first_cluster = first;
    dir->nofat = nofat;
    if (!dir->is_root)
    {
        u32 csize = f->spc * f->ss;
        if ((u64)dir->size + csize > 0xFFFFFFFFull)
            return EXFAT_ERR_FULL;
        dir->size += csize;
        dir->valid = dir->size;
        if (!dir_parent)
            return EXFAT_ERR_ARG;
        r = update_set(f, dir_parent, dir);
        if (r)
            return r;
    }
    return 0;
}

#define MAX_DIR_ENTRIES 262144u

/* `need` consecutive unused entries in the folder (it grows if it must). Returns the first index. */
static int find_free_run(ExFat *f, ExEntry *dir, const ExEntry *dir_parent, u32 need, u32 *start)
{
    u32 run = 0, first = 0, index = 0;
    for (;;)
    {
        u8 e[32];
        int r = get_entry(f, dir, index, e);
        if (r < 0)
            return EXFAT_ERR_IO;
        if (r == 0)
            break;
        if (!(e[0] & 0x80)) /* unused (0x00 and the deleted ones) */
        {
            if (!run)
                first = index;
            if (++run >= need)
            {
                *start = first;
                return 0;
            }
        }
        else
            run = 0;
        index++;
    }
    u32 per = f->spc * f->ss / 32;
    while (run < need)
    {
        if (index >= MAX_DIR_ENTRIES)
            return EXFAT_ERR_FULL;
        int r = dir_grow(f, dir, dir_parent);
        if (r)
            return r;
        if (!run)
            first = index;
        run += per;
        index += per;
    }
    *start = first;
    return 0;
}

/* build the entry set of a new entry; returns the number of entries */
static u32 make_set(ExFat *f, u8 *set, const char *name, u32 attr, u32 first, u32 size, u32 valid, int nofat, const u8 *times)
{
    u32 nlen = 0;
    while (name[nlen])
        nlen++;
    u32 names = (nlen + 14) / 15, total = 2 + names;
    zero(set, total * 32);
    set[0] = 0x85;
    set[1] = (u8)(total - 1);
    wr16(set + 4, attr);
    if (times)
        copy(set + 8, times, 24);
    else
    {
        u32 stamp;
        now(&stamp);
        wr32(set + 8, stamp);
        wr32(set + 12, stamp);
        wr32(set + 16, stamp);
    }
    u8 *st = set + 32;
    st[0] = 0xC0;
    st[1] = (u8)(1 | (nofat ? 2 : 0));
    st[3] = (u8)nlen;
    wr16(st + 4, name_hash(f, name));
    wr64(st + 8, valid);
    wr32(st + 20, first);
    wr64(st + 24, size);
    for (u32 n = 0; n < names; n++)
    {
        u8 *ne = set + 32 * (2 + n);
        ne[0] = 0xC1;
        for (u32 c = 0; c < 15; c++)
        {
            u32 idx = n * 15 + c;
            wr16(ne + 2 + 2 * c, idx < nlen ? (u8)name[idx] : 0);
        }
    }
    wr16(set + 2, set_checksum(set, total));
    return total;
}

static int write_set(ExFat *f, const ExEntry *dir, u32 index, const u8 *set, u32 total)
{
    for (u32 i = 0; i < total; i++)
    {
        int r = put_entry(f, dir, index + i, set + 32 * i);
        if (r)
            return r;
    }
    return 0;
}

/* mark an entry set as deleted: the "in use" bit of each entry goes */
static int kill_set(ExFat *f, const ExEntry *dir, const ExEntry *e)
{
    for (u32 i = 0; i <= e->secondary; i++)
    {
        u8 b[32];
        int r = get_entry(f, dir, e->set_index + i, b);
        if (r <= 0)
            return EXFAT_ERR_IO;
        b[0] &= 0x7F;
        r = put_entry(f, dir, e->set_index + i, b);
        if (r)
            return r;
    }
    return 0;
}

/* ---- create ---- */
int exfat_create(ExFat *f, ExEntry *dir, const ExEntry *dir_parent, const char *name, int is_dir, ExEntry *out)
{
    if (!writable(f))
        return EXFAT_ERR_RO;
    if (!name_valid(name))
        return EXFAT_ERR_BADNAME;
    int r = name_exists(f, dir, name, 0xFFFFFFFFu);
    if (r < 0)
        return r;
    if (r)
        return EXFAT_ERR_EXISTS;
    u32 first = 0, size = 0, csize = f->spc * f->ss;
    if (is_dir)
    {
        u32 n = 0;
        u8 nofat = 0;
        r = append_cluster(f, &first, &nofat, &n, 1);
        if (r)
        {
            finish(f);
            return r;
        }
        size = csize;
    }
    u8 set[32 * 20];
    u32 total = make_set(f, set, name, is_dir ? EXFAT_ATTR_DIR : 0x20, first, size, size, 0, 0);
    u32 start;
    r = find_free_run(f, dir, dir_parent, total, &start);
    if (r == 0)
        r = write_set(f, dir, start, set, total);
    if (r)
    {
        if (first)
            trim_clusters(f, &first, 0, 1, 0);
        finish(f);
        return r;
    }
    zero(out, sizeof *out);
    out->first_cluster = first;
    out->size = size;
    out->valid = size;
    out->set_index = start;
    out->attr = is_dir ? EXFAT_ATTR_DIR : 0x20;
    out->secondary = (u8)(total - 1);
    return finish(f);
}

/* ---- write ---- */
static int data_write(ExFat *f, u32 first, int nofat, u32 offset, const u8 *buf, u32 len)
{
    u32 csize = f->spc * f->ss, cl, index = offset / csize, pos = offset % csize;
    int r = cluster_at(f, first, nofat, index, &cl);
    if (r <= 0)
        return EXFAT_ERR_IO;
    for (;;)
    {
        u32 n = csize - pos < len ? csize - pos : len;
        u64 lba = cluster_lba(f, cl) + pos / f->ss;
        u32 head = pos % f->ss, left = n;
        const u8 *p = buf;
        while (left)
        {
            if (head == 0 && left >= f->ss)
            {
                u32 whole = left / f->ss;
                if (!writable(f))
                    return EXFAT_ERR_RO;
                if (blk_write(f->blk, lba, whole, p) != 0)
                    return EXFAT_ERR_IO;
                f->sec_lba = NONE;
                p += (u64)whole * f->ss;
                left -= whole * f->ss;
                lba += whole;
                continue;
            }
            if (load_sec(f, lba))
                return EXFAT_ERR_IO;
            u32 take = f->ss - head < left ? f->ss - head : left;
            copy(f->sec + head, p, take);
            r = wr_sector(f, lba, f->sec);
            if (r)
                return r;
            p += take;
            left -= take;
            lba++;
            head = 0;
        }
        buf += n;
        len -= n;
        if (!len)
            return 0;
        pos = 0;
        index++;
        r = cluster_at(f, first, nofat, index, &cl);
        if (r <= 0)
            return EXFAT_ERR_IO;
    }
}

int exfat_write(ExFat *f, const ExEntry *dir, ExEntry *e, unsigned offset, const void *buf, unsigned len)
{
    if (!writable(f))
        return EXFAT_ERR_RO;
    if ((e->attr & EXFAT_ATTR_DIR) || offset > e->size || (e->valid < e->size && offset > e->valid))
        return EXFAT_ERR_ARG;
    if (len == 0)
        return 0;
    if ((u64)offset + len > 0xFFFFFFFFull)
        return EXFAT_ERR_FULL;
    int r;
    if (e->size == 0 && e->first_cluster >= 2) /* an empty file that owns a cluster */
    {
        u32 one = 1, first = e->first_cluster;
        r = trim_clusters(f, &first, e->nofat, one, 0);
        if (r)
            return r;
        e->first_cluster = 0;
    }
    u32 have = e->first_cluster ? clusters_of(f, e->size) : 0;
    u32 orig_have = have, orig_first = e->first_cluster;
    u8 orig_nofat = e->nofat;
    u32 need = clusters_of(f, offset + len);
    u32 first = e->first_cluster;
    u8 nofat = e->nofat;
    while (have < need)
    {
        r = append_cluster(f, &first, &nofat, &have, 0);
        if (r)
        {
            /* out of room: give back what this call took */
            u32 f2 = first;
            trim_clusters(f, &f2, nofat, have, orig_have);
            e->first_cluster = orig_first; /* the entry on the disk was not touched */
            e->nofat = orig_nofat;
            finish(f);
            return r;
        }
    }
    e->first_cluster = first;
    e->nofat = nofat;
    r = data_write(f, first, nofat, offset, (const u8 *)buf, len);
    if (r)
    {
        finish(f);
        return r;
    }
    u32 end = offset + len;
    if (end > e->size)
        e->size = end;
    if (end > e->valid)
        e->valid = end;
    r = update_set(f, dir, e);
    if (r)
        return r;
    r = finish(f);
    return r ? r : (int)len;
}

/* ---- delete, rename, move ---- */
static int first_cb(const char *name, const ExEntry *e, void *user)
{
    (void)name;
    (void)e;
    (void)user;
    return 1;
}

int exfat_delete(ExFat *f, const ExEntry *dir, const ExEntry *e)
{
    if (!writable(f))
        return EXFAT_ERR_RO;
    if (e->secondary < 1)
        return EXFAT_ERR_ARG;
    if (e->attr & EXFAT_ATTR_DIR)
    {
        int n = exfat_list(f, e, first_cb, 0);
        if (n < 0)
            return EXFAT_ERR_IO;
        if (n > 0)
            return EXFAT_ERR_NOTEMPTY;
    }
    int r = kill_set(f, dir, e); /* first the entries: a failure after this loses clusters, not a half entry */
    if (r)
        return r;
    if (e->first_cluster >= 2)
    {
        u32 first = e->first_cluster;
        r = trim_clusters(f, &first, e->nofat, clusters_of(f, e->size), 0);
        if (r)
            return r;
    }
    return finish(f);
}

/* put the same file / folder under another name (and maybe in another folder): a new entry set, then the old one goes */
static int relink(ExFat *f, const ExEntry *old_dir, ExEntry *new_dir, const ExEntry *new_dir_parent, ExEntry *e, const char *name)
{
    u8 old[32];
    int r = get_entry(f, old_dir, e->set_index, old);
    if (r <= 0)
        return EXFAT_ERR_IO;
    u8 set[32 * 20];
    u32 total = make_set(f, set, name, old[4] | (old[5] << 8), e->first_cluster, e->size, e->valid, e->nofat, old + 8);
    u32 start;
    r = find_free_run(f, new_dir, new_dir_parent, total, &start);
    if (r)
        return r;
    r = write_set(f, new_dir, start, set, total);
    if (r)
        return r;
    r = kill_set(f, old_dir, e);
    if (r)
        return r;
    e->set_index = start;
    e->secondary = (u8)(total - 1);
    return 0;
}

int exfat_rename(ExFat *f, ExEntry *dir, const ExEntry *dir_parent, ExEntry *e, const char *new_name)
{
    if (!writable(f))
        return EXFAT_ERR_RO;
    if (!name_valid(new_name))
        return EXFAT_ERR_BADNAME;
    int r = name_exists(f, dir, new_name, e->set_index);
    if (r < 0)
        return r;
    if (r)
        return EXFAT_ERR_EXISTS;
    ExEntry same = *dir;
    r = relink(f, &same, dir, dir_parent, e, new_name);
    if (r)
    {
        finish(f);
        return r;
    }
    return finish(f);
}

int exfat_move(ExFat *f, const ExEntry *old_dir, ExEntry *new_dir, const ExEntry *new_dir_parent, ExEntry *e, const char *name)
{
    if (!writable(f))
        return EXFAT_ERR_RO;
    if (!name_valid(name))
        return EXFAT_ERR_BADNAME;
    int r = name_exists(f, new_dir, name, 0xFFFFFFFFu);
    if (r < 0)
        return r;
    if (r)
        return EXFAT_ERR_EXISTS;
    r = relink(f, old_dir, new_dir, new_dir_parent, e, name);
    if (r)
    {
        finish(f);
        return r;
    }
    return finish(f);
}
