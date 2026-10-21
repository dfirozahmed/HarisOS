/* FAT32 file system. See include/fat32.h.
 *
 * Layout of a FAT32 volume (all in sectors, counted from the start of the partition):
 *   reserved area (boot sector "BPB", FSInfo, backup boot sector ...)
 *   FAT #1, FAT #2   one 32-bit entry per cluster: 0 = free, next cluster number, or >= 0x0FFFFFF8 = last of the chain
 *   data area        cluster 2, 3, 4 ...; the root folder is a chain of clusters too
 * A folder is a list of 32-byte entries. A file with a long name has some "long name" entries (attribute
 * 0x0F, 13 UCS-2 characters each, last piece first) right before its short 8.3 entry. */
#include "blk.h"
#include "fat32.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define EOC 0x0FFFFFF8u /* values >= this end a chain */
#define FAT_MASK 0x0FFFFFFFu
#define MAX_DIR_ENTRIES 65536u

static FatClockFn CLOCK;

void fat_set_clock(FatClockFn fn) { CLOCK = fn; }
unsigned fat_make_date(unsigned year, unsigned month, unsigned day)
{
    if (year < 1980)
        year = 1980;
    return ((year - 1980) << 9) | (month << 5) | day;
}
unsigned fat_make_time(unsigned hour, unsigned minute, unsigned second) { return (hour << 11) | (minute << 5) | (second / 2); }

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void wr16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

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

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static int same_name(const char *a, const char *b)
{
    while (*a && lower((u8)*a) == lower((u8)*b))
        a++, b++;
    return *a == *b;
}

/* ---- block device access --------------------------------------- */
static int writable(const Fat *f)
{
    const BlkDev *d = blk_get(f->blk);
    if (!d)
        return 0;
    const BlkDev *disk = d->parent >= 0 ? blk_get(d->parent) : d;
    return disk && disk->writable && !d->locked; /* the lock is the partition's own (see blk_set_locked) */
}

static int rd_sectors(Fat *f, u64 lba, unsigned n, void *buf) { return blk_read(f->blk, lba, n, buf) == 0 ? 0 : FAT_ERR_IO; }

static int wr_sectors(Fat *f, u64 lba, unsigned n, const void *buf)
{
    if (!writable(f))
        return FAT_ERR_RO;
    return blk_write(f->blk, lba, n, buf) == 0 ? 0 : FAT_ERR_IO;
}

static u64 cluster_lba(const Fat *f, u32 cl) { return f->data_lba + (u64)(cl - 2) * f->spc; }
static int valid_cluster(const Fat *f, u32 cl) { return cl >= 2 && cl < f->clusters + 2; }

/* ---- the FAT ---------------------------------------------------- */
static int fat_flush_cache(Fat *f)
{
    if (!f->fat_dirty)
        return 0;
    u32 rel = (u32)f->fat_buf_lba;
    for (unsigned k = 0; k < f->nfats; k++)
    {
        u64 lba = f->fat0_lba + (u64)k * f->fat_sectors + rel;
        if (!f->mirror && lba != f->fat_lba + rel)
            continue; /* only the active FAT is kept up to date */
        int r = wr_sectors(f, lba, 1, f->fat_buf);
        if (r)
            return r;
    }
    f->fat_dirty = 0;
    return 0;
}

static int fat_load(Fat *f, u32 rel)
{
    if (f->fat_buf_lba == rel)
        return 0;
    int r = fat_flush_cache(f);
    if (r)
        return r;
    r = rd_sectors(f, f->fat_lba + rel, 1, f->fat_buf);
    if (r)
    {
        f->fat_buf_lba = ~0ULL;
        return r;
    }
    f->fat_buf_lba = rel;
    return 0;
}

static int fat_get(Fat *f, u32 cl, u32 *out)
{
    u32 off = cl * 4;
    int r = fat_load(f, off / f->ss);
    if (r)
        return r;
    *out = rd32(f->fat_buf + off % f->ss) & FAT_MASK;
    return 0;
}

static int fat_set(Fat *f, u32 cl, u32 value)
{
    u32 off = cl * 4;
    int r = fat_load(f, off / f->ss);
    if (r)
        return r;
    u8 *p = f->fat_buf + off % f->ss;
    wr32(p, (rd32(p) & ~FAT_MASK) | (value & FAT_MASK)); /* the top 4 bits are reserved: keep them */
    f->fat_dirty = 1;
    return 0;
}

