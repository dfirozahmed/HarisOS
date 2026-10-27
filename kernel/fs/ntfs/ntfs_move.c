/* NTFS write side: rename and move. */
#include "ntfs_internal.h"

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
        return unsup(__LINE__);
    int r;
    if (record_read(nt, e->recno, FREC))
        return NTFS_ERR_IO;
    if (rd64(FREC + 32) || rec_find(FREC, rs, AT_ATTRIBUTE_LIST, 0))
        return unsup(__LINE__);
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
