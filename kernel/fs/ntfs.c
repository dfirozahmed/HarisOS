/* NTFS file system. See include/ntfs.h. The first half reads; the second half (after ntfs_read) writes.
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
    /* the attributes must lie inside the bytes in use (and those inside the record): the writing code moves bytes by
     * these numbers, so a damaged record must not get that far */
    u32 used = rd32(buf + 24), o = rd16(buf + 20);
    if (used > nt->rec_size || used > rd32(buf + 28) || rd32(buf + 28) > nt->rec_size || o < 0x18 || o > used)
        return -1;
    for (;;)
    {
        if (o + 4 > used)
            return -1;
        if (rd32(buf + o) == AT_END)
            return 0;
        u32 len = o + 16 <= used ? rd32(buf + o + 4) : 0;
        if (len < 16 || (len & 7) || o + len > used)
            return -1;
        o += len;
    }
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

/* ====================================================================== */
/* ======================== writing ===================================== */
/* ====================================================================== */
/* How a change is made (one call = one change, the code is not re-entered):
 *   1. wr_begin: refuse unless the disk may be written and the volume is clean (dirty bit off, $LogFile clean); turn
 *      the $Volume dirty bit ON.
 *   2. bitmaps first (clusters in $Bitmap, records in the $MFT bitmap), then the MFT record, then the folder index
 *      and last the folder's own record. A crash in between leaves leaked or orphaned things that chkdsk repairs,
 *      never an index entry that points to nothing.
 *   3. wr_end: flush, turn the dirty bit OFF, flush.
 * Records and index blocks are edited in RAM in their "real" form (fixups undone) and sealed again when written. */

static NtfsClockFn CLOCK;
void ntfs_set_clock(NtfsClockFn fn) { CLOCK = fn; }
static u64 now_ft(void) { return CLOCK ? CLOCK() : 0x01DC000000000000ULL; }

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
static u32 al8(u32 v) { return (v + 7) & ~7u; }

/* like memmove */
static void move_bytes(void *dst, const void *src, u32 n)
{
    volatile u8 *d = (volatile u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)src;
    if (d < s)
        for (u32 i = 0; i < n; i++)
            d[i] = s[i];
    else
        for (u32 i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
}

static u32 slen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

static NtStream S_BM, S_MB, S_UP, S_LOG; /* $Bitmap, the $MFT bitmap, $UpCase, $LogFile */
static u8 FREC[4096], DREC[4096], MREC[4096], RSEAL[4096];
static u8 MPBUF[NT_MAX_RUNS * 17 + 8]; /* mapping pairs while they are being made */

/* ---- raw writes ---------------------------------------------------- */
static int vol_write(Ntfs *nt, u64 off, const u8 *buf, u32 len)
{
    while (len)
    {
        u64 lba = off / nt->ss;
        u32 head = (u32)(off % nt->ss);
        if (head == 0 && len >= nt->ss)
        {
            u32 whole = len / nt->ss;
            if (blk_write(nt->blk, lba, whole, buf) != 0)
            {
                nt->poisoned = 1;
                return -1;
            }
            if (nt->sec_lba != NONE && nt->sec_lba >= lba && nt->sec_lba < lba + whole)
                nt->sec_lba = NONE;
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
                nt->poisoned = 1;
                return -1;
            }
            nt->sec_lba = lba;
        }
        u32 take = nt->ss - head < len ? nt->ss - head : len;
        copy(nt->sec + head, buf, take);
        if (blk_write(nt->blk, lba, 1, nt->sec) != 0)
        {
            nt->sec_lba = NONE;
            nt->poisoned = 1;
            return -1;
        }
        buf += take;
        off += take;
        len -= take;
    }
    return 0;
}

/* the same walk as runs_read, writing. A hole cannot be written. */
static int runs_write(Ntfs *nt, const NtRuns *rs, u64 offset, const u8 *buf, u32 len)
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
        if (!run || run->lcn < 0)
            return -1;
        u64 left_in_run = (run->vcn + run->len - vcn) * nt->csize - in;
        u32 n = left_in_run < len ? (u32)left_in_run : len;
        if (vol_write(nt, ((u64)run->lcn + (vcn - run->vcn)) * nt->csize + in, buf, n))
            return -1;
        buf += n;
        offset += n;
        len -= n;
    }
    return 0;
}

/* `n` zero bytes at byte `offset` of the stream */
static int runs_zero(Ntfs *nt, const NtRuns *rs, u64 offset, u64 n)
{
    static const u8 Z[512];
    while (n)
    {
        u32 take = n < sizeof Z ? (u32)n : (u32)sizeof Z;
        if (runs_write(nt, rs, offset, Z, take))
            return -1;
        offset += take;
        n -= take;
    }
    return 0;
}

/* ---- records ------------------------------------------------------- */
/* Write a record that is in real form: seal it (new update sequence number, the real last two bytes of each sector
 * into the array), write it, and write the copy in $MFTMirr for the first records. `rec` stays in real form. */
static int rec_store(Ntfs *nt, u32 recno, u8 *rec)
{
    u32 rs = nt->rec_size, usa = rd16(rec + 4), n = rd16(rec + 6);
    if (n < 2 || usa + n * 2 > rs || (n - 1) * 512 != rs)
        return -1;
    u32 usn = (rd16(rec + usa) + 1u) & 0xFFFF;
    if (usn == 0 || usn == 0xFFFF)
        usn = 1;
    copy(RSEAL, rec, rs);
    wr16(RSEAL + usa, usn);
    for (u32 i = 1; i < n; i++)
    {
        u8 *end = RSEAL + i * 512 - 2;
        RSEAL[usa + 2 * i] = end[0];
        RSEAL[usa + 2 * i + 1] = end[1];
        wr16(end, usn);
    }
    wr16(rec + usa, usn);
    /* the copy first: if the power fails between the two writes, the real record is still the old one (for the
     * $Volume dirty bit that means it stays dirty, never "clean" with a stale copy) */
    if (recno < nt->mirr_recs && vol_write(nt, nt->mirr_lcn * nt->csize + (u64)recno * rs, RSEAL, rs))
        return -1;
    if (runs_write(nt, &nt->mft, (u64)recno * rs, RSEAL, rs))
    {
        nt->poisoned = 1;
        return -1;
    }
    return 0;
}

/* the sequence number of a record slot that may be unused (0 = never used) */
static u32 rec_slot_seq(Ntfs *nt, u32 recno)
{
    if (runs_read(nt, &nt->mft, (u64)recno * nt->rec_size, RSEAL, nt->rec_size))
        return 0;
    if (RSEAL[0] != 'F' || RSEAL[1] != 'I' || RSEAL[2] != 'L' || RSEAL[3] != 'E')
        return 0;
    return rd16(RSEAL + 16);
}

static u8 *rec_find(u8 *rec, u32 rs, u32 type, const char *name)
{
    for (const u8 *a = attr_first(rec, rs); a && rd32(a) != AT_END; a = attr_next(rec, rs, a))
        if (attr_is(a, type, name))
            return (u8 *)a;
    return 0;
}

/* give the attribute `a` of rec a new length (a multiple of 8): the attributes after it move */
static int rec_resize(u8 *rec, u32 rs, u8 *a, u32 new_len)
{
    (void)rs;
    u32 old = rd32(a + 4), used = rd32(rec + 24), at = (u32)(a - rec);
    if (new_len == old)
        return 0;
    if (new_len > old && used + (new_len - old) > rd32(rec + 28))
        return -1;
    move_bytes(a + new_len, a + old, used - at - old);
    wr32(a + 4, new_len);
    wr32(rec + 24, used - old + new_len);
    return 0;
}

/* put a complete attribute in its place (attributes are ordered by type); gives it a new instance number */
static u8 *rec_insert(u8 *rec, u32 rs, const u8 *attr)
{
    u32 type = rd32(attr), len = rd32(attr + 4), used = rd32(rec + 24);
    if (used + len > rd32(rec + 28) || used + len > rs)
        return 0;
    u8 *a = rec + rd16(rec + 20);
    while (a + 8 <= rec + used && rd32(a) != AT_END && rd32(a) <= type)
    {
        u32 l = rd32(a + 4);
        if (l < 16 || (l & 7))
            return 0;
        a += l;
    }
    u32 at = (u32)(a - rec);
    move_bytes(a + len, a, used - at);
    copy(a, attr, len);
    u32 id = rd16(rec + 40);
    wr16(a + 14, id);
    wr16(rec + 40, id + 1);
    wr32(rec + 24, used + len);
    return a;
}

static void rec_remove(u8 *rec, u8 *a)
{
    u32 len = rd32(a + 4), used = rd32(rec + 24), at = (u32)(a - rec);
    move_bytes(a, a + len, used - at - len);
    wr32(rec + 24, used - len);
}

/* ---- attributes ----------------------------------------------------- */
static u32 name_to_attr(u8 *a, u32 off, const char *name)
{
    u32 nl = name ? slen(name) : 0;
    for (u32 i = 0; i < nl; i++)
        wr16(a + off + 2 * i, (u8)name[i]);
    return nl;
}

/* a resident attribute: header, name, value. Returns its length. */
static u32 build_res(u8 *out, u32 type, const char *name, const u8 *val, u32 vlen, u32 res_flags)
{
    u32 nl = name ? slen(name) : 0, voff = al8(0x18 + nl * 2), total = al8(voff + vlen);
    zero(out, total);
    wr32(out, type);
    wr32(out + 4, total);
    out[9] = (u8)nl;
    wr16(out + 10, nl ? 0x18 : 0);
    wr32(out + 16, vlen);
    wr16(out + 20, voff);
    out[22] = (u8)res_flags;
    name_to_attr(out, 0x18, name);
    if (vlen)
        copy(out + voff, val, vlen);
    return total;
}

/* change the value of a resident attribute (it may change size). */
static int res_set(u8 *rec, u32 rs, u8 *a, const u8 *val, u32 vlen)
{
    u32 voff = rd16(a + 20);
    if (rec_resize(rec, rs, a, al8(voff + vlen)))
        return -1;
    zero(a + voff, rd32(a + 4) - voff);
    wr32(a + 16, vlen);
    if (vlen)
        copy(a + voff, val, vlen);
    return 0;
}

/* a run list as mapping pairs. Returns the length including the final 0, or 0 if it does not fit. */
static u32 encode_runs(const NtRuns *rs, u8 *out, u32 cap)
{
    u32 o = 0;
    s64 prev = 0;
    for (int i = 0; i < rs->n; i++)
    {
        u64 len = rs->r[i].len;
        u32 nl = 1, no = 0;
        while (nl < 8 && (len >> (8 * nl)))
            nl++;
        s64 delta = 0;
        if (rs->r[i].lcn >= 0)
        {
            delta = rs->r[i].lcn - prev;
            prev = rs->r[i].lcn;
            no = 1;
            while (no < 8)
            {
                s64 lo = -((s64)1 << (8 * no - 1)), hi = ((s64)1 << (8 * no - 1)) - 1;
                if (delta >= lo && delta <= hi)
                    break;
                no++;
            }
        }
        if (o + 1 + nl + no + 1 > cap)
            return 0;
        out[o++] = (u8)(nl | (no << 4));
        for (u32 k = 0; k < nl; k++)
            out[o++] = (u8)(len >> (8 * k));
        for (u32 k = 0; k < no; k++)
            out[o++] = (u8)((u64)delta >> (8 * k));
    }
    out[o++] = 0;
    return o;
}