/* The index-th cluster of a chain (0 = the first). Returns 1 and the cluster, 0 if the chain ends before it,
 * or an error. A cursor remembers the last position so that reading or writing a file in order is quick. */
static int chain_at(Fat *f, u32 first, u32 index, u32 *out)
{
    if (!valid_cluster(f, first))
        return 0;
    u32 c = first, i = 0;
    if (f->cur_first == first && f->cur_index <= index && valid_cluster(f, f->cur_cluster))
    {
        c = f->cur_cluster;
        i = f->cur_index;
    }
    while (i < index)
    {
        u32 next;
        int r = fat_get(f, c, &next);
        if (r)
            return r;
        if (next >= EOC)
            return 0;
        if (!valid_cluster(f, next))
            return FAT_ERR_IO; /* free (0) or bad cluster in the middle of a chain: damaged */
        c = next;
        i++;
    }
    f->cur_first = first;
    f->cur_index = i;
    f->cur_cluster = c;
    *out = c;
    return 1;
}

static void forget_cursor(Fat *f) { f->cur_first = 0; }

static int alloc_cluster(Fat *f, u32 *out)
{
    for (u32 i = 0; i < f->clusters; i++)
    {
        u32 c = 2 + (f->next_free - 2 + i) % f->clusters;
        u32 v;
        int r = fat_get(f, c, &v);
        if (r)
            return r;
        if (v == 0)
        {
            r = fat_set(f, c, 0x0FFFFFFF);
            if (r)
                return r;
            f->next_free = c + 1 < f->clusters + 2 ? c + 1 : 2;
            if (f->free_count != 0xFFFFFFFFu && f->free_count)
                f->free_count--;
            *out = c;
            return 0;
        }
    }
    return FAT_ERR_FULL;
}

/* free a whole chain starting at `c` */
static int free_chain(Fat *f, u32 c)
{
    forget_cursor(f);
    for (u32 guard = 0; valid_cluster(f, c) && guard <= f->clusters; guard++)
    {
        u32 next;
        int r = fat_get(f, c, &next);
        if (r)
            return r;
        r = fat_set(f, c, 0);
        if (r)
            return r;
        if (f->free_count != 0xFFFFFFFFu)
            f->free_count++;
        if (c < f->next_free)
            f->next_free = c; /* the next search can start here */
        if (next >= EOC)
            break;
        c = next;
    }
    return 0;
}

static int zero_cluster(Fat *f, u32 cl)
{
    zero(f->sec, f->ss);
    for (unsigned s = 0; s < f->spc; s++)
    {
        int r = wr_sectors(f, cluster_lba(f, cl) + s, 1, f->sec);
        if (r)
            return r;
    }
    return 0;
}

int fat_sync(Fat *f)
{
    int r = fat_flush_cache(f);
    if (r)
        return r;
    if (f->fsinfo_sector && f->free_count != 0xFFFFFFFFu && writable(f))
    {
        r = rd_sectors(f, f->fsinfo_sector, 1, f->sec);
        if (r == 0 && rd32(f->sec) == 0x41615252u)
        {
            wr32(f->sec + 488, f->free_count);
            wr32(f->sec + 492, f->next_free);
            r = wr_sectors(f, f->fsinfo_sector, 1, f->sec);
        }
        if (r)
            return r;
    }
    return blk_flush(f->blk) == 0 ? 0 : FAT_ERR_IO;
}

/* the end of a changing call: write everything out, unless a batch of changes is running */
static int finish(Fat *f) { return f->batch ? 0 : fat_sync(f); }

void fat_batch(Fat *f, int on)
{
    f->batch = on != 0;
    if (!on)
        fat_sync(f);
}

/* ---- mount ------------------------------------------------------ */
unsigned fat_root(const Fat *f) { return f->root_cluster; }

