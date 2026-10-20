/* NTFS file system, read only. See include/ntfs.h.
 *
 * NTFS in short: everything is a file, described by a 1 KiB "record" in the Master File Table ($MFT, itself a file;
 * record 5 is the root folder). A record holds attributes: $FILE_NAME (0x30), $DATA (0x80: the content),
 * $INDEX_ROOT (0x90) and $INDEX_ALLOCATION (0xA0) for folders, $ATTRIBUTE_LIST (0x20) when a file has too much for
 * one record. An attribute is "resident" (its content is right there) or "non-resident": then it holds data runs,
 * a compact list of (length, start cluster) pieces. On disk each 512-byte sector of a record or index block ends
 * with a copy of a sequence number instead of the real last two bytes ("fixups"); the real bytes sit in an array at
 * the start of the record. A folder is a B-tree of $FILE_NAME entries (name, size, record of the file): the root of
 * the tree is in $INDEX_ROOT, the rest in 4 KiB "INDX" blocks of $INDEX_ALLOCATION. To list a folder we do not follow
 * the tree: every block that is marked used in the index's $BITMAP holds some of the entries, so we read them all. */
#include "blk.h"
#include "ntfs.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;

#define NONE (~0ULL)
#define RES_MAX 4096 /* resident data we can hold */

enum
{
    AT_STANDARD_INFORMATION = 0x10,
    AT_ATTRIBUTE_LIST = 0x20,
    AT_FILE_NAME = 0x30,
    AT_VOLUME_NAME = 0x60,
    AT_DATA = 0x80,
    AT_INDEX_ROOT = 0x90,
    AT_INDEX_ALLOCATION = 0xA0,
    AT_BITMAP = 0xB0
};
#define AT_END 0xFFFFFFFFu /* ends the attributes of a record */

#define FL_COMPRESSED 0x0001
#define FL_ENCRYPTED 0x4000

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

/* One attribute's content, as found in the MFT. */
typedef struct
{
    int resident;
    u8 res[RES_MAX]; /* resident content */
    u32 res_len;
    NtRuns runs;     /* non-resident */
    u64 size;        /* real size in bytes */
    u64 init;        /* initialized size: bytes after it read as zero */
    u16 flags;       /* compressed / encrypted / sparse */
    int found;
} NtStream;

/* Scratch space shared by all volumes: the code is not re-entered. Big things stay out of the stack. */
static NtStream S_MAIN, S_AUX, S_FILE; /* S_FILE: the file being read (kept between calls) */
static const Ntfs *S_FILE_NT;
static unsigned S_FILE_REC = ~0u;
static u8 REC[4096], REC2[4096];
static u8 ALBUF[32768];
static u8 ROOTBUF[RES_MAX];
static u8 IDXBUF[16384];
static u8 NAMEBUF[NTFS_NAME_MAX * 2];

/* ---- reading the volume ------------------------------------------ */
/* `len` bytes at byte offset `off` of the volume */
static int vol_read(Ntfs *nt, u64 off, u8 *buf, u32 len)
{
    while (len)
    {
        u64 lba = off / nt->ss;
        u32 head = (u32)(off % nt->ss);
        if (head == 0 && len >= nt->ss)
        {
            u32 whole = len / nt->ss;
            if (blk_read(nt->blk, lba, whole, buf) != 0)
                return -1;
            buf += (u64)whole * nt->ss;
            off += (u64)whole * nt->ss;
            len -= whole * nt->ss;
            continue;
        }
        if (nt->sec_lba != lba)
        {
            if (blk_read(nt->blk, lba, 1, nt->sec) != 0)
            {
                nt->sec_lba = NONE;
                return -1;
            }
            nt->sec_lba = lba;
        }
        u32 take = nt->ss - head < len ? nt->ss - head : len;
        copy(buf, nt->sec + head, take);
        buf += take;
        off += take;
        len -= take;
    }
    return 0;
}

/* Read from a run list: `len` bytes at byte `offset` of the stream. Holes read as zeros. */
static int runs_read(Ntfs *nt, const NtRuns *rs, u64 offset, u8 *buf, u32 len)
{
    while (len)
    {
        u64 vcn = offset / nt->csize;
        u32 in = (u32)(offset % nt->csize);
        const NtRun *run = 0;
        for (int i = 0; i < rs->n; i++)
            if (vcn >= rs->r[i].vcn && vcn < rs->r[i].vcn + rs->r[i].len)
            {
                run = &rs->r[i];
                break;
            }
        if (!run)
            return -1; /* beyond what the runs describe */
        u64 left_in_run = (run->vcn + run->len - vcn) * nt->csize - in;
        u32 n = left_in_run < len ? (u32)left_in_run : len;
        if (run->lcn < 0)
            zero(buf, n);
        else if (vol_read(nt, ((u64)run->lcn + (vcn - run->vcn)) * nt->csize + in, buf, n))
            return -1;
        buf += n;
        offset += n;
        len -= n;
    }
    return 0;
}