static u64 runs_clusters(const NtRuns *rs)
{
    u64 n = 0;
    for (int i = 0; i < rs->n; i++)
        n += rs->r[i].len;
    return n;
}

/* a non-resident attribute with all its runs in this record. Returns its length, 0 if it does not fit in cap. */
static u32 build_nonres(u8 *out, u32 cap, u32 type, const char *name, const NtRuns *rs, u64 alloc, u64 real, u64 init)
{
    u32 nl = name ? slen(name) : 0, runs_off = al8(0x40 + nl * 2);
    if (runs_off >= cap)
        return 0;
    u32 mp = encode_runs(rs, MPBUF, sizeof MPBUF);
    u32 total = al8(runs_off + mp);
    if (!mp || total > cap)
        return 0;
    zero(out, total);
    wr32(out, type);
    wr32(out + 4, total);
    out[8] = 1;
    out[9] = (u8)nl;
    wr16(out + 10, nl ? 0x40 : 0);
    u64 cl = runs_clusters(rs);
    wr64(out + 24, cl ? cl - 1 : ~0ULL);
    wr16(out + 32, runs_off);
    wr64(out + 40, alloc);
    wr64(out + 48, real);
    wr64(out + 56, init);
    name_to_attr(out, 0x40, name);
    copy(out + runs_off, MPBUF, mp);
    return total;
}

/* new runs and sizes for an existing non-resident attribute of rec (it may grow or shrink in the record) */
static int nonres_set(u8 *rec, u32 rs, u8 *a, const NtRuns *runs, u64 alloc, u64 real, u64 init)
{
    u32 runs_off = rd16(a + 32);
    u32 mp = encode_runs(runs, MPBUF, sizeof MPBUF);
    if (!mp)
        return -1;
    if (rec_resize(rec, rs, a, al8(runs_off + mp)))
        return -1;
    zero(a + runs_off, rd32(a + 4) - runs_off);
    copy(a + runs_off, MPBUF, mp);
    u64 cl = runs_clusters(runs);
    wr64(a + 16, 0);
    wr64(a + 24, cl ? cl - 1 : ~0ULL);
    wr64(a + 40, alloc);
    wr64(a + 48, real);
    wr64(a + 56, init);
    return 0;
}

/* append `len` clusters at lcn to the end of a run list (joining the last run when they touch) */
static int runs_append(NtRuns *rs, u64 lcn, u64 len)
{
    if (rs->n && rs->r[rs->n - 1].lcn >= 0 && (u64)rs->r[rs->n - 1].lcn + rs->r[rs->n - 1].len == lcn)
    {
        rs->r[rs->n - 1].len += len;
        return 0;
    }
    if (rs->n >= NT_MAX_RUNS)
        return -1;
    NtRun *r = &rs->r[rs->n];
    r->vcn = rs->n ? rs->r[rs->n - 1].vcn + rs->r[rs->n - 1].len : 0;
    r->len = len;
    r->lcn = (s64)lcn;
    rs->n++;
    return 0;
}

/* ---- the cluster bitmap ----------------------------------------------- */
static u8 BW[4096];
static u64 BW_BASE = NONE;

static int bm_load(Ntfs *nt)
{
    BW_BASE = NONE;
    if (load_stream(nt, 6, AT_DATA, 0, &S_BM) != 0 || !S_BM.found || S_BM.resident || S_BM.size * 8 < nt->clusters)
        return -1;
    return 0;
}

/* bit of cluster c: 0, 1 or -1 */
static int bm_get(Ntfs *nt, u64 c)
{
    u64 byte = c / 8, base = byte & ~4095ULL;
    if (BW_BASE != base)
    {
        u64 n = S_BM.size - base;
        if (n > sizeof BW)
            n = sizeof BW;
        if (runs_read(nt, &S_BM.runs, base, BW, (u32)n))
            return -1;
        BW_BASE = base;
    }
    return (BW[byte - base] >> (c & 7)) & 1;
}

/* set clusters [c, c + n) to val */
static int bm_range(Ntfs *nt, u64 c, u64 n, int val)
{
    BW_BASE = NONE;
    u8 buf[512];
    while (n)
    {
        u64 first = c / 8, last = (c + n - 1) / 8;
        u32 nb = (u32)(last - first + 1 > sizeof buf ? sizeof buf : last - first + 1);
        if (runs_read(nt, &S_BM.runs, first, buf, nb))
            return -1;
        while (n && c / 8 < first + nb)
        {
            if (val)
                buf[c / 8 - first] |= (u8)(1u << (c & 7));
            else
                buf[c / 8 - first] &= (u8)~(1u << (c & 7));
            c++;
            n--;
        }
        if (runs_write(nt, &S_BM.runs, first, buf, nb))
        {
            nt->poisoned = 1;
            return -1;
        }
    }
    return 0;
}

/* Take `want` clusters, in as few runs as possible, starting the search at `hint`. The runs are appended to out
 * (joined to its last run when they touch it) and marked used. On failure nothing is taken: NTFS_ERR_FULL. */
static int alloc_clusters(Ntfs *nt, u64 want, u64 hint, NtRuns *out)
{
    static NtRuns got;
    got.n = 0;
    u64 need = want, c = hint < nt->clusters ? hint : 0, passed = 0;
    while (need && passed < nt->clusters)
    {
        if (c >= nt->clusters)
            c = 0;
        int bit = bm_get(nt, c);
        if (bit < 0)
            return NTFS_ERR_IO;
        if (bit)
        {
            c++;
            passed++;
            continue;
        }
        u64 s = c, len = 0;
        while (len < need && c < nt->clusters && passed < nt->clusters)
        {
            bit = bm_get(nt, c);
            if (bit < 0)
                return NTFS_ERR_IO;
            if (bit)
                break;
            c++;
            len++;
            passed++;
        }
        if (got.n >= NT_MAX_RUNS)
            return NTFS_ERR_FULL;
        got.r[got.n].vcn = 0;
        got.r[got.n].len = len;
        got.r[got.n].lcn = (s64)s;
        got.n++;
        need -= len;
    }
    if (need)
        return NTFS_ERR_FULL;
    if (out->n + got.n > NT_MAX_RUNS) /* would not fit in the list: nothing is marked yet, so nothing to undo */
        return NTFS_ERR_FULL;
    for (int i = 0; i < got.n; i++)
        runs_append(out, (u64)got.r[i].lcn, got.r[i].len);
    for (int i = 0; i < got.n; i++)
        if (bm_range(nt, (u64)got.r[i].lcn, got.r[i].len, 1))
            return NTFS_ERR_IO;
    nt->hint = (u64)got.r[got.n - 1].lcn + got.r[got.n - 1].len;
    if (nt->free_clusters != NONE)
        nt->free_clusters -= want;
    return 0;
}

/* give the clusters of runs [from, n) back */
static int free_runs(Ntfs *nt, const NtRuns *rs, int from)
{
    for (int i = from; i < rs->n; i++)
        if (rs->r[i].lcn >= 0)
        {
            if (bm_range(nt, (u64)rs->r[i].lcn, rs->r[i].len, 0))
                return -1;
            if (nt->free_clusters != NONE)
                nt->free_clusters += rs->r[i].len;
        }
    return 0;
}

/* ---- the MFT bitmap ------------------------------------------------------ */
static int mb_load(Ntfs *nt)
{
    if (load_stream(nt, 0, AT_BITMAP, 0, &S_MB) != 0 || !S_MB.found)
        return -1;
    return 0;
}

static int mb_get(Ntfs *nt, u32 recno)
{
    u8 b;
    if ((u64)recno / 8 >= S_MB.size || stream_read(nt, &S_MB, recno / 8, &b, 1) != 1)
        return -1;
    return (b >> (recno & 7)) & 1;
}

static int mb_set(Ntfs *nt, u32 recno, int val)
{
    u8 b;
    if ((u64)recno / 8 >= S_MB.size || stream_read(nt, &S_MB, recno / 8, &b, 1) != 1)
        return -1;
    if (val)
        b |= (u8)(1u << (recno & 7));
    else
        b &= (u8)~(1u << (recno & 7));
    if (!S_MB.resident)
    {
        if (runs_write(nt, &S_MB.runs, recno / 8, &b, 1))
        {
            nt->poisoned = 1;
            return -1;
        }
        return 0;
    }
    /* resident in record 0: patch it there */
    if (record_read(nt, 0, MREC))
        return -1;
    u8 *a = rec_find(MREC, nt->rec_size, AT_BITMAP, 0);
    if (!a || a[8] != 0 || recno / 8 >= rd32(a + 16))
        return -1;
    a[rd16(a + 20) + recno / 8] = b;
    S_MB.res[recno / 8] = b;
    return rec_store(nt, 0, MREC);
}

/* the first free record from 24 up (0..23 are the system's) */
static int mb_find_free(Ntfs *nt, u32 *recno)
{
    u64 total = nt->mft_size / nt->rec_size;
    if (total > S_MB.size * 8)
        total = S_MB.size * 8;
    for (u64 r = 24; r < total; r++)
    {
        int bit = mb_get(nt, (u32)r);
        if (bit < 0)
            return NTFS_ERR_IO;
        if (!bit)
        {
            *recno = (u32)r;
            return 0;
        }
    }
    return NTFS_ERR_FULL;
}

/* ---- names and collation -------------------------------------------------- */
static u32 up_ch(Ntfs *nt, u32 ch)
{
    if (ch < 0x80)
        return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
    if (S_UP.found && !S_UP.resident && (u64)ch * 2 + 2 <= S_UP.size)
    {
        u8 b[2];
        if (stream_read(nt, &S_UP, (u64)ch * 2, b, 2) == 2)
            return rd16(b);
    }
    return ch;
}

/* order of two $FILE_NAME keys: the names compared letter by letter in upper case (the $UpCase table) */
static int key_cmp(Ntfs *nt, const u8 *a, const u8 *b)
{
    u32 la = a[64], lb = b[64], n = la < lb ? la : lb;
    for (u32 i = 0; i < n; i++)
    {
        u32 x = up_ch(nt, rd16(a + 66 + 2 * i)), y = up_ch(nt, rd16(b + 66 + 2 * i));
        if (x != y)
            return x < y ? -1 : 1;
    }
    return la == lb ? 0 : la < lb ? -1 : 1;
}

/* is `name` a name we create? 0 = yes, else NTFS_ERR_BADNAME */
static int name_ok(const char *name)
{
    if (!name)
        return NTFS_ERR_BADNAME;
    u32 n = slen(name);
    if (n == 0 || n > 255 || name[n - 1] == '.' || name[n - 1] == ' ')
        return NTFS_ERR_BADNAME;
    for (u32 i = 0; i < n; i++)
    {
        u8 c = (u8)name[i];
        if (c < 0x20 || c >= 0x7F || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' ||
            c == '|')
            return NTFS_ERR_BADNAME;
    }
    return 0;
}

/* ---- timestamps and the $FILE_NAME key ------------------------------------- */
/* Fill a $FILE_NAME (the key of a folder index entry, the same bytes as the attribute): parent, times, sizes,
 * flags, name. Returns its length. */
static u32 make_key(u8 *key, u64 parent_ref, const char *name, u64 t, u64 alloc, u64 real, u32 flags, u32 ns)
{
    u32 nl = slen(name);
    zero(key, 66 + nl * 2);
    wr64(key, parent_ref);
    for (int i = 0; i < 4; i++)
        wr64(key + 8 + 8 * i, t);
    wr64(key + 40, alloc);
    wr64(key + 48, real);
    wr32(key + 56, flags);
    key[64] = (u8)nl;
    key[65] = (u8)ns;
    for (u32 i = 0; i < nl; i++)
        wr16(key + 66 + 2 * i, (u8)name[i]);
    return 66 + nl * 2;
}