/* FAT entry 1 keeps two flags: bit 27 "shut down cleanly" and bit 26 "no disk error"; both must be set. */
int fat_is_clean(Fat *f)
{
    if (rd_sectors(f, f->fat_lba, 1, f->sec))
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
            if (fat_get(f, c, &v))
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
static u32 per_cluster(const Fat *f) { return f->spc * f->ss / 32; }

/* where slot `index` of the folder lives: sector and byte offset. 1 = found, 0 = past the end of the folder. */
static int slot_loc(Fat *f, u32 dir, u32 index, u64 *lba, u32 *off)
{
    u32 per = per_cluster(f), cl;
    int r = chain_at(f, dir, index / per, &cl);
    if (r <= 0)
        return r;
    u32 byte = (index % per) * 32;
    *lba = cluster_lba(f, cl) + byte / f->ss;
    *off = byte % f->ss;
    return 1;
}

static int slot_read(Fat *f, u32 dir, u32 index, u8 out[32])
{
    u64 lba;
    u32 off;
    int r = slot_loc(f, dir, index, &lba, &off);
    if (r <= 0)
        return r;
    if (rd_sectors(f, lba, 1, f->sec))
        return FAT_ERR_IO;
    copy(out, f->sec + off, 32);
    return 1;
}

static int slot_write(Fat *f, u32 dir, u32 index, const u8 in[32])
{
    u64 lba;
    u32 off;
    int r = slot_loc(f, dir, index, &lba, &off);
    if (r < 0)
        return r;
    if (r == 0)
        return FAT_ERR_IO;
    if (rd_sectors(f, lba, 1, f->sec))
        return FAT_ERR_IO;
    copy(f->sec + off, in, 32);
    return wr_sectors(f, lba, 1, f->sec);
}

/* ---- names ------------------------------------------------------ */
static u8 sfn_checksum(const u8 *n)
{
    u8 sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (u8)(((sum & 1) << 7) + (sum >> 1) + n[i]);
    return sum;
}

static int name_valid(const char *name)
{
    unsigned n = slen(name);
    if (n == 0 || n > 255 || name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.')
        return 0;
    for (unsigned i = 0; i < n; i++)
    {
        u8 c = (u8)name[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
            c == '\\' || c == '|')
            return 0;
    }
    return 1;
}

/* characters allowed in a short name besides letters and digits */
static int sfn_char_ok(u8 c) { return c > 0x20 && c < 0x7F && !(c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']' || c == '.'); }

/* Short name for `name`. Returns 1 if the name needs long-name entries as well (it does not fit 8.3 exactly,
 * has mixed case in a part, or characters a short name cannot hold). `tail` > 0 adds "~tail". */
static int make_sfn(const char *name, u8 sfn[11], u8 *ntres, unsigned tail)
{
    unsigned n = slen(name), dot = n;
    for (unsigned i = n; i > 0; i--)
        if (name[i - 1] == '.')
        {
            dot = i - 1;
            break;
        }
    unsigned bstart = 0;
    int lossy = 0;
    if (dot == 0) /* ".name": the short name drops the dot */
    {
        bstart = 1;
        dot = n;
        lossy = 1;
        for (unsigned i = n; i > 1; i--)
            if (name[i - 1] == '.')
            {
                dot = i - 1;
                break;
            }
        if (dot < 1)
            dot = n;
    }
    for (int i = 0; i < 11; i++)
        sfn[i] = ' ';
    int bl = 0, bu = 0, el = 0, eu = 0;
    unsigned bn = 0, en = 0;
    for (unsigned i = bstart; i < dot; i++)
    {
        u8 c = (u8)name[i];
        if (c == '.' || c == ' ')
        {
            lossy = 1; /* extra dots and spaces are dropped */
            continue;
        }
        if (!sfn_char_ok(c))
        {
            c = '_';
            lossy = 1;
        }
        if (c >= 'a' && c <= 'z')
            bl = 1;
        if (c >= 'A' && c <= 'Z')
            bu = 1;
        if (bn < 8)
            sfn[bn++] = (u8)upper(c);
        else
            lossy = 1;
    }
    for (unsigned i = dot + 1; dot < n && i < n; i++)
    {
        u8 c = (u8)name[i];
        if (c == ' ')
        {
            lossy = 1;
            continue;
        }
        if (!sfn_char_ok(c))
        {
            c = '_';
            lossy = 1;
        }
        if (c >= 'a' && c <= 'z')
            el = 1;
        if (c >= 'A' && c <= 'Z')
            eu = 1;
        if (en < 3)
            sfn[8 + en++] = (u8)upper(c);
        else
            lossy = 1;
    }
    if (bn == 0)
    {
        sfn[0] = '_';
        bn = 1;
        lossy = 1;
    }
    int need_lfn = lossy || (bl && bu) || (el && eu);
    *ntres = 0;
    if (!need_lfn)
    {
        if (bl)
            *ntres |= 0x08;
        if (el)
            *ntres |= 0x10;
        if (sfn[0] == 0xE5)
            sfn[0] = 0x05;
        return 0;
    }
    if (tail)
    {
        char t[8];
        int tn = 0;
        t[tn++] = '~';
        char digits[8];
        int dn = 0;
        for (unsigned v = tail; v; v /= 10)
            digits[dn++] = (char)('0' + v % 10);
        while (dn)
            t[tn++] = digits[--dn];
        unsigned keep = 8 - (unsigned)tn;
        if (bn < keep)
            keep = bn;
        for (int i = 0; i < tn; i++)
            sfn[keep + i] = (u8)t[i];
        for (unsigned i = keep + (unsigned)tn; i < 8; i++)
            sfn[i] = ' ';
    }
    if (sfn[0] == 0xE5)
        sfn[0] = 0x05;
    return 1;
}

/* the name of a short entry: "NAME.EXT" (lower-case parts as the NT flags say) */
static void sfn_to_name(const u8 *e, char *out)
{
    int n = 0, lowb = (e[12] & 0x08) != 0, lowe = (e[12] & 0x10) != 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++)
    {
        int c = (i == 0 && e[0] == 0x05) ? 0xE5 : e[i];
        out[n++] = (char)(c >= 0x20 && c < 0x7F ? (lowb ? lower(c) : c) : '_');
    }
    if (e[8] != ' ')
    {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++)
            out[n++] = (char)(e[i] >= 0x20 && e[i] < 0x7F ? (lowe ? lower(e[i]) : e[i]) : '_');
    }
    out[n] = 0;
}

