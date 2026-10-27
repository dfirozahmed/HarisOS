/* NTFS write side: writing, truncating and deleting files. */
#include "ntfs_internal.h"

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
        return unsup(__LINE__);
    if (record_read(nt, e->recno, FREC))
        return NTFS_ERR_IO;
    if ((rd16(FREC + 22) & 2) || rd64(FREC + 32))
        return NTFS_ERR_ARG;
    if (rec_find(FREC, rs, AT_ATTRIBUTE_LIST, 0))
        return unsup(__LINE__);
    u8 *a = rec_find(FREC, rs, AT_DATA, 0);
    if (!a)
        return unsup(__LINE__);
    if (rd16(a + 12) & (FL_COMPRESSED | FL_ENCRYPTED | 0x8000))
        return unsup(__LINE__);
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
            return unsup(__LINE__);
        size = rd64(a + 48);
        init = rd64(a + 56);
        alloc = rd64(a + 40);
        if (parse_runs(a + rd16(a + 32), a + rd32(a + 4), 0, &DSR))
            return NTFS_ERR_IO;
        for (int i = 0; i < DSR.n; i++)
            if (DSR.r[i].lcn < 0)
                return unsup(__LINE__); /* sparse */
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
int names_of_file(Ntfs *nt, u32 *parent, u32 *klens)
{
    u8 *fa[MAX_NAMES];
    int n = fn_attrs(FREC, nt->rec_size, fa, MAX_NAMES);
    if (n < 1 || n > MAX_NAMES)
        return unsup(__LINE__); /* no name, or hard links */
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
            return unsup(__LINE__); /* a hard link in another folder */
    }
    return n;
}

static int delete_inner(Ntfs *nt, const NtEntry *e)
{
    u32 rs = nt->rec_size;
    if (e->recno < 24)
        return unsup(__LINE__);
    if (record_read(nt, e->recno, FREC))
        return NTFS_ERR_IO;
    if (rd64(FREC + 32) || rec_find(FREC, rs, AT_ATTRIBUTE_LIST, 0))
        return unsup(__LINE__);
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