/* set the modified / MFT-changed / accessed times in $STANDARD_INFORMATION */
static void touch_si(u8 *rec, u32 rs, u64 t)
{
    u8 *si = rec_find(rec, rs, AT_STANDARD_INFORMATION, 0);
    if (si && si[8] == 0 && rd32(si + 16) >= 32)
    {
        u8 *v = si + rd16(si + 20);
        wr64(v + 8, t);
        wr64(v + 16, t);
        wr64(v + 24, t);
    }
}

/* ---- begin / end of a change ------------------------------------------------- */
/* $Volume's flags (the dirty bit and friends) */
static int vol_flags(Ntfs *nt, u32 set, u32 clear, u32 *old)
{
    if (record_read(nt, 3, MREC))
        return -1;
    u8 *a = rec_find(MREC, nt->rec_size, 0x70, 0);
    if (!a || a[8] != 0 || rd32(a + 16) < 12 || rd16(a + 20) + 12u > rd32(a + 4))
        return -1;
    u8 *f = a + rd16(a + 20) + 10;
    u32 cur = rd16(f);
    if (old)
        *old = cur;
    u32 now = (cur | set) & ~clear;
    if (now == cur)
        return 0;
    wr16(f, now);
    return rec_store(nt, 3, MREC);
}

#define VOL_DIRTY 0x0001u
#define VOL_RESIZE_LOG 0x0002u
#define VOL_UPGRADE 0x0004u
#define VOL_DELETE_USN 0x0010u

/* One restart page of $LogFile at byte `pos`: 1 if it is a good one (then *lsn is its current LSN, *clean says if it
 * has the "volume is clean" flag), else 0 (all 0xFF = empty, or damaged). */
static int log_page(Ntfs *nt, u64 pos, u64 *lsn, int *clean)
{
    u8 p[0x20], r[16];
    if (stream_read(nt, &S_LOG, pos, p, sizeof p) != (int)sizeof p)
        return 0;
    if (!(p[0] == 'R' && p[1] == 'S' && p[2] == 'T' && p[3] == 'R'))
        return 0;
    u32 ra = rd16(p + 0x18);
    if (ra < 0x20 || stream_read(nt, &S_LOG, pos + ra, r, sizeof r) != (int)sizeof r)
        return 0;
    *lsn = rd64(r);
    *clean = (rd16(r + 14) & 0x0002) != 0; /* RESTART_VOLUME_IS_CLEAN */
    return 1;
}

/* Is the journal clean? Windows writes the restart area alternately to two pages, so the one with the higher LSN is
 * the current one; the other may be older and still say "not clean". An empty log (all 0xFF) is clean. */
static int log_clean(Ntfs *nt)
{
    if (load_stream(nt, 2, AT_DATA, 0, &S_LOG) != 0 || !S_LOG.found)
        return 0;
    if (S_LOG.size == 0)
        return 1; /* nothing in it (tests, a fresh volume) */
    if (S_LOG.flags & (FL_COMPRESSED | FL_ENCRYPTED))
        return 0;
    u8 h[24];
    if (stream_read(nt, &S_LOG, 0, h, sizeof h) != (int)sizeof h)
        return 0;
    u32 sps = (h[0] == 'R' && h[1] == 'S' && h[2] == 'T' && h[3] == 'R') ? rd32(h + 16) : 4096; /* system page size */
    if (sps < 512 || sps > 65536 || S_LOG.size < 2ULL * sps)
        return 0;
    u64 lsn0 = 0, lsn1 = 0;
    int c0 = 0, c1 = 0;
    int v0 = log_page(nt, 0, &lsn0, &c0), v1 = log_page(nt, sps, &lsn1, &c1);
    if (v0 && v1)
        return lsn1 > lsn0 ? c1 : c0;
    if (v0 || v1)
        return v0 ? c0 : c1;
    u8 a[4], b[4]; /* no restart page at all: clean only if the log was reset (all 0xFF) */
    return stream_read(nt, &S_LOG, 0, a, 4) == 4 && stream_read(nt, &S_LOG, sps, b, 4) == 4 && rd32(a) == 0xFFFFFFFFu &&
           rd32(b) == 0xFFFFFFFFu;
}

static int wr_begin(Ntfs *nt)
{
    BlkDev *d = blk_get(nt->blk);
    if (!d || d->locked) /* (a partition mirrors the lock of its disk) */
        return NTFS_ERR_RO;
    if (nt->wstate < 0)
        return nt->wstate;
    S_FILE_NT = 0; /* what was cached for reading may change now */
    if (nt->wstate == 0)
    {
        u32 fl;
        if (vol_flags(nt, 0, 0, &fl))
            return nt->wstate = NTFS_ERR_UNSUPPORTED;
        if (fl & (VOL_DIRTY | VOL_RESIZE_LOG | VOL_UPGRADE | VOL_DELETE_USN))
            return nt->wstate = NTFS_ERR_DIRTY;
        if (!log_clean(nt))
            return nt->wstate = NTFS_ERR_DIRTY;
        nt->wstate = 1;
    }
    if (bm_load(nt) || mb_load(nt))
        return NTFS_ERR_UNSUPPORTED;
    S_UP.found = 0;
    load_stream(nt, 10, AT_DATA, 0, &S_UP); /* $UpCase; without it only ASCII letters fold */
    if (vol_flags(nt, VOL_DIRTY, 0, 0))
    {
        nt->poisoned = 0; /* the very first write failed: nothing changed (a read-only device) */
        return NTFS_ERR_RO;
    }
    return 0;
}

/* finish: r is the result of the change. The dirty bit goes off only if every write worked. */
static int wr_end(Ntfs *nt, int r)
{
    S_FILE_NT = 0;
    if (nt->poisoned)
    {
        nt->wstate = NTFS_ERR_DIRTY; /* leave the dirty bit on: chkdsk has to look at this */
        blk_flush(nt->blk);
        return r >= 0 ? NTFS_ERR_IO : r;
    }
    if (blk_flush(nt->blk) != 0 || vol_flags(nt, 0, VOL_DIRTY, 0) || blk_flush(nt->blk) != 0)
    {
        nt->wstate = NTFS_ERR_DIRTY;
        return r >= 0 ? NTFS_ERR_IO : r;
    }
    return r;
}

/* ---- folder indexes: the B-tree ------------------------------------------------ */
/* A folder's index is a B-tree of $FILE_NAME entries. Its root lives inside the folder's MFT record ($INDEX_ROOT),
 * the other nodes are 4 KiB blocks ($INDEX_ALLOCATION, used ones marked in the index $BITMAP). A node is a list
 * of entries ending with an "end" entry. In a node that is not a leaf every entry (also the end entry) points to a
 * child node that holds the names smaller than that entry's name. The rules this code keeps:
 *   - the entries of a node are sorted (collation: upper case via $UpCase) and a name appears once in the tree;
 *   - every block that is reachable is marked used in the bitmap, every marked block is reachable;
 *   - all leaves are at the same depth;
 *   - a block never holds zero entries: such a block is freed (its parent's separator moves down into a sibling).
 * The folder's record is edited in DREC and written by idx_close(). */
#define ROOT_BLK (~0ULL)
#define MAXD 12

typedef struct
{
    u32 rec;
    int dirty;           /* DREC has to be written */
    int has_ia;
    NtRuns ia;           /* $INDEX_ALLOCATION runs */
    u64 ia_size;
    int ib_res;          /* the index $BITMAP is resident (in DREC) */
    NtRuns ib;
    u64 ib_size;
} Idx;
static Idx IX;
static int IDX_WROTE; /* something of the index reached the disk (a block, or the folder's record) during this change */

typedef struct
{
    u8 *buf;
    u32 hdr; /* offset of the index header in buf: 16 in the root, 24 in a block */
    int root;
} Node;

typedef struct
{
    u64 blk; /* ROOT_BLK or the block number */
    u32 pos; /* bytes from the first entry to the entry we went through / insert before */
} Lvl;
static Lvl PATH[MAXD + 2];
static int PDEPTH;

static u8 NB1[16384], NB2[16384], ISEAL[16384];
static u8 SPL[16384 + 1400];
static u8 DSNAP[4096];
static u8 SENT[640], DEMOTE[640], EWORK[640];
static NtRuns CAND, NEWR;

static u32 e_len(const u8 *e) { return rd16(e + 8); }
static int e_end(const u8 *e) { return (rd16(e + 12) & 2) != 0; }
static int e_sub(const u8 *e) { return (rd16(e + 12) & 1) != 0; }
static u64 e_ptr(const u8 *e) { return rd64(e + e_len(e) - 8); }
static void e_setptr(u8 *e, u64 v) { wr64(e + e_len(e) - 8, v); }

static u32 nd_ent0(const Node *n) { return n->hdr + rd32(n->buf + n->hdr); }
static u32 nd_end(const Node *n) { return n->hdr + rd32(n->buf + n->hdr + 4); }
static void nd_set_end(Node *n, u32 end) { wr32(n->buf + n->hdr + 4, end - n->hdr); }
static int nd_leaf(const Node *n) { return !(n->buf[n->hdr + 12] & 1); }
static int nd_empty(const Node *n) { return e_end(n->buf + nd_ent0(n)); }

static u32 vcn_unit(const Ntfs *nt) { return nt->csize <= nt->idx_size ? nt->csize : 512; }
static u64 blk_vcn(const Ntfs *nt, u64 n) { return n * nt->idx_size / vcn_unit(nt); }
static u64 vcn_blk(const Ntfs *nt, u64 v) { return v * vcn_unit(nt) / nt->idx_size; }

static int nd_check(const Node *n, u32 limit)
{
    u32 e0 = nd_ent0(n), end = nd_end(n);
    if (e0 < n->hdr + 16 || e0 > end || end > limit)
        return -1;
    int leaf = nd_leaf(n);
    for (u32 p = e0;;)
    {
        if (p + 16 > end)
            return -1;
        const u8 *e = n->buf + p;
        u32 len = e_len(e);
        if (len < 16 || len > 640 /* the biggest entry we make is 600; the buffers hold 640 */ || (len & 7) || p + len > end ||
            ((!leaf) != (e_sub(e) != 0)) || (e_sub(e) && len < 24))
            return -1;
        if (e_end(e))
            return p + len == end ? 0 : -1;
        u32 kl = rd16(e + 10);
        if (kl < 66 || 16 + kl + (e_sub(e) ? 8u : 0u) > len)
            return -1;
        p += len;
    }
}