/* ---- data runs ---------------------------------------------------- */
/* Decode the run list in [p, end) and append it to rs; the first run starts at virtual cluster start_vcn. */
static int parse_runs(const u8 *p, const u8 *end, u64 start_vcn, NtRuns *rs)
{
    u64 vcn = start_vcn;
    s64 lcn = 0;
    while (p < end && *p)
    {
        u32 nlen = *p & 0x0F, noff = *p >> 4;
        p++;
        if (nlen == 0 || nlen > 8 || noff > 8 || p + nlen + noff > end)
            return -1;
        u64 len = 0;
        for (u32 i = 0; i < nlen; i++)
            len |= (u64)p[i] << (8 * i);
        p += nlen;
        s64 delta = 0;
        for (u32 i = 0; i < noff; i++)
            delta |= (s64)((u64)p[i] << (8 * i));
        if (noff && (p[noff - 1] & 0x80)) /* sign extension */
            for (u32 i = noff; i < 8; i++)
                delta |= (s64)((u64)0xFF << (8 * i));
        p += noff;
        if (rs->n >= NT_MAX_RUNS)
            return -1;
        NtRun *r = &rs->r[rs->n++];
        r->vcn = vcn;
        r->len = len;
        if (noff == 0)
            r->lcn = -1; /* a hole */
        else
        {
            lcn += delta;
            r->lcn = lcn;
        }
        vcn += len;
    }
    return 0;
}

/* ---- records ------------------------------------------------------ */
/* Undo the fixups of a record or index block of `len` bytes: put the real last two bytes back at the end of
 * each 512-byte sector. */
static int apply_fixups(u8 *b, u32 len)
{
    u32 usa = rd16(b + 4), n = rd16(b + 6);
    if (n < 2 || usa + n * 2 > len || (n - 1) * 512 != len)
        return -1;
    u16 usn = rd16(b + usa);
    for (u32 i = 1; i < n; i++)
    {
        u8 *end = b + i * 512 - 2;
        if (rd16(end) != usn)
            return -1; /* the record was torn: not written completely */
        end[0] = b[usa + 2 * i];
        end[1] = b[usa + 2 * i + 1];
    }
    return 0;
}

static int record_read(Ntfs *nt, u32 recno, u8 *buf)
{
    if (runs_read(nt, &nt->mft, (u64)recno * nt->rec_size, buf, nt->rec_size))
        return -1;
    if (buf[0] != 'F' || buf[1] != 'I' || buf[2] != 'L' || buf[3] != 'E')
        return -1;
    if (apply_fixups(buf, nt->rec_size))
        return -1;
    if (!(rd16(buf + 22) & 1)) /* not in use */
        return -1;
    return 0;
}

/* the first attribute of a record, and the next one; NULL at the end */
static const u8 *attr_first(const u8 *rec, u32 rec_size)
{
    u32 o = rd16(rec + 20);
    return (o + 24 <= rec_size) ? rec + o : (const u8 *)0;
}

static const u8 *attr_next(const u8 *rec, u32 rec_size, const u8 *a)
{
    u32 len = rd32(a + 4);
    u32 off = (u32)(a - rec) + len;
    if (len < 16 || (len & 7) || off + 8 > rec_size)
        return (const u8 *)0;
    return rec + off;
}

/* is this attribute of the wanted type and name (ASCII; NULL = unnamed)? */
static int attr_is(const u8 *a, u32 type, const char *name)
{
    if (rd32(a) != type)
        return 0;
    u32 nl = a[9];
    if (!name)
        return nl == 0;
    u32 want = 0;
    while (name[want])
        want++;
    if (nl != want)
        return 0;
    for (u32 i = 0; i < nl; i++)
        if (rd16(a + rd16(a + 10) + 2 * i) != (u8)name[i])
            return 0;
    return 1;
}

/* add one attribute (an "extent" of a stream) to s */
static int stream_add(const u8 *a, u32 attr_len, NtStream *s)
{
    if (a[8] == 0) /* resident */
    {
        u32 n = rd32(a + 16), off = rd16(a + 20);
        if (off + n > attr_len || n > RES_MAX)
            return -1;
        copy(s->res, a + off, n);
        s->res_len = n;
        s->resident = 1;
        s->size = s->init = n;
        s->flags = rd16(a + 12);
        s->found = 1;
        return 0;
    }
    u64 start = rd64(a + 16);
    u32 runs_off = rd16(a + 32);
    if (runs_off > attr_len)
        return -1;
    if (start == 0) /* the first extent carries the sizes */
    {
        s->resident = 0;
        s->size = rd64(a + 48);
        s->init = rd64(a + 56);
        s->flags = rd16(a + 12);
        s->runs.n = 0;
        s->found = 1;
    }
    else if (!s->found)
        return -1;
    return parse_runs(a + runs_off, a + attr_len, start, &s->runs);
}

/* Find the attribute (type, name) of record `recno` and load it into s. s->found = 0 if there is none. Follows the
 * $ATTRIBUTE_LIST into extension records. Uses REC (the record) and REC2. */