static const u8 LFN_OFFS[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

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
        if (!f->batch && f->fsinfo_sector && rd_sectors(f, f->fsinfo_sector, 1, f->sec) == 0 && rd32(f->sec) == 0x41615252u)
        {
            u32 fc = rd32(f->sec + 488);
            f->free_count = fc <= f->clusters ? fc : 0xFFFFFFFFu;
        }
    }
    forget_cursor(f);
    u8 lfn_sum = 0;
    int reported = 0;
    u32 index = 0, cl = dir, guard = 0;
    while (valid_cluster(f, cl) && guard++ <= f->clusters)
    {
        for (u32 s = 0; s < f->spc; s += per_read)
        {
            u32 n = f->spc - s < per_read ? f->spc - s : per_read;
            if (rd_sectors(f, cluster_lba(f, cl) + s, n, LIST_BUF))
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
                int has_lfn = lfn_ok && expect == 1 && sfn_checksum(e) == lfn_sum;
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
                    sfn_to_name(e, name);
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
        int r = fat_get(f, cl, &next);
        if (r)
            return r;
        if (next >= EOC)
            break;
        cl = next;
    }
    return reported;
}

/* ---- reading and writing file data ------------------------------ */
/* bytes [pos, pos+n) of one cluster, in sectors: partial first / last sector through f->sec, the middle directly */
static int cluster_io(Fat *f, u32 cl, u32 pos, u8 *buf, u32 n, int write)
{
    u64 lba = cluster_lba(f, cl) + pos / f->ss;
    u32 head = pos % f->ss;
    while (n)
    {
        if (head == 0 && n >= f->ss)
        {
            u32 whole = n / f->ss;
            int r = write ? wr_sectors(f, lba, whole, buf) : rd_sectors(f, lba, whole, buf);
            if (r)
                return r;
            buf += (u64)whole * f->ss;
            n -= whole * f->ss;
            lba += whole;
            continue;
        }
        u32 take = f->ss - head < n ? f->ss - head : n;
        if (write)
        {
            if (rd_sectors(f, lba, 1, f->sec))
                return FAT_ERR_IO;
            copy(f->sec + head, buf, take);
            int r = wr_sectors(f, lba, 1, f->sec);
            if (r)
                return r;
        }
        else
        {
            if (rd_sectors(f, lba, 1, f->sec))
                return FAT_ERR_IO;
            copy(buf, f->sec + head, take);
        }
        buf += take;
        n -= take;
        lba++;
        head = 0;
    }
    return 0;
}