/* ---- open / close a folder ------------------------------------------------ */
static int idx_open(Ntfs *nt, u32 recno)
{
    u32 rs = nt->rec_size;
    if (record_read(nt, recno, DREC))
        return NTFS_ERR_IO;
    if (!(rd16(DREC + 22) & 2))
        return NTFS_ERR_ARG;
    if (rec_find(DREC, rs, AT_ATTRIBUTE_LIST, 0))
        return NTFS_ERR_UNSUPPORTED;
    u8 *root = rec_find(DREC, rs, AT_INDEX_ROOT, "$I30");
    if (!root || root[8] != 0 || rd32(root + 16) < 32 || rd16(root + 20) + rd32(root + 16) > rd32(root + 4))
        return NTFS_ERR_IO;
    const u8 *v = root + rd16(root + 20);
    if (rd32(v) != AT_FILE_NAME || rd32(v + 4) != 1 || rd32(v + 8) != nt->idx_size)
        return NTFS_ERR_UNSUPPORTED;
    IX.rec = recno;
    IX.dirty = 0;
    IX.has_ia = 0;
    IX.ia.n = 0;
    IX.ia_size = 0;
    IX.ib_res = 1;
    IX.ib.n = 0;
    IX.ib_size = 0;
    u8 *ia = rec_find(DREC, rs, AT_INDEX_ALLOCATION, "$I30");
    u8 *ib = rec_find(DREC, rs, AT_BITMAP, "$I30");
    if (ia)
    {
        if (ia[8] == 0 || rd16(ia + 12) || parse_runs(ia + rd16(ia + 32), ia + rd32(ia + 4), rd64(ia + 16), &IX.ia))
            return NTFS_ERR_UNSUPPORTED;
        IX.ia_size = rd64(ia + 48);
        IX.has_ia = 1;
        if (IX.ia_size % nt->idx_size || IX.ia_size > runs_clusters(&IX.ia) * nt->csize)
            return NTFS_ERR_IO;
        if (!ib)
            return NTFS_ERR_IO;
    }
    if (ib)
    {
        if (ib[8] == 0)
            IX.ib_res = 1;
        else
        {
            IX.ib_res = 0;
            if (rd16(ib + 12) || parse_runs(ib + rd16(ib + 32), ib + rd32(ib + 4), rd64(ib + 16), &IX.ib))
                return NTFS_ERR_UNSUPPORTED;
            IX.ib_size = rd64(ib + 48);
        }
    }
    if (!IX.has_ia && (v[28] & 1))
        return NTFS_ERR_IO;
    return 0;
}

/* write the folder's record if the index changed it */
static int idx_close(Ntfs *nt, int ok, u64 touch)
{
    if (!ok)
        return 0;
    if (touch) /* the folder's times change with its names, also when only a block of its index did */
    {
        touch_si(DREC, nt->rec_size, touch);
        IX.dirty = 1;
    }
    if (!IX.dirty)
        return 0;
    IX.dirty = 0;
    IDX_WROTE = 1;
    return rec_store(nt, IX.rec, DREC) ? NTFS_ERR_IO : 0;
}

/* ---- the index bitmap ----------------------------------------------------- */
static int ib_get(Ntfs *nt, u64 n)
{
    if (IX.ib_res)
    {
        u8 *b = rec_find(DREC, nt->rec_size, AT_BITMAP, "$I30");
        if (!b || n / 8 >= rd32(b + 16))
            return 0;
        return (b[rd16(b + 20) + n / 8] >> (n & 7)) & 1;
    }
    u8 x;
    if (n / 8 >= IX.ib_size)
        return 0;
    if (runs_read(nt, &IX.ib, n / 8, &x, 1))
        return -1;
    return (x >> (n & 7)) & 1;
}

static int ib_set(Ntfs *nt, u64 n, int val)
{
    if (IX.ib_res)
    {
        u8 *b = rec_find(DREC, nt->rec_size, AT_BITMAP, "$I30");
        if (!b || n / 8 >= rd32(b + 16))
            return -1;
        u8 *p = b + rd16(b + 20) + n / 8;
        if (val)
            *p |= (u8)(1u << (n & 7));
        else
            *p &= (u8)~(1u << (n & 7));
        IX.dirty = 1;
        return 0;
    }
    u8 x;
    if (n / 8 >= IX.ib_size || runs_read(nt, &IX.ib, n / 8, &x, 1))
        return -1;
    if (val)
        x |= (u8)(1u << (n & 7));
    else
        x &= (u8)~(1u << (n & 7));
    if (runs_write(nt, &IX.ib, n / 8, &x, 1))
    {
        nt->poisoned = 1;
        return -1;
    }
    return 0;
}

/* Take a block of the allocation (growing it by one block when all are used; making the allocation when the folder
 * has none). The record edits are all-or-nothing: on failure DREC is put back and the clusters are given back. */
static int idx_new_block_mem(Ntfs *nt, u64 *out)
{
    u32 rs = nt->rec_size;
    if (nt->csize > nt->idx_size)
        return NTFS_ERR_UNSUPPORTED;
    u64 nblocks = IX.has_ia ? IX.ia_size / nt->idx_size : 0;
    for (u64 n = 0; n < nblocks; n++)
    {
        int b = ib_get(nt, n);
        if (b < 0)
            return NTFS_ERR_IO;
        if (!b)
        {
            if (ib_set(nt, n, 1))
                return NTFS_ERR_IO;
            *out = n;
            return 0;
        }
    }
    u32 ncl = nt->idx_size / nt->csize;
    u64 need_bytes = nblocks / 8 + 1;
    if (!IX.ib_res && need_bytes > IX.ib_size)
        return NTFS_ERR_UNSUPPORTED; /* a non-resident bitmap would have to grow */
    NEWR.n = 0;
    u64 hint = (IX.has_ia && IX.ia.n && IX.ia.r[IX.ia.n - 1].lcn >= 0) ? (u64)IX.ia.r[IX.ia.n - 1].lcn + IX.ia.r[IX.ia.n - 1].len : nt->hint;
    int r = alloc_clusters(nt, ncl, hint, &NEWR);
    if (r)
        return r;
    copy(DSNAP, DREC, rs);
    int ok = 1;
    u64 bytes_after = (nblocks + 1) * nt->idx_size;
    u8 tmp[512];
    CAND.n = 0;
    if (IX.has_ia)
        copy(&CAND, &IX.ia, sizeof CAND);
    for (int i = 0; i < NEWR.n && ok; i++)
        if (runs_append(&CAND, (u64)NEWR.r[i].lcn, NEWR.r[i].len))
            ok = 0;
    if (ok)
    {
        u64 alloc = runs_clusters(&CAND) * nt->csize;
        if (IX.has_ia)
        {
            u8 *a = rec_find(DREC, rs, AT_INDEX_ALLOCATION, "$I30");
            ok = a && nonres_set(DREC, rs, a, &CAND, alloc, bytes_after, bytes_after) == 0;
        }
        else
        {
            u32 l = build_nonres(tmp, sizeof tmp, AT_INDEX_ALLOCATION, "$I30", &CAND, alloc, bytes_after, bytes_after);
            ok = l && rec_insert(DREC, rs, tmp);
            if (ok)
            {
                static const u8 Z8[8];
                l = build_res(tmp, AT_BITMAP, "$I30", Z8, 8, 0);
                ok = rec_insert(DREC, rs, tmp) != 0;
            }
        }
    }
    if (ok && IX.ib_res)
    {
        u8 *b = rec_find(DREC, rs, AT_BITMAP, "$I30");
        if (!b)
            ok = 0;
        else if (need_bytes > rd32(b + 16))
        {
            u8 big[64];
            u32 old = rd32(b + 16), nl = al8((u32)need_bytes);
            if (nl > sizeof big || old > sizeof big)
                ok = 0;
            else
            {
                zero(big, sizeof big);
                copy(big, b + rd16(b + 20), old);
                ok = res_set(DREC, rs, b, big, nl) == 0;
            }
        }
    }
    if (!ok)
    {
        copy(DREC, DSNAP, rs);
        free_runs(nt, &NEWR, 0);
        return NTFS_ERR_FULL;
    }
    copy(&IX.ia, &CAND, sizeof IX.ia);
    IX.ia_size = bytes_after;
    IX.has_ia = 1;
    IX.dirty = 1;
    if (ib_set(nt, nblocks, 1))
        return NTFS_ERR_IO;
    *out = nblocks;
    return 0;
}

/* idx_new_block_mem edits only DREC in RAM. A block that is going to be pointed to by a block already on disk must
 * be allocated on disk first (the allocation size and the bitmap are in the folder's record), or a power cut leaves
 * a pointer to a block that does not exist: persist = write the record now. */
static int idx_new_block(Ntfs *nt, u64 *out, int persist)
{
    int r = idx_new_block_mem(nt, out);
    if (r == 0 && persist && IX.dirty)
    {
        IX.dirty = 0;
        IDX_WROTE = 1;
        if (rec_store(nt, IX.rec, DREC))
            return NTFS_ERR_IO;
    }
    return r;
}

/* ---- loading and storing a node --------------------------------------------- */
static int node_load(Ntfs *nt, Node *n, u8 *buf, u64 blk)
{
    n->buf = buf;
    if (blk == ROOT_BLK)
    {
        u8 *a = rec_find(DREC, nt->rec_size, AT_INDEX_ROOT, "$I30");
        if (!a)
            return NTFS_ERR_IO;
        u32 vlen = rd32(a + 16);
        if (vlen > sizeof NB1 / 2)
            return NTFS_ERR_IO;
        copy(buf, a + rd16(a + 20), vlen);
        n->hdr = 16;
        n->root = 1;
        return nd_check(n, vlen) ? NTFS_ERR_IO : 0;
    }
    u32 bs = nt->idx_size;
    if (!IX.has_ia || (blk + 1) * bs > IX.ia_size)
        return NTFS_ERR_IO;
    if (runs_read(nt, &IX.ia, blk * bs, buf, bs))
        return NTFS_ERR_IO;
    if (buf[0] != 'I' || buf[1] != 'N' || buf[2] != 'D' || buf[3] != 'X' || apply_fixups(buf, bs))
        return NTFS_ERR_IO;
    n->hdr = 24;
    n->root = 0;
    return nd_check(n, bs) ? NTFS_ERR_IO : 0;
}

static int node_store(Ntfs *nt, Node *n, u64 blk)
{
    if (n->root)
    {
        u32 vlen = nd_end(n);
        wr32(n->buf + n->hdr + 8, vlen - n->hdr); /* allocated = used: a resident index */
        u8 *a = rec_find(DREC, nt->rec_size, AT_INDEX_ROOT, "$I30");
        if (!a || res_set(DREC, nt->rec_size, a, n->buf, vlen))
            return NTFS_ERR_FULL;
        IX.dirty = 1;
        return 0;
    }
    u32 bs = nt->idx_size, usa = rd16(n->buf + 4), cnt = rd16(n->buf + 6);
    u32 usn = (rd16(n->buf + usa) + 1u) & 0xFFFF;
    if (usn == 0 || usn == 0xFFFF)
        usn = 1;
    copy(ISEAL, n->buf, bs);
    wr16(ISEAL + usa, usn);
    for (u32 i = 1; i < cnt; i++)
    {
        u8 *end = ISEAL + i * 512 - 2;
        ISEAL[usa + 2 * i] = end[0];
        ISEAL[usa + 2 * i + 1] = end[1];
        wr16(end, usn);
    }
    wr16(n->buf + usa, usn);
    IDX_WROTE = 1;
    if (runs_write(nt, &IX.ia, blk * bs, ISEAL, bs))
    {
        nt->poisoned = 1;
        return NTFS_ERR_IO;
    }
    return 0;
}

/* an empty block image: header, update sequence array, just the end entry */
static void node_init_block(Ntfs *nt, Node *n, u8 *buf, u64 blk, int leaf)
{
    u32 bs = nt->idx_size, cnt = bs / 512 + 1, first = al8(0x28 + 2 * cnt);
    zero(buf, bs);
    buf[0] = 'I';
    buf[1] = 'N';
    buf[2] = 'D';
    buf[3] = 'X';
    wr16(buf + 4, 0x28);
    wr16(buf + 6, cnt);
    wr64(buf + 16, blk_vcn(nt, blk));
    wr32(buf + 24, first - 0x18);
    wr32(buf + 28, first - 0x18 + 16);
    wr32(buf + 32, bs - 0x18);
    buf[36] = leaf ? 0 : 1;
    wr16(buf + 0x28, 1);
    wr16(buf + first + 8, 16);
    wr16(buf + first + 12, 2);
    n->buf = buf;
    n->hdr = 24;
    n->root = 0;
    if (!leaf) /* the end entry of an inner node carries a pointer */
    {
        wr16(buf + first + 8, 24);
        wr16(buf + first + 12, 3);
        wr32(buf + 28, first - 0x18 + 24);
    }
}