static int load_stream(Ntfs *nt, u32 recno, u32 type, const char *name, NtStream *s)
{
    s->found = 0;
    s->runs.n = 0;
    s->res_len = 0;
    s->size = s->init = 0;
    s->flags = 0;
    s->resident = 0;
    if (record_read(nt, recno, REC))
        return -1;
    u32 rs = nt->rec_size;
    const u8 *list = 0;
    for (const u8 *a = attr_first(REC, rs); a && rd32(a) != AT_END; a = attr_next(REC, rs, a))
    {
        if (rd32(a) == AT_ATTRIBUTE_LIST)
            list = a;
        else if (attr_is(a, type, name) && !list)
        {
            /* in the base record: a normal attribute; if a list shows up later it will redo this */
            if (stream_add(a, rd32(a + 4), s))
                return -1;
        }
    }
    if (!list)
        return 0;

    /* an attribute list: read it, then take every extent of the wanted attribute in order */
    u32 list_len;
    if (list[8] == 0)
    {
        list_len = rd32(list + 16);
        u32 off = rd16(list + 20);
        if (list_len > sizeof ALBUF || off + list_len > rd32(list + 4))
            return -1;
        copy(ALBUF, list + off, list_len);
    }
    else
    {
        u64 real = rd64(list + 48);
        if (real > sizeof ALBUF)
            return -1;
        static NtRuns lr;
        lr.n = 0;
        u32 roff = rd16(list + 32);
        if (parse_runs(list + roff, list + rd32(list + 4), rd64(list + 16), &lr))
            return -1;
        if (runs_read(nt, &lr, 0, ALBUF, (u32)real))
            return -1;
        list_len = (u32)real;
    }
    s->found = 0;
    s->runs.n = 0;
    for (u32 o = 0; o + 26 <= list_len;)
    {
        const u8 *le = ALBUF + o;
        u32 elen = rd16(le + 4);
        if (elen < 26 || o + elen > list_len)
            break;
        u32 etype = rd32(le), nl = le[6], noff = le[7];
        int same = etype == type;
        if (same)
        {
            if (!name)
                same = nl == 0;
            else
            {
                u32 want = 0;
                while (name[want])
                    want++;
                same = nl == want;
                for (u32 i = 0; same && i < nl; i++)
                    if (noff + 2 * i + 1 >= elen || rd16(le + noff + 2 * i) != (u8)name[i])
                        same = 0;
            }
        }
        if (same)
        {
            u64 ref = rd64(le + 16) & 0xFFFFFFFFFFFFull;
            u16 id = rd16(le + 24);
            u8 *rec = (ref == recno) ? REC : REC2;
            if (ref != recno && record_read(nt, (u32)ref, REC2))
                return -1;
            for (const u8 *a = attr_first(rec, rs); a && rd32(a) != AT_END; a = attr_next(rec, rs, a))
                if (rd32(a) == etype && rd16(a + 14) == id)
                {
                    if (stream_add(a, rd32(a + 4), s))
                        return -1;
                    break;
                }
        }
        o += elen;
    }
    return 0;
}

/* read from a loaded stream */
static int stream_read(Ntfs *nt, const NtStream *s, u64 offset, u8 *buf, u32 len)
{
    if (offset >= s->size)
        return 0;
    if (len > s->size - offset)
        len = (u32)(s->size - offset);
    if (s->resident)
    {
        copy(buf, s->res + offset, len);
        return (int)len;
    }
    u32 real = 0;
    if (offset < s->init)
        real = (s->init - offset < len) ? (u32)(s->init - offset) : len;
    if (real && runs_read(nt, &s->runs, offset, buf, real))
        return -1;
    zero(buf + real, len - real); /* after the initialized size */
    return (int)len;
}

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
            u8 buf[512];
            while (ok && done < nt->clusters)
            {
                int r = stream_read(nt, &S_AUX, pos, buf, sizeof buf);
                if (r <= 0)
                {
                    ok = 0;
                    break;
                }
                for (int i = 0; i < r && done < nt->clusters; i++)
                    for (int bit = 0; bit < 8 && done < nt->clusters; bit++, done++)
                        used += (buf[i] >> bit) & 1;
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
    for (u64 n = 0; n < nblocks && !c.stop; n++)
    {
        u8 bits;
        int r = stream_read(nt, &S_AUX, n / 8, &bits, 1);
        if (r <= 0)
            return -1;
        if (!((bits >> (n % 8)) & 1))
            continue; /* this block is free: it may still hold old entries */
        if (stream_read(nt, &S_MAIN, n * block, IDXBUF, block) != (int)block)
            return -1;
        if (IDXBUF[0] != 'I' || IDXBUF[1] != 'N' || IDXBUF[2] != 'D' || IDXBUF[3] != 'X' || apply_fixups(IDXBUF, block))
            return -1;
        u32 eo = rd32(IDXBUF + 24), et = rd32(IDXBUF + 28);
        if (24 + et > block)
            return -1;
        walk_entries(IDXBUF, 24 + eo, 24 + et, &c);
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
