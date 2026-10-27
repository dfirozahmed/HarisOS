/* NTFS write side: how a change is made, raw writes, MFT records and attributes. */
#include "ntfs_internal.h"

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
u64 now_ft(void) { return CLOCK ? CLOCK() : 0x01DC000000000000ULL; }


/* like memmove */
void move_bytes(void *dst, const void *src, u32 n)
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


NtStream S_BM, S_MB, S_UP, S_LOG; /* $Bitmap, the $MFT bitmap, $UpCase, $LogFile */
u8 FREC[4096], DREC[4096], MREC[4096], RSEAL[4096];
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
int runs_write(Ntfs *nt, const NtRuns *rs, u64 offset, const u8 *buf, u32 len)
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
int runs_zero(Ntfs *nt, const NtRuns *rs, u64 offset, u64 n)
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
int rec_store(Ntfs *nt, u32 recno, u8 *rec)
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
u32 rec_slot_seq(Ntfs *nt, u32 recno)
{
    if (runs_read(nt, &nt->mft, (u64)recno * nt->rec_size, RSEAL, nt->rec_size))
        return 0;
    if (RSEAL[0] != 'F' || RSEAL[1] != 'I' || RSEAL[2] != 'L' || RSEAL[3] != 'E')
        return 0;
    return rd16(RSEAL + 16);
}

u8 *rec_find(u8 *rec, u32 rs, u32 type, const char *name)
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
u8 *rec_insert(u8 *rec, u32 rs, const u8 *attr)
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

void rec_remove(u8 *rec, u8 *a)
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
u32 build_res(u8 *out, u32 type, const char *name, const u8 *val, u32 vlen, u32 res_flags)
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
int res_set(u8 *rec, u32 rs, u8 *a, const u8 *val, u32 vlen)
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

u64 runs_clusters(const NtRuns *rs)
{
    u64 n = 0;
    for (int i = 0; i < rs->n; i++)
        n += rs->r[i].len;
    return n;
}

/* a non-resident attribute with all its runs in this record. Returns its length, 0 if it does not fit in cap. */
u32 build_nonres(u8 *out, u32 cap, u32 type, const char *name, const NtRuns *rs, u64 alloc, u64 real, u64 init)
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
int nonres_set(u8 *rec, u32 rs, u8 *a, const NtRuns *runs, u64 alloc, u64 real, u64 init)
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
int runs_append(NtRuns *rs, u64 lcn, u64 len)
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