static int nd_fits(const Ntfs *nt, const Node *n, u32 add)
{
    u32 end = nd_end(n);
    if (!n->root)
        return end + add <= n->hdr + rd32(n->buf + n->hdr + 8);
    u8 *a = rec_find(DREC, nt->rec_size, AT_INDEX_ROOT, "$I30");
    if (!a)
        return 0;
    u32 newlen = al8(rd16(a + 20) + end + add), oldlen = rd32(a + 4);
    return newlen <= oldlen || rd32(DREC + 24) + (newlen - oldlen) <= rd32(DREC + 28);
}

static void nd_insert(Node *n, u32 at, const u8 *e, u32 len)
{
    u32 end = nd_end(n);
    move_bytes(n->buf + at + len, n->buf + at, end - at);
    copy(n->buf + at, e, len);
    nd_set_end(n, end + len);
}

static void nd_remove(Node *n, u32 at)
{
    u32 len = e_len(n->buf + at), end = nd_end(n);
    move_bytes(n->buf + at, n->buf + at + len, end - at - len);
    nd_set_end(n, end - len);
}

/* ---- the root: single child, empty ------------------------------------------- */
/* the root becomes just an end entry: pointing to block `vcn` (child >= 0) or a plain empty leaf (child < 0) */
static int root_reset(Ntfs *nt, s64 child)
{
    u8 *a = rec_find(DREC, nt->rec_size, AT_INDEX_ROOT, "$I30");
    if (!a)
        return NTFS_ERR_IO;
    u8 v[56];
    zero(v, sizeof v);
    copy(v, a + rd16(a + 20), 16);
    u32 elen = child >= 0 ? 24 : 16;
    wr32(v + 16, 16);
    wr32(v + 20, 16 + elen);
    wr32(v + 24, 16 + elen);
    v[28] = child >= 0 ? 1 : 0;
    wr16(v + 32 + 8, elen);
    wr16(v + 32 + 12, child >= 0 ? 3 : 2);
    if (child >= 0)
        wr64(v + 32 + 16, (u64)child);
    if (res_set(DREC, nt->rec_size, a, v, 32 + elen))
        return NTFS_ERR_FULL;
    IX.dirty = 1;
    return 0;
}

/* The root is full: its entries move into a new block, and the root points to that block. Returns the block. */
static int push_down(Ntfs *nt, Node *root, u64 *blk)
{
    u32 bs = nt->idx_size;
    if (bs < 2048)
        return NTFS_ERR_UNSUPPORTED;
    Node c;
    node_init_block(nt, &c, NB2, 0, nd_leaf(root));
    u32 r0 = nd_ent0(root), body = nd_end(root) - r0, c0 = nd_ent0(&c);
    if (c0 + body > bs)
        return NTFS_ERR_UNSUPPORTED;
    copy(NB2 + c0, root->buf + r0, body);
    nd_set_end(&c, c0 + body);
    /* shrink the root first: the new attributes of a folder that had no allocation need its room */
    int r = root_reset(nt, 0);
    if (r)
        return r;
    r = idx_new_block(nt, blk, 0); /* the root is a placeholder just now: do not write the record */
    if (r)
        return r;
    wr64(NB2 + 16, blk_vcn(nt, *blk));
    r = node_store(nt, &c, *blk);
    if (r)
        return r;
    return root_reset(nt, (s64)blk_vcn(nt, *blk));
}

/* ---- search ------------------------------------------------------------------ */
/* Walk down from the root to the entry for `key` (a $FILE_NAME). PATH / PDEPTH say how we got there; for a name
 * that is not there, the last level is the leaf and pos is where it goes. */
static int idx_locate(Ntfs *nt, const u8 *key, int *found)
{
    u64 blk = ROOT_BLK;
    *found = 0;
    for (int lv = 0;; lv++)
    {
        Node n;
        int r = node_load(nt, &n, NB1, blk);
        if (r)
            return r;
        u32 e0 = nd_ent0(&n), p = e0;
        const u8 *e;
        for (;; p += e_len(e))
        {
            e = n.buf + p;
            if (e_end(e))
                break;
            int c = key_cmp(nt, key, e + 16);
            if (c == 0)
            {
                PATH[lv].blk = blk;
                PATH[lv].pos = p - e0;
                PDEPTH = lv + 1;
                *found = 1;
                return 0;
            }
            if (c < 0)
                break;
        }
        PATH[lv].blk = blk;
        PATH[lv].pos = p - e0;
        if (!e_sub(e))
        {
            PDEPTH = lv + 1;
            return 0;
        }
        if (lv + 1 >= MAXD)
            return NTFS_ERR_IO;
        u64 v = e_ptr(e);
        if ((v * vcn_unit(nt)) % nt->idx_size)
            return NTFS_ERR_IO;
        blk = vcn_blk(nt, v);
        if ((blk + 1) * nt->idx_size > IX.ia_size || ib_get(nt, blk) != 1)
            return NTFS_ERR_IO;
    }
}

/* Like idx_locate, for a change that may add blocks to the index. A new block makes the folder's record bigger (one
 * more run in the $INDEX_ALLOCATION list, now and then 8 more bytes of bitmap), and the root keeps taking separators
 * until the record is full: with no room left the block could not be added in the middle of a split. So when the
 * record has too little room for the blocks this change can add (one per level of the tree), the root's entries go
 * down into a block first, which frees most of the root. */
static int idx_locate_room(Ntfs *nt, const u8 *key, int *found, int extra_levels)
{
    int r = idx_locate(nt, key, found);
    if (r || !IX.has_ia)
        return r; /* without an allocation the push-down makes its own room */
    if (rd32(DREC + 28) - rd32(DREC + 24) >= 32 + 25 * (u32)(PDEPTH + extra_levels))
        return 0;
    Node n;
    r = node_load(nt, &n, NB1, ROOT_BLK);
    if (r || nd_empty(&n))
        return r; /* nothing to move down: the record is full of other things */
    u64 nb;
    r = push_down(nt, &n, &nb);
    if (r)
        return r;
    return idx_locate(nt, key, found);
}

/* ---- insert -------------------------------------------------------------------- */
/* Put entry E (elen bytes) into the node of PATH[lv] before the entry at PATH[lv].pos. When fixptr >= 0 the entry
 * after the new one gets that child pointer (a split: the old child keeps the left half, the new block the right).
 * A full block splits: the middle entry goes up to the parent and so on up to the root; a full root pushes its
 * entries down into a block. */
static int ins_entry(Ntfs *nt, int lv, const u8 *E0, u32 elen, s64 fixptr)
{
    copy(EWORK, E0, elen);
    for (;;)
    {
        Lvl *L = &PATH[lv];
        Node n;
        int r = node_load(nt, &n, NB1, L->blk);
        if (r)
            return r;
        u32 e0 = nd_ent0(&n), at = e0 + L->pos;
        if (nd_fits(nt, &n, elen))
        {
            nd_insert(&n, at, EWORK, elen);
            if (fixptr >= 0)
                e_setptr(n.buf + at + elen, (u64)fixptr);
            return node_store(nt, &n, L->blk);
        }
        if (n.root)
        {
            u64 nb;
            u32 pos = L->pos;
            r = push_down(nt, &n, &nb);
            if (r)
                return r;
            PATH[1].blk = nb;
            PATH[1].pos = pos;
            lv = 1;
            continue;
        }

        /* split: the entries with the new one in SPL, the middle entry M goes up */
        u32 end = nd_end(&n), body = end - e0, pos = L->pos;
        copy(SPL, n.buf + e0, pos);
        copy(SPL + pos, EWORK, elen);
        copy(SPL + pos + elen, n.buf + at, end - at);
        if (fixptr >= 0)
            e_setptr(SPL + pos + elen, (u64)fixptr);
        u32 total = body + elen, half = total / 2, p = 0, m = 0, cnt = 0;
        int leaf = nd_leaf(&n);
        for (; !e_end(SPL + p); p += e_len(SPL + p))
        {
            if (p >= half && cnt >= 1 && !m && !e_end(SPL + p + e_len(SPL + p))) /* not the last name: the right half needs one */
                m = p;
            cnt++;
        }
        if (!m || cnt < 3)
            return NTFS_ERR_UNSUPPORTED;
        u32 mlen = e_len(SPL + m), after = m + mlen, endlen = e_len(SPL + p);
        u64 this_vcn = blk_vcn(nt, L->blk);
        /* the promoted entry, pointing at this node (which keeps the left half) */
        static u8 M[640];
        copy(M, SPL + m, mlen);
        u32 mnew = mlen;
        if (leaf)
        {
            zero(M + mlen, 8);
            mnew = mlen + 8;
            wr16(M + 8, mnew);
            wr16(M + 12, rd16(M + 12) | 1);
        }
        wr64(M + mnew - 8, this_vcn);
        /* the right half: a new block */
        u64 rb;
        r = idx_new_block(nt, &rb, 1);
        if (r)
            return r;
        Node rn;
        node_init_block(nt, &rn, NB2, rb, leaf);
        u32 r0 = nd_ent0(&rn), rbody = total - after;
        copy(NB2 + r0, SPL + after, rbody);
        nd_set_end(&rn, r0 + rbody);
        (void)endlen;
        r = node_store(nt, &rn, rb);
        if (r)
            return r;
        /* the left half: entries before M, then an end entry (pointing where M pointed) */
        u32 lbody = m;
        copy(n.buf + e0, SPL, lbody);
        u32 lend = e0 + lbody;
        zero(n.buf + lend, leaf ? 16 : 24);
        wr16(n.buf + lend + 8, leaf ? 16 : 24);
        wr16(n.buf + lend + 12, leaf ? 2 : 3);
        if (!leaf)
            wr64(n.buf + lend + 16, rd64(SPL + m + mlen - 8));
        nd_set_end(&n, lend + (leaf ? 16 : 24));
        r = node_store(nt, &n, L->blk);
        if (r)
            return r;
        /* go up: insert M in the parent before the entry that pointed here, which now points to the new block */
        copy(EWORK, M, mnew);
        elen = mnew;
        fixptr = (s64)blk_vcn(nt, rb);
        lv--;
    }
}

/* add a name (a ready entry without sub-node) to the open folder */
static int idx_add(Ntfs *nt, const u8 *ent, u32 elen)
{
    int found;
    int r = idx_locate_room(nt, ent + 16, &found, 1);
    if (r)
        return r;
    if (found)
        return NTFS_ERR_EXISTS;
    return ins_entry(nt, PDEPTH - 1, ent, elen, -1);
}

/* ---- remove ---------------------------------------------------------------------- */
static int prune(Ntfs *nt, int lv);

/* take the entry at PATH[lv].pos out of its node. *empty: the node has no entries left. */
static int del_at(Ntfs *nt, int lv, int *empty)
{
    Node n;
    int r = node_load(nt, &n, NB1, PATH[lv].blk);
    if (r)
        return r;
    nd_remove(&n, nd_ent0(&n) + PATH[lv].pos);
    *empty = nd_empty(&n);
    return node_store(nt, &n, PATH[lv].blk);
}