/* read or write `len` bytes of a chain starting at byte `offset`; the chain must be long enough */
static int chain_io(Fat *f, u32 first, u32 offset, u8 *buf, u32 len, int write)
{
    u32 csize = f->spc * f->ss;
    u32 cl;
    int r = chain_at(f, first, offset / csize, &cl);
    if (r <= 0)
        return r < 0 ? r : FAT_ERR_IO;
    u32 pos = offset % csize;
    for (;;)
    {
        u32 n = csize - pos < len ? csize - pos : len;
        r = cluster_io(f, cl, pos, buf, n, write);
        if (r)
            return r;
        buf += n;
        len -= n;
        if (!len)
            return 0;
        pos = 0;
        u32 next;
        r = fat_get(f, cl, &next);
        if (r)
            return r;
        if (!valid_cluster(f, next))
            return FAT_ERR_IO;
        cl = next;
        f->cur_first = first; /* keep the cursor with us */
        f->cur_index = f->cur_index + 1;
        f->cur_cluster = cl;
    }
}

int fat_read(Fat *f, const FatEntry *e, unsigned offset, void *buf, unsigned len)
{
    if (offset >= e->size || len == 0)
        return 0;
    if (len > e->size - offset)
        len = e->size - offset;
    if (e->first_cluster < 2)
        return 0;
    int r = chain_io(f, e->first_cluster, offset, (u8 *)buf, len, 0);
    return r ? r : (int)len;
}

/* ---- changing entries ------------------------------------------- */
static void now(unsigned *date, unsigned *time)
{
    if (CLOCK)
        CLOCK(date, time);
    else
    {
        *date = fat_make_date(1980, 1, 1);
        *time = 0;
    }
}

/* the 32-byte short entry of `e`, rewritten with new cluster / size / time */
static int update_entry(Fat *f, const FatEntry *e, int touch)
{
    u8 raw[32];
    int r = slot_read(f, e->dir_cluster, e->index, raw);
    if (r <= 0)
        return r < 0 ? r : FAT_ERR_IO;
    wr16(raw + 20, e->first_cluster >> 16);
    wr16(raw + 26, e->first_cluster & 0xFFFF);
    wr32(raw + 28, (e->attr & FAT_ATTR_DIR) ? 0 : e->size);
    if (touch)
    {
        unsigned d, t;
        now(&d, &t);
        wr16(raw + 22, t);
        wr16(raw + 24, d);
        wr16(raw + 18, d);
        raw[11] |= FAT_ATTR_ARCHIVE;
    }
    return slot_write(f, e->dir_cluster, e->index, raw);
}

int fat_write(Fat *f, FatEntry *e, unsigned offset, const void *buf, unsigned len)
{
    if (!writable(f))
        return FAT_ERR_RO;
    if ((e->attr & FAT_ATTR_DIR) || offset > e->size)
        return FAT_ERR_ARG;
    if (len == 0)
        return 0;
    if ((u64)offset + len > 0xFFFFFFFFull)
        return FAT_ERR_FULL;
    u32 csize = f->spc * f->ss;
    if (e->size == 0 && e->first_cluster >= 2) /* an empty file that still owns a cluster (made by another system) */
    {
        int fr = free_chain(f, e->first_cluster);
        if (fr)
            return fr;
        e->first_cluster = 0;
    }
    u32 have = (e->size + csize - 1) / csize; /* clusters in the chain now */
    u32 orig_have = have, orig_first = e->first_cluster;
    u32 need = (u32)(((u64)offset + len + csize - 1) / csize);
    int r;
    while (have < need)
    {
        u32 c;
        r = alloc_cluster(f, &c);
        if (r)
        {
            /* out of room (or an error): give back what this call took, so the file keeps its old size and chain */
            if (have > orig_have)
            {
                u32 tail, extra;
                if (orig_have == 0)
                    free_chain(f, e->first_cluster);
                else if (chain_at(f, e->first_cluster, orig_have - 1, &tail) > 0 && fat_get(f, tail, &extra) == 0)
                {
                    fat_set(f, tail, 0x0FFFFFFF);
                    if (valid_cluster(f, extra))
                        free_chain(f, extra);
                }
                forget_cursor(f);
            }
            e->first_cluster = orig_first;
            finish(f);
            return r;
        }
        if (have == 0)
            e->first_cluster = c;
        else
        {
            u32 tail;
            r = chain_at(f, e->first_cluster, have - 1, &tail);
            if (r <= 0)
                return r < 0 ? r : FAT_ERR_IO;
            r = fat_set(f, tail, c);
            if (r)
                return r;
        }
        have++;
    }
    r = chain_io(f, e->first_cluster, offset, (u8 *)buf, len, 1);
    if (r)
    {
        finish(f);
        return r;
    }
    if (offset + len > e->size)
        e->size = offset + len;
    r = update_entry(f, e, 1);
    if (r)
        return r;
    r = finish(f);
    return r ? r : (int)len;
}

