/* NTFS: reading the volume, data runs, MFT records. See include/ntfs.h and ntfs_internal.h. */
#include "ntfs_internal.h"

/* Scratch space shared by all volumes: the code is not re-entered. Big things stay out of the stack. */
NtStream S_MAIN, S_AUX, S_FILE; /* S_FILE: the file being read (kept between calls) */
const Ntfs *S_FILE_NT;

/* Every refusal with NTFS_ERR_UNSUPPORTED remembers the source line it came from, so "not supported yet" can be
 * traced to the exact limit that was hit (see ntfs_unsupported_line). */
static int UNSUP_LINE;
static const char *UNSUP_WHY; /* a sentence for the limits that users run into (0 = only the line is known) */
int unsup(int line)
{
    UNSUP_LINE = line;
    UNSUP_WHY = 0;
    return NTFS_ERR_UNSUPPORTED;
}
int unsup_why(int line, const char *why)
{
    unsup(line);
    UNSUP_WHY = why;
    return NTFS_ERR_UNSUPPORTED;
}
int ntfs_unsupported_line(void) { return UNSUP_LINE; }
const char *ntfs_unsupported_why(void) { return UNSUP_WHY; }
unsigned S_FILE_REC = ~0u;
u8 REC[4096], REC2[4096];
static u8 ALBUF[32768];
u8 ROOTBUF[RES_MAX];
u8 IDXBUF[16384];
u8 NAMEBUF[NTFS_NAME_MAX * 2];

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
int runs_read(Ntfs *nt, const NtRuns *rs, u64 offset, u8 *buf, u32 len)
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
int parse_runs(const u8 *p, const u8 *end, u64 start_vcn, NtRuns *rs)
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
int apply_fixups(u8 *b, u32 len)
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

int record_read(Ntfs *nt, u32 recno, u8 *buf)
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
const u8 *attr_first(const u8 *rec, u32 rec_size)
{
    u32 o = rd16(rec + 20);
    return (o + 24 <= rec_size) ? rec + o : (const u8 *)0;
}

const u8 *attr_next(const u8 *rec, u32 rec_size, const u8 *a)
{
    u32 len = rd32(a + 4);
    u32 off = (u32)(a - rec) + len;
    if (len < 16 || (len & 7) || off + 8 > rec_size)
        return (const u8 *)0;
    return rec + off;
}

/* is this attribute of the wanted type and name (ASCII; NULL = unnamed)? */
int attr_is(const u8 *a, u32 type, const char *name)
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
int load_stream(Ntfs *nt, u32 recno, u32 type, const char *name, NtStream *s)
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
int stream_read(Ntfs *nt, const NtStream *s, u64 offset, u8 *buf, u32 len)
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