/* The block at PATH[lv] (not the root) has to go: it has no entries. Its parent's entry X that points to it:
 *  - X is a separator (a name): the name moves down into the neighbour on the right, X goes;
 *  - X is the end entry: the last name before it moves down into the neighbour on the left and becomes... the end
 *    entry takes over that name's child pointer;
 *  - X is the only entry: the parent has no children any more and goes too (the root becomes an empty leaf). */
static int prune(Ntfs *nt, int lv)
{
    u64 gone = PATH[lv].blk;
    int pl = lv - 1;
    Node p;
    int r = node_load(nt, &p, NB1, PATH[pl].blk);
    if (r)
        return r;
    u32 e0 = nd_ent0(&p), at = e0 + PATH[pl].pos;
    u32 dlen = 0;
    if (!e_end(p.buf + at))
    {
        dlen = e_len(p.buf + at) - 8;
        copy(DEMOTE, p.buf + at, dlen);
        wr16(DEMOTE + 8, dlen);
        wr16(DEMOTE + 12, rd16(DEMOTE + 12) & ~1u);
        nd_remove(&p, at);
    }
    else if (at == e0)
    {
        if (ib_set(nt, gone, 0))
            return NTFS_ERR_IO;
        if (p.root)
            return root_reset(nt, -1);
        return prune(nt, pl); /* the parent is left with nothing below it */
    }
    else
    {
        u32 y = e0;
        while (y + e_len(p.buf + y) != at)
            y += e_len(p.buf + y);
        dlen = e_len(p.buf + y) - 8;
        u64 ychild = e_ptr(p.buf + y);
        copy(DEMOTE, p.buf + y, dlen);
        wr16(DEMOTE + 8, dlen);
        wr16(DEMOTE + 12, rd16(DEMOTE + 12) & ~1u);
        nd_remove(&p, y); /* the end entry moved to y */
        e_setptr(p.buf + y, ychild);
    }
    r = node_store(nt, &p, PATH[pl].blk);
    if (r)
        return r;
    if (ib_set(nt, gone, 0))
        return NTFS_ERR_IO;
    return idx_add(nt, DEMOTE, dlen);
}

/* take a leaf entry out (at PATH[lv]) and free the block when that leaves it empty */
static int rm_leaf_entry(Ntfs *nt, int lv)
{
    int empty;
    int r = del_at(nt, lv, &empty);
    if (r)
        return r;
    if (empty && lv > 0)
        return prune(nt, lv);
    return 0;
}

/* Remove the entry for `key` from the open folder. An entry inside an inner node is replaced by the smallest
 * entry of the subtree on its right (taken out of its leaf). */
static int idx_remove(Ntfs *nt, const u8 *key)
{
    int found;
    int r = idx_locate_room(nt, key, &found, 2);
    if (r)
        return r;
    if (!found)
        return NTFS_ERR_IO;
    int lv = PDEPTH - 1;
    Node n;
    r = node_load(nt, &n, NB1, PATH[lv].blk);
    if (r)
        return r;
    u32 e0 = nd_ent0(&n), at = e0 + PATH[lv].pos;
    if (!e_sub(n.buf + at))
        return rm_leaf_entry(nt, lv);

    /* inner node: find the successor, the first entry of the leftmost leaf below the entry after this one */
    u32 zpos = PATH[lv].pos + e_len(n.buf + at);
    u64 v = e_ptr(n.buf + at + e_len(n.buf + at));
    PATH[lv].pos = zpos;
    int l2 = lv;
    for (;;)
    {
        if (++l2 >= MAXD || (v * vcn_unit(nt)) % nt->idx_size)
            return NTFS_ERR_IO;
        u64 blk = vcn_blk(nt, v);
        if ((blk + 1) * nt->idx_size > IX.ia_size || ib_get(nt, blk) != 1)
            return NTFS_ERR_IO;
        r = node_load(nt, &n, NB1, blk);
        if (r)
            return r;
        PATH[l2].blk = blk;
        PATH[l2].pos = 0;
        const u8 *f = n.buf + nd_ent0(&n);
        if (nd_leaf(&n))
        {
            if (e_end(f))
                return NTFS_ERR_IO;
            u32 sl = e_len(f);
            copy(SENT, f, sl);
            break;
        }
        v = e_ptr(f);
    }
    r = rm_leaf_entry(nt, l2); /* S leaves its leaf (the leaf may go too) */
    if (r)
        return r;
    u32 slen_ = e_len(SENT);

    /* now E: still in its inner node, unless freeing a block made it move down into a leaf */
    r = idx_locate(nt, key, &found);
    if (r)
        return r;
    if (!found)
        return NTFS_ERR_IO;
    lv = PDEPTH - 1;
    r = node_load(nt, &n, NB1, PATH[lv].blk);
    if (r)
        return r;
    e0 = nd_ent0(&n);
    at = e0 + PATH[lv].pos;
    if (!e_sub(n.buf + at))
    {
        r = rm_leaf_entry(nt, lv);
        if (r)
            return r;
        return idx_add(nt, SENT, slen_);
    }
    static u8 R[640];
    u64 child = e_ptr(n.buf + at);
    copy(R, SENT, slen_);
    zero(R + slen_, 8);
    wr16(R + 8, slen_ + 8);
    wr16(R + 12, rd16(R + 12) | 1);
    wr64(R + slen_, child);
    nd_remove(&n, at);
    r = node_store(nt, &n, PATH[lv].blk);
    if (r)
        return r;
    return ins_entry(nt, lv, R, slen_ + 8, -1);
}

/* patch the fixed-size part of an entry's key (times, sizes, flags) in place */
static int idx_update(Ntfs *nt, const u8 *key, const u8 *newkey)
{
    int found;
    int r = idx_locate(nt, key, &found);
    if (r)
        return r;
    if (!found)
        return NTFS_ERR_IO;
    Node n;
    r = node_load(nt, &n, NB1, PATH[PDEPTH - 1].blk);
    if (r)
        return r;
    u8 *e = n.buf + nd_ent0(&n) + PATH[PDEPTH - 1].pos;
    copy(e + 16 + 8, newkey + 8, 56); /* times, sizes, flags, reparse: bytes 8..63 */
    return node_store(nt, &n, PATH[PDEPTH - 1].blk);
}

/* is the folder (open) free of names? */
static int idx_is_empty(Ntfs *nt, int *empty)
{
    *empty = 1;
    Node n;
    int r = node_load(nt, &n, NB1, ROOT_BLK);
    if (r)
        return r;
    if (!nd_empty(&n))
    {
        *empty = 0;
        return 0;
    }
    u64 nblocks = IX.has_ia ? IX.ia_size / nt->idx_size : 0;
    for (u64 b = 0; b < nblocks; b++)
    {
        int used = ib_get(nt, b);
        if (used < 0)
            return NTFS_ERR_IO;
        if (!used)
            continue;
        r = node_load(nt, &n, NB1, b);
        if (r)
            return r;
        if (!nd_empty(&n))
        {
            *empty = 0;
            return 0;
        }
    }
    return 0;
}

/* ---- records of files: names, sizes ----------------------------------------------- */
#define REF_MASK 0xFFFFFFFFFFFFULL
#define MAX_NAMES 4
static u8 KNEW[600], ENT[640], ATMP[1024];
static u8 KOLD[MAX_NAMES][600];
static u8 RESV[4096], RESN[4096];
static NtRuns FREED, DSR;

static u32 make_entry(u8 *out, u64 ref, const u8 *key, u32 klen)
{
    u32 len = al8(16 + klen);
    zero(out, len);
    wr64(out, ref);
    wr16(out + 8, len);
    wr16(out + 10, klen);
    copy(out + 16, key, klen);
    return len;
}

/* the resident $FILE_NAME attributes of a record: how many there are; the first `max` are put in out */
static int fn_attrs(u8 *rec, u32 rs, u8 **out, int max)
{
    int n = 0;
    for (const u8 *a = attr_first(rec, rs); a && rd32(a) != AT_END; a = attr_next(rec, rs, a))
        if (rd32(a) == AT_FILE_NAME && a[8] == 0 && rd32(a + 16) >= 66)
        {
            if (n < max)
                out[n] = (u8 *)a;
            n++;
        }
    return n;
}
static u8 *attr_val(u8 *a) { return a + rd16(a + 20); }

/* allocated and real size of what the record holds: a file's $DATA, a folder's $INDEX_ALLOCATION */
static void data_sizes(u8 *rec, u32 rs, u64 *alloc, u64 *real)
{
    u8 *a = (rd16(rec + 22) & 2) ? rec_find(rec, rs, AT_INDEX_ALLOCATION, "$I30") : rec_find(rec, rs, AT_DATA, 0);
    *alloc = *real = 0;
    if (!a)
        return;
    if (a[8] == 0)
    {
        *real = rd32(a + 16);
        *alloc = al8((u32)*real);
    }
    else
    {
        *alloc = rd64(a + 40);
        *real = rd64(a + 48);
    }
}

/* copy times and sizes into every $FILE_NAME of the record */
static void names_refresh(Ntfs *nt, u8 *rec)
{
    u32 rs = nt->rec_size;
    u8 *si = rec_find(rec, rs, AT_STANDARD_INFORMATION, 0);
    u64 alloc, real;
    data_sizes(rec, rs, &alloc, &real);
    u8 *fa[MAX_NAMES];
    int n = fn_attrs(rec, rs, fa, MAX_NAMES);
    for (int i = 0; i < n && i < MAX_NAMES; i++)
    {
        u8 *v = attr_val(fa[i]);
        if (si && si[8] == 0 && rd32(si + 16) >= 32)
        {
            const u8 *s = attr_val(si);
            copy(v + 8, s, 8);       /* created */
            copy(v + 16, s + 8, 8);  /* modified */
            copy(v + 24, s + 16, 8); /* MFT changed */
            copy(v + 32, s + 24, 8); /* accessed */
        }
        wr64(v + 40, alloc);
        wr64(v + 48, real);
    }
}

/* the same times and sizes in the index entries of the folders that list the file */
static int names_index_sync(Ntfs *nt, u8 *rec)
{
    u8 *fa[MAX_NAMES];
    int n = fn_attrs(rec, nt->rec_size, fa, MAX_NAMES);
    for (int i = 0; i < n && i < MAX_NAMES; i++)
    {
        u8 *v = attr_val(fa[i]);
        int r = idx_open(nt, (u32)(rd64(v) & REF_MASK));
        if (r == 0)
            r = idx_update(nt, v, v);
        if (r == 0)
            r = idx_close(nt, 1, 0);
        if (r)
            return r;
    }
    return 0;
}

/* free the clusters of every non-resident attribute of the record */
static int free_record_clusters(Ntfs *nt, u8 *rec)
{
    static NtRuns rs;
    for (const u8 *a = attr_first(rec, nt->rec_size); a && rd32(a) != AT_END; a = attr_next(rec, nt->rec_size, a))
        if (a[8] != 0 && rd64(a + 16) == 0)
        {
            rs.n = 0;
            if (parse_runs(a + rd16(a + 32), a + rd32(a + 4), 0, &rs))
                return -1;
            if (free_runs(nt, &rs, 0))
                return -1;
        }
    return 0;
}

/* Enough free clusters for the blocks one index change can need: a split takes a block per level (levels), and a
 * folder without allocation takes one when its root overflows. 0 or an error. */