int fat_shrink(Fat *f, FatEntry *e, unsigned new_size)
{
    if (!writable(f))
        return FAT_ERR_RO;
    if ((e->attr & FAT_ATTR_DIR) || new_size > e->size)
        return FAT_ERR_ARG;
    u32 csize = f->spc * f->ss;
    u32 keep = (new_size + csize - 1) / csize;
    int r = 0;
    if (e->first_cluster >= 2)
    {
        if (keep == 0)
        {
            r = free_chain(f, e->first_cluster);
            e->first_cluster = 0;
        }
        else
        {
            u32 last, after;
            r = chain_at(f, e->first_cluster, keep - 1, &last);
            if (r <= 0)
                return r < 0 ? r : FAT_ERR_IO;
            r = fat_get(f, last, &after);
            if (r == 0 && after < EOC && valid_cluster(f, after))
            {
                r = fat_set(f, last, 0x0FFFFFFF);
                if (r == 0)
                    r = free_chain(f, after);
            }
        }
    }
    if (r)
        return r;
    e->size = new_size;
    forget_cursor(f);
    r = update_entry(f, e, 1);
    return r ? r : finish(f);
}

/* is `name` (ignoring case) already used in folder `dir` by an entry other than `skip` (index, or ~0)? */
typedef struct
{
    const char *name;
    u32 skip;
    int found;
} NameCheck;

static int name_cb(const char *name, const FatEntry *e, void *user)
{
    NameCheck *c = (NameCheck *)user;
    if (e->index != c->skip && same_name(name, c->name))
        c->found = 1;
    return c->found;
}

static int name_exists(Fat *f, u32 dir, const char *name, u32 skip)
{
    NameCheck c = {name, skip, 0};
    int r = fat_list(f, dir, name_cb, &c);
    return r < 0 ? r : c.found;
}

/* is the 11-byte short name used in `dir` by an entry other than `skip`? */
static int sfn_exists(Fat *f, u32 dir, const u8 *sfn, u32 skip)
{
    u32 cl = dir, index = 0, guard = 0;
    while (valid_cluster(f, cl) && guard++ <= f->clusters)
    {
        for (u32 s = 0; s < f->spc; s++)
        {
            if (rd_sectors(f, cluster_lba(f, cl) + s, 1, f->sec))
                return FAT_ERR_IO;
            for (u32 o = 0; o < f->ss; o += 32, index++)
            {
                const u8 *e = f->sec + o;
                if (e[0] == 0x00)
                    return 0;
                if (e[0] == 0xE5 || e[11] == 0x0F || index == skip)
                    continue;
                int same = 1;
                for (int i = 0; i < 11 && same; i++)
                    same = e[i] == sfn[i];
                if (same)
                    return 1;
            }
        }
        u32 next;
        int r = fat_get(f, cl, &next);
        if (r)
            return r;
        if (next >= EOC)
            break;
        cl = next;
    }
    return 0;
}