static int index_room(Ntfs *nt, int levels)
{
    u64 need = (u64)(levels + 1) * (nt->idx_size >= nt->csize ? nt->idx_size / nt->csize : 1);
    u64 found = 0, c = nt->hint < nt->clusters ? nt->hint : 0;
    for (u64 seen = 0; seen < nt->clusters && found < need; seen++, c++)
    {
        if (c >= nt->clusters)
            c = 0;
        int bit = bm_get(nt, c);
        if (bit < 0)
            return NTFS_ERR_IO;
        if (!bit)
            found++;
    }
    return found >= need ? 0 : NTFS_ERR_FULL;
}

/* open folder `dir` and see whether changing the name `key` in it has the room it may need */
static int parent_room(Ntfs *nt, u32 dir, const u8 *key)
{
    int r = idx_open(nt, dir);
    if (r)
        return r;
    int found;
    r = idx_locate(nt, key, &found);
    if (r)
        return r;
    return index_room(nt, IX.has_ia ? PDEPTH + 1 : 0);
}

/* ---- create ------------------------------------------------------------------------- */
static int create_inner(Ntfs *nt, const NtEntry *dir, const char *name, int is_dir, NtEntry *out)
{
    u32 rs = nt->rec_size;
    int r = idx_open(nt, dir->recno);
    if (r)
        return r;
    u8 *si = rec_find(DREC, rs, AT_STANDARD_INFORMATION, 0);
    if (!si || si[8] != 0 || rd32(si + 16) < 72)
        return NTFS_ERR_UNSUPPORTED; /* a folder from before NTFS 3.0: security descriptors would be needed */
    u32 sid = rd32(attr_val(si) + 52);
    u64 now = now_ft();
    u32 klen = make_key(KNEW, (u64)dir->recno | (u64)rd16(DREC + 16) << 48, name, now, 0, 0, is_dir ? 0x10000000u : 0x20u, 1);
    int found;
    r = idx_locate(nt, KNEW, &found);
    if (r)
        return r;
    if (found)
        return NTFS_ERR_EXISTS;
    r = index_room(nt, IX.has_ia ? PDEPTH : 0);
    if (r)
        return r;
    u32 recno;
    r = mb_find_free(nt, &recno);
    if (r)
        return r;
    u32 seq = rec_slot_seq(nt, recno);
    if (!seq)
        seq = 1;

    /* the new record: header, $STANDARD_INFORMATION, $FILE_NAME, and an empty $DATA or an empty index */
    u32 cnt = rs / 512 + 1, first = al8(0x30 + 2 * cnt);
    zero(FREC, rs);
    copy(FREC, "FILE", 4);
    wr16(FREC + 4, 0x30);
    wr16(FREC + 6, cnt);
    wr16(FREC + 16, seq);
    wr16(FREC + 18, 1);
    wr16(FREC + 20, first);
    wr16(FREC + 22, is_dir ? 3 : 1);
    wr32(FREC + 24, first + 8);
    wr32(FREC + 28, rs);
    wr32(FREC + 44, recno);
    wr16(FREC + 0x30, 1);
    wr32(FREC + first, AT_END);
    u8 v[72];
    zero(v, sizeof v);
    for (int i = 0; i < 4; i++)
        wr64(v + 8 * i, now);
    wr32(v + 32, is_dir ? 0x10 : 0x20);
    wr32(v + 52, sid); /* the security descriptor of the folder it is made in */
    build_res(ATMP, AT_STANDARD_INFORMATION, 0, v, sizeof v, 0);
    if (!rec_insert(FREC, rs, ATMP))
        return NTFS_ERR_FULL;
    build_res(ATMP, AT_FILE_NAME, 0, KNEW, klen, 1); /* 1 = indexed */
    if (!rec_insert(FREC, rs, ATMP))
        return NTFS_ERR_FULL;
    if (is_dir)
    {
        u8 root[48];
        zero(root, sizeof root);
        wr32(root, AT_FILE_NAME);
        wr32(root + 4, 1); /* collation: file names */
        wr32(root + 8, nt->idx_size);
        u32 sh = 0;
        while ((1u << sh) < nt->idx_size)
            sh++;
        root[12] = nt->idx_size >= nt->csize ? (u8)(nt->idx_size / nt->csize) : (u8)(256 - sh);
        wr32(root + 16, 16); /* index header: entries at +16, used 32, allocated 32, no sub-nodes */
        wr32(root + 20, 32);
        wr32(root + 24, 32);
        wr16(root + 32 + 8, 16); /* the end entry */
        wr16(root + 32 + 12, 2);
        build_res(ATMP, AT_INDEX_ROOT, "$I30", root, sizeof root, 0);
    }
    else
        build_res(ATMP, AT_DATA, 0, 0, 0, 0);
    if (!rec_insert(FREC, rs, ATMP))
        return NTFS_ERR_FULL;

    /* bitmap, record, then the index */
    if (mb_set(nt, recno, 1) || rec_store(nt, recno, FREC))
        return NTFS_ERR_IO;
    u32 elen = make_entry(ENT, (u64)recno | (u64)seq << 48, KNEW, klen);
    IDX_WROTE = 0;
    r = idx_add(nt, ENT, elen);
    if (r == 0)
        r = idx_close(nt, 1, now);
    if (r)
    {
        if (IDX_WROTE || nt->poisoned)
        {
            nt->poisoned = 1; /* the record exists but is not listed: chkdsk has to look */
            return r;
        }
        /* a refusal before anything of the index was written (no room to grow the folder ...): take the new record back */
        u32 s = (rd16(FREC + 16) + 1u) & 0xFFFF;
        wr16(FREC + 22, 0);
        wr16(FREC + 16, s ? s : 1);
        if (rec_store(nt, recno, FREC) || mb_set(nt, recno, 0))
            nt->poisoned = 1;
        return r;
    }
    out->recno = recno;
    out->size = 0;
    out->is_dir = is_dir != 0;
    out->pad[0] = out->pad[1] = out->pad[2] = 0;
    return 0;
}

int ntfs_create(Ntfs *nt, const NtEntry *dir, const char *name, int is_dir, NtEntry *out)
{
    if (!dir || !out)
        return NTFS_ERR_ARG;
    int r = name_ok(name);
    if (r)
        return r;
    r = wr_begin(nt);
    if (r)
        return r;
    return wr_end(nt, create_inner(nt, dir, name, is_dir, out));
}

/* ---- write, truncate ------------------------------------------------------------------- */
/* keep the first `keep` clusters of rs; the rest goes to freed */
static void runs_trim(NtRuns *rs, u64 keep, NtRuns *freed)
{
    freed->n = 0;
    u64 acc = 0;
    for (int i = 0; i < rs->n; i++)
    {
        NtRun *r = &rs->r[i];
        if (acc + r->len <= keep)
        {
            acc += r->len;
            continue;
        }
        u64 take = keep > acc ? keep - acc : 0;
        if (r->len > take)
        {
            freed->r[freed->n].vcn = r->vcn + take;
            freed->r[freed->n].len = r->len - take;
            freed->r[freed->n].lcn = r->lcn + (s64)take;
            freed->n++;
        }
        for (int j = i + 1; j < rs->n; j++)
            copy(&freed->r[freed->n++], &rs->r[j], sizeof(NtRun));
        r->len = take;
        rs->n = take ? i + 1 : i;
        return;
    }
}

/* is_trunc: make the file tsize bytes; else write len bytes of buf at offset. */
static int data_op(Ntfs *nt, NtEntry *e, int is_trunc, u64 offset, const u8 *buf, u32 len, u64 tsize)
{
    u32 rs = nt->rec_size;
    if (e->recno < 24)
        return NTFS_ERR_UNSUPPORTED;
    if (record_read(nt, e->recno, FREC))
        return NTFS_ERR_IO;
    if ((rd16(FREC + 22) & 2) || rd64(FREC + 32))
        return NTFS_ERR_ARG;
    if (rec_find(FREC, rs, AT_ATTRIBUTE_LIST, 0))
        return NTFS_ERR_UNSUPPORTED;
    u8 *a = rec_find(FREC, rs, AT_DATA, 0);
    if (!a)
        return NTFS_ERR_UNSUPPORTED;
    if (rd16(a + 12) & (FL_COMPRESSED | FL_ENCRYPTED | 0x8000))
        return NTFS_ERR_UNSUPPORTED;
    int res = a[8] == 0;
    u64 size, init, alloc = 0;
    DSR.n = 0;
    if (res)
    {
        size = init = rd32(a + 16);
        if (size > sizeof RESV || rd16(a + 20) + size > rd32(a + 4))
            return NTFS_ERR_IO;
        copy(RESV, attr_val(a), (u32)size);
    }
    else
    {
        if (rd64(a + 16) != 0)
            return NTFS_ERR_UNSUPPORTED;
        size = rd64(a + 48);
        init = rd64(a + 56);
        alloc = rd64(a + 40);
        if (parse_runs(a + rd16(a + 32), a + rd32(a + 4), 0, &DSR))
            return NTFS_ERR_IO;
        for (int i = 0; i < DSR.n; i++)
            if (DSR.r[i].lcn < 0)
                return NTFS_ERR_UNSUPPORTED; /* sparse */
        if (runs_clusters(&DSR) * nt->csize < alloc || alloc < size || init > size)
            return NTFS_ERR_IO;
    }
    u64 new_size;
    if (is_trunc)
        new_size = tsize;
    else
    {
        if (offset > size)
            return NTFS_ERR_ARG;
        new_size = offset + len > size ? offset + len : size;
    }
    if (new_size > 0xFFFFFFFFull)
        return NTFS_ERR_FULL;
    u64 now = now_ft();

    if (res)
    {
        /* stay resident when the new content fits in the record */
        if (new_size <= sizeof RESN)
        {
            copy(RESN, RESV, (u32)size);
            if (new_size > size)
                zero(RESN + size, (u32)(new_size - size));
            if (!is_trunc)
                copy(RESN + offset, buf, len);
            if (res_set(FREC, rs, a, RESN, (u32)new_size) == 0)
            {
                touch_si(FREC, rs, now);
                names_refresh(nt, FREC);
                if (rec_store(nt, e->recno, FREC))
                    return NTFS_ERR_IO;
                int r = names_index_sync(nt, FREC);
                e->size = clamp32(new_size);
                return r;
            }
        }
        init = size; /* it becomes non-resident below: the old bytes are all initialized */
    }

    /* non-resident: clusters for new_size, the new sizes, the record, then the data */
    u64 need = (new_size + nt->csize - 1) / nt->csize, have = runs_clusters(&DSR);
    u64 new_init = is_trunc ? (new_size < init ? new_size : init) : (offset + len > init ? offset + len : init);
    NEWR.n = 0;
    FREED.n = 0;
    if (need > have)
    {
        u64 hint = DSR.n ? (u64)DSR.r[DSR.n - 1].lcn + DSR.r[DSR.n - 1].len : nt->hint;
        int r = alloc_clusters(nt, need - have, hint, &NEWR);
        if (r)
            return r;
        for (int i = 0; i < NEWR.n; i++)
            runs_append(&DSR, (u64)NEWR.r[i].lcn, NEWR.r[i].len);
    }
    else if (need < have)
        runs_trim(&DSR, need, &FREED);
    u64 new_alloc = need * nt->csize;
    int ok;
    if (res)
    {
        u32 l = build_nonres(ATMP, sizeof ATMP, AT_DATA, 0, &DSR, new_alloc, new_size, new_init);
        ok = l && rd32(FREC + 24) - rd32(a + 4) + l <= rd32(FREC + 28);
        if (ok)
        {
            rec_remove(FREC, a);
            ok = rec_insert(FREC, rs, ATMP) != 0;
        }
    }
    else
        ok = nonres_set(FREC, rs, a, &DSR, new_alloc, new_size, new_init) == 0;
    if (!ok)
    {
        free_runs(nt, &NEWR, 0); /* the record could not hold the run list: give the new clusters back */
        return NTFS_ERR_FULL;
    }
    if (res && size && runs_write(nt, &DSR, 0, RESV, (u32)size))
        return NTFS_ERR_IO;
    if (!is_trunc)
    {
        if (offset > init && runs_zero(nt, &DSR, init, offset - init))
            return NTFS_ERR_IO;
        if (runs_write(nt, &DSR, offset, buf, len))
            return NTFS_ERR_IO;
    }
    touch_si(FREC, rs, now);
    names_refresh(nt, FREC);
    if (rec_store(nt, e->recno, FREC))
        return NTFS_ERR_IO;
    if (free_runs(nt, &FREED, 0))
        return NTFS_ERR_IO;
    e->size = clamp32(new_size);
    return names_index_sync(nt, FREC);
}