/* `need` consecutive free slots in the folder (growing it if it must). Returns the first index or an error. */
static int find_free_run(Fat *f, u32 dir, u32 need, u32 *start)
{
    u32 run = 0, first = 0, index = 0;
    for (;;)
    {
        u8 e[32];
        int r = slot_read(f, dir, index, e);
        if (r < 0)
            return r;
        if (r == 0)
            break; /* past the last cluster of the folder */
        if (e[0] == 0x00 || e[0] == 0xE5)
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
    u32 per = per_cluster(f);
    while (run < need)
    {
        if (index >= MAX_DIR_ENTRIES)
            return FAT_ERR_FULL;
        u32 c, tail;
        int r = alloc_cluster(f, &c);
        if (r)
            return r;
        r = zero_cluster(f, c);
        if (r)
            return r;
        r = chain_at(f, dir, index / per - 1, &tail);
        if (r <= 0)
            return r < 0 ? r : FAT_ERR_IO;
        r = fat_set(f, tail, c);
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

/* write the entries (long-name pieces + short entry) for `name` into folder `dir`. `proto` supplies attribute,
 * times, cluster and size (32 bytes, name field ignored). */
static int place_entry(Fat *f, u32 dir, const char *name, const u8 proto[32], u32 skip, FatEntry *out)
{
    u8 sfn[11], ntres;
    int need_lfn = make_sfn(name, sfn, &ntres, 0);
    if (need_lfn)
    {
        for (unsigned tail = 1;; tail++)
        {
            if (tail > 999999)
                return FAT_ERR_FULL;
            make_sfn(name, sfn, &ntres, tail);
            int r = sfn_exists(f, dir, sfn, skip);
            if (r < 0)
                return r;
            if (!r)
                break;
        }
    }
    else
    {
        int r = sfn_exists(f, dir, sfn, skip);
        if (r < 0)
            return r;
        if (r)
        {
            need_lfn = 1; /* same short name, different long name: give it a tail */
            for (unsigned tail = 1;; tail++)
            {
                if (tail > 999999)
                    return FAT_ERR_FULL;
                make_sfn(name, sfn, &ntres, tail);
                r = sfn_exists(f, dir, sfn, skip);
                if (r < 0)
                    return r;
                if (!r)
                    break;
            }
            ntres = 0;
        }
    }
    unsigned n = slen(name);
    u32 nlfn = need_lfn ? (n + 12) / 13 : 0;
    u32 start;
    int r = find_free_run(f, dir, nlfn + 1, &start);
    if (r)
        return r;

    u8 sum = sfn_checksum(sfn);
    for (u32 k = nlfn; k >= 1; k--) /* the piece with the highest number comes first */
    {
        u8 e[32];
        zero(e, 32);
        e[0] = (u8)(k | (k == nlfn ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (u32 i = 0; i < 13; i++)
        {
            u32 pos = (k - 1) * 13 + i;
            u32 ch = pos < n ? (u8)name[pos] : pos == n ? 0x0000 : 0xFFFF;
            wr16(e + LFN_OFFS[i], ch);
        }
        r = slot_write(f, dir, start + (nlfn - k), e);
        if (r)
            return r;
    }
    u8 e[32];
    copy(e, proto, 32);
    for (int i = 0; i < 11; i++)
        e[i] = sfn[i];
    e[12] = ntres;
    r = slot_write(f, dir, start + nlfn, e);
    if (r)
        return r;
    out->first_cluster = (u32)rd16(e + 20) << 16 | rd16(e + 26);
    out->size = (e[11] & FAT_ATTR_DIR) ? 0 : rd32(e + 28);
    out->dir_cluster = dir;
    out->index = start + nlfn;
    out->attr = e[11];
    out->nlfn = (u8)nlfn;
    return 0;
}

int fat_create(Fat *f, unsigned dir, const char *name, int is_dir, FatEntry *out)
{
    if (!writable(f))
        return FAT_ERR_RO;
    if (!valid_cluster(f, dir))
        return FAT_ERR_ARG;
    if (!name_valid(name) || (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))))
        return FAT_ERR_BADNAME;
    int r = name_exists(f, dir, name, 0xFFFFFFFFu);
    if (r < 0)
        return r;
    if (r)
        return FAT_ERR_EXISTS;

    u8 proto[32];
    zero(proto, 32);
    unsigned d, t;
    now(&d, &t);
    proto[11] = is_dir ? FAT_ATTR_DIR : FAT_ATTR_ARCHIVE;
    wr16(proto + 14, t);
    wr16(proto + 16, d);
    wr16(proto + 18, d);
    wr16(proto + 22, t);
    wr16(proto + 24, d);
    u32 cluster = 0;
    if (is_dir)
    {
        r = alloc_cluster(f, &cluster);
        if (r)
            return r;
        r = zero_cluster(f, cluster);
        if (r)
            return r;
        u8 dots[32];
        copy(dots, proto, 32); /* "." and ".." share the attribute and times */
        for (int i = 0; i < 11; i++)
            dots[i] = ' ';
        dots[0] = '.';
        wr16(dots + 20, cluster >> 16);
        wr16(dots + 26, cluster & 0xFFFF);
        r = slot_write(f, cluster, 0, dots);
        if (r)
            return r;
        dots[1] = '.';
        u32 parent = dir == f->root_cluster ? 0 : dir; /* ".." of a folder in the root points to 0 */
        wr16(dots + 20, parent >> 16);
        wr16(dots + 26, parent & 0xFFFF);
        r = slot_write(f, cluster, 1, dots);
        if (r)
            return r;
        wr16(proto + 20, cluster >> 16);
        wr16(proto + 26, cluster & 0xFFFF);
    }
    r = place_entry(f, dir, name, proto, 0xFFFFFFFFu, out);
    if (r)
    {
        if (cluster)
            free_chain(f, cluster);
        finish(f);
        return r;
    }
    return finish(f);
}

/* mark the entries of `e` (long-name pieces and short entry) as deleted */
static int kill_entries(Fat *f, const FatEntry *e)
{
    for (u32 i = e->index - e->nlfn; i <= e->index; i++)
    {
        u8 raw[32];
        int r = slot_read(f, e->dir_cluster, i, raw);
        if (r <= 0)
            return r < 0 ? r : FAT_ERR_IO;
        raw[0] = 0xE5;
        r = slot_write(f, e->dir_cluster, i, raw);
        if (r)
            return r;
    }
    return 0;
}

static int first_cb(const char *name, const FatEntry *e, void *user)
{
    (void)name;
    (void)e;
    (void)user;
    return 1;
}

int fat_delete(Fat *f, const FatEntry *e)
{
    if (!writable(f))
        return FAT_ERR_RO;
    if (e->nlfn > e->index)
        return FAT_ERR_ARG;
    if (e->attr & FAT_ATTR_DIR)
    {
        int n = fat_list(f, e->first_cluster, first_cb, 0);
        if (n < 0)
            return n;
        if (n > 0)
            return FAT_ERR_NOTEMPTY;
    }
    int r = kill_entries(f, e); /* first the entries: if the next step fails we have lost clusters, not a broken entry */
    if (r)
        return r;
    if (e->first_cluster >= 2)
    {
        r = free_chain(f, e->first_cluster);
        if (r)
            return r;
    }
    return finish(f);
}

int fat_rename(Fat *f, FatEntry *e, const char *new_name)
{
    if (!writable(f))
        return FAT_ERR_RO;
    if (!name_valid(new_name) || (new_name[0] == '.' && (new_name[1] == 0 || (new_name[1] == '.' && new_name[2] == 0))))
        return FAT_ERR_BADNAME;
    int r = name_exists(f, e->dir_cluster, new_name, e->index);
    if (r < 0)
        return r;
    if (r)
        return FAT_ERR_EXISTS;
    u8 proto[32];
    r = slot_read(f, e->dir_cluster, e->index, proto);
    if (r <= 0)
        return r < 0 ? r : FAT_ERR_IO;
    FatEntry neu;
    r = place_entry(f, e->dir_cluster, new_name, proto, e->index, &neu);
    if (r)
        return r;
    r = kill_entries(f, e);
    if (r)
        return r;
    *e = neu;
    return finish(f);
}

int fat_move(Fat *f, FatEntry *e, const char *name, unsigned new_dir)
{
    if (!writable(f))
        return FAT_ERR_RO;
    if (!valid_cluster(f, new_dir))
        return FAT_ERR_ARG;
    if (new_dir == e->dir_cluster)
        return 0;
    if (!name_valid(name))
        return FAT_ERR_BADNAME;
    int r = name_exists(f, new_dir, name, 0xFFFFFFFFu);
    if (r < 0)
        return r;
    if (r)
        return FAT_ERR_EXISTS;
    u8 proto[32];
    r = slot_read(f, e->dir_cluster, e->index, proto);
    if (r <= 0)
        return r < 0 ? r : FAT_ERR_IO;
    FatEntry neu;
    r = place_entry(f, new_dir, name, proto, 0xFFFFFFFFu, &neu);
    if (r)
        return r;
    r = kill_entries(f, e);
    if (r)
        return r;
    if (e->attr & FAT_ATTR_DIR) /* the folder's ".." must point to its new parent */
    {
        u8 dots[32];
        r = slot_read(f, e->first_cluster, 1, dots);
        if (r <= 0)
            return r < 0 ? r : FAT_ERR_IO;
        u32 parent = new_dir == f->root_cluster ? 0 : new_dir;
        wr16(dots + 20, parent >> 16);
        wr16(dots + 26, parent & 0xFFFF);
        r = slot_write(f, e->first_cluster, 1, dots);
        if (r)
            return r;
    }
    *e = neu;
    return finish(f);
}