int ntfs_write(Ntfs *nt, NtEntry *e, unsigned offset, const void *buf, unsigned len)
{
    if (!e || (!buf && len))
        return NTFS_ERR_ARG;
    if (len == 0)
        return 0;
    int r = wr_begin(nt);
    if (r)
        return r;
    r = data_op(nt, e, 0, offset, (const u8 *)buf, len, 0);
    return wr_end(nt, r < 0 ? r : (int)len);
}

int ntfs_truncate(Ntfs *nt, NtEntry *e, unsigned size)
{
    if (!e)
        return NTFS_ERR_ARG;
    int r = wr_begin(nt);
    if (r)
        return r;
    return wr_end(nt, data_op(nt, e, 1, 0, 0, 0, size));
}

/* ---- delete ------------------------------------------------------------------------------- */
/* read the names of the record in FREC into KOLD: returns how many (or an error); *parent = the folder */
static int names_of_file(Ntfs *nt, u32 *parent, u32 *klens)
{
    u8 *fa[MAX_NAMES];
    int n = fn_attrs(FREC, nt->rec_size, fa, MAX_NAMES);
    if (n < 1 || n > MAX_NAMES)
        return NTFS_ERR_UNSUPPORTED; /* no name, or hard links */
    for (int i = 0; i < n; i++)
    {
        u32 kl = rd32(fa[i] + 16);
        if (kl > sizeof KOLD[0])
            return NTFS_ERR_IO;
        copy(KOLD[i], attr_val(fa[i]), kl);
        klens[i] = kl;
        u32 p = (u32)(rd64(KOLD[i]) & REF_MASK);
        if (i == 0)
            *parent = p;
        else if (p != *parent)
            return NTFS_ERR_UNSUPPORTED; /* a hard link in another folder */
    }
    return n;
}

static int delete_inner(Ntfs *nt, const NtEntry *e)
{
    u32 rs = nt->rec_size;
    if (e->recno < 24)
        return NTFS_ERR_UNSUPPORTED;
    if (record_read(nt, e->recno, FREC))
        return NTFS_ERR_IO;
    if (rd64(FREC + 32) || rec_find(FREC, rs, AT_ATTRIBUTE_LIST, 0))
        return NTFS_ERR_UNSUPPORTED;
    u32 parent, kl[MAX_NAMES];
    int n = names_of_file(nt, &parent, kl);
    if (n < 0)
        return n;
    int r;
    if (rd16(FREC + 22) & 2)
    {
        int empty;
        r = idx_open(nt, e->recno);
        if (r == 0)
            r = idx_is_empty(nt, &empty);
        if (r)
            return r;
        if (!empty)
            return NTFS_ERR_NOTEMPTY;
    }
    /* Taking a name out of a big folder can need a block (a name moves down). On a full disk the file's own clusters
     * are given back first for that: if the power fails now, the volume is dirty and chkdsk takes the file's
     * clusters back, which it would otherwise have to do for a file that is half deleted anyway. */
    int early = 0;
    r = parent_room(nt, parent, KOLD[0]);
    if (r == NTFS_ERR_FULL)
    {
        if (free_record_clusters(nt, FREC))
            return NTFS_ERR_IO;
        early = 1;
        r = parent_room(nt, parent, KOLD[0]);
        if (r)
        {
            nt->poisoned = 1;
            return r;
        }
    }
    else if (r)
        return r;
    /* out of the folder first: nothing lists a file that is about to be freed */
    IDX_WROTE = 0;
    r = idx_open(nt, parent);
    for (int i = 0; r == 0 && i < n; i++)
        r = idx_remove(nt, KOLD[i]);
    if (r == 0)
        r = idx_close(nt, 1, now_ft());
    if (r)
    {
        if (IDX_WROTE || early || nt->poisoned)
            nt->poisoned = 1;
        return r;
    }
    /* the record, the clusters, then the MFT bitmap */
    u32 seq = (rd16(FREC + 16) + 1u) & 0xFFFF;
    wr16(FREC + 16, seq ? seq : 1);
    wr16(FREC + 18, 0);
    wr16(FREC + 22, rd16(FREC + 22) & ~3u);
    if (rec_store(nt, e->recno, FREC) || (!early && free_record_clusters(nt, FREC)) || mb_set(nt, e->recno, 0))
        return NTFS_ERR_IO;
    return 0;
}

int ntfs_delete(Ntfs *nt, const NtEntry *e)
{
    if (!e)
        return NTFS_ERR_ARG;
    int r = wr_begin(nt);
    if (r)
        return r;
    return wr_end(nt, delete_inner(nt, e));
}

/* ---- rename, move -------------------------------------------------------------------------- */
static int idx_found_ref(Ntfs *nt, u32 *recno)
{
    Node n;
    int r = node_load(nt, &n, NB1, PATH[PDEPTH - 1].blk);
    if (r)
        return r;
    *recno = (u32)(rd64(n.buf + nd_ent0(&n) + PATH[PDEPTH - 1].pos) & REF_MASK);
    return 0;
}

/* new_dir < 0: the folder it is in now */
static int move_inner(Ntfs *nt, NtEntry *e, long long new_dir, const char *name)
{
    u32 rs = nt->rec_size;
    if (e->recno < 24)
        return NTFS_ERR_UNSUPPORTED;
    int r;
    if (record_read(nt, e->recno, FREC))
        return NTFS_ERR_IO;
    if (rd64(FREC + 32) || rec_find(FREC, rs, AT_ATTRIBUTE_LIST, 0))
        return NTFS_ERR_UNSUPPORTED;
    u32 old_parent, kl[MAX_NAMES];
    int n = names_of_file(nt, &old_parent, kl);
    if (n < 0)
        return n;
    r = parent_room(nt, old_parent, KOLD[0]); /* taking the old names out may need a block */
    if (r)
        return r;
    u32 np = new_dir >= 0 ? (u32)new_dir : old_parent;
    int is_dir = (rd16(FREC + 22) & 2) != 0;
    if (is_dir && np != old_parent)
    {
        u32 cur = np; /* the new place must not be inside the folder itself */
        for (int d = 0; d < 64; d++)
        {
            if (cur == e->recno)
                return NTFS_ERR_ARG;
            if (cur == 5)
                break;
            if (record_read(nt, cur, MREC))
                return NTFS_ERR_IO;
            u8 *fa[1];
            if (fn_attrs(MREC, rs, fa, 1) < 1)
                return NTFS_ERR_IO;
            cur = (u32)(rd64(attr_val(fa[0])) & REF_MASK);
        }
    }
    r = idx_open(nt, np);
    if (r)
        return r;
    u32 klen = make_key(KNEW, (u64)np | (u64)rd16(DREC + 16) << 48, name, 0, 0, 0, 0, 1);
    copy(KNEW + 8, KOLD[0] + 8, 52); /* times, sizes and flags stay */
    int found;
    r = idx_locate(nt, KNEW, &found);
    if (r)
        return r;
    int same = 0; /* the name is taken by the file itself (only the letter case changes) */
    if (found)
    {
        u32 who;
        r = idx_found_ref(nt, &who);
        if (r)
            return r;
        if (who != e->recno || np != old_parent)
            return NTFS_ERR_EXISTS;
        same = 1;
        if (n == 1 && KOLD[0][64] == KNEW[64])
        {
            int equal = 1;
            for (u32 i = 0; i < (u32)KNEW[64] * 2; i++)
                if (KOLD[0][66 + i] != KNEW[66 + i])
                    equal = 0;
            if (equal)
                return 0; /* the same name */
        }
    }
    r = index_room(nt, IX.has_ia ? PDEPTH + 1 : 0); /* putting the new name in may split blocks */
    if (r)
        return r;
    u64 seq = rd16(FREC + 16);
    u32 elen = make_entry(ENT, (u64)e->recno | seq << 48, KNEW, klen);
    u64 now = now_ft();
    if (same)
    {
        /* one folder: take the old names out, put the new one in (the order cannot be the other way round) */
        IDX_WROTE = 0;
        for (int i = 0; r == 0 && i < n; i++)
            r = idx_remove(nt, KOLD[i]);
        if (r == 0)
            r = idx_add(nt, ENT, elen);
        if (r == 0)
            r = idx_close(nt, 1, now);
        if (r)
        {
            if (IDX_WROTE || nt->poisoned)
                nt->poisoned = 1;
            return r;
        }
    }
    else
    {
        /* the new name goes in first: the file is listed twice for a moment, never not at all */
        IDX_WROTE = 0;
        r = idx_add(nt, ENT, elen);
        if (r == 0)
            r = idx_close(nt, 1, now);
        if (r)
        {
            if (IDX_WROTE || nt->poisoned)
                nt->poisoned = 1;
            return r;
        }
    }
    /* the record: its names become the one new name */
    u8 *fa[MAX_NAMES];
    while (fn_attrs(FREC, rs, fa, 1) > 0)
        rec_remove(FREC, fa[0]);
    build_res(ATMP, AT_FILE_NAME, 0, KNEW, klen, 1);
    if (!rec_insert(FREC, rs, ATMP))
    {
        nt->poisoned = 1;
        return NTFS_ERR_FULL;
    }
    wr16(FREC + 18, 1);
    touch_si(FREC, rs, now);
    names_refresh(nt, FREC);
    if (rec_store(nt, e->recno, FREC))
        return NTFS_ERR_IO;
    if (!same)
    {
        r = idx_open(nt, old_parent);
        for (int i = 0; r == 0 && i < n; i++)
            r = idx_remove(nt, KOLD[i]);
        if (r == 0)
            r = idx_close(nt, 1, now);
        if (r)
        {
            nt->poisoned = 1;
            return r;
        }
    }
    r = names_index_sync(nt, FREC);
    return r;
}

int ntfs_move(Ntfs *nt, NtEntry *e, const NtEntry *new_dir, const char *new_name)
{
    if (!e || !new_dir)
        return NTFS_ERR_ARG;
    int r = name_ok(new_name);
    if (r)
        return r;
    r = wr_begin(nt);
    if (r)
        return r;
    return wr_end(nt, move_inner(nt, e, (long long)new_dir->recno, new_name));
}

int ntfs_rename(Ntfs *nt, NtEntry *e, const char *new_name)
{
    if (!e)
        return NTFS_ERR_ARG;
    int r = name_ok(new_name);
    if (r)
        return r;
    r = wr_begin(nt);
    if (r)
        return r;
    return wr_end(nt, move_inner(nt, e, -1, new_name));
}
