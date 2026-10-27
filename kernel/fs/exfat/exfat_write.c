/* exFAT write side: create, write, delete, rename, move. */
#include "exfat_internal.h"

/* ---- create ---- */
int exfat_create(ExFat *f, ExEntry *dir, const ExEntry *dir_parent, const char *name, int is_dir, ExEntry *out)
{
    if (!exf_writable(f))
        return EXFAT_ERR_RO;
    if (!exf_name_valid(name))
        return EXFAT_ERR_BADNAME;
    int r = exf_name_exists(f, dir, name, 0xFFFFFFFFu);
    if (r < 0)
        return r;
    if (r)
        return EXFAT_ERR_EXISTS;
    u32 first = 0, size = 0, csize = f->spc * f->ss;
    if (is_dir)
    {
        u32 n = 0;
        u8 nofat = 0;
        r = exf_append_cluster(f, &first, &nofat, &n, 1);
        if (r)
        {
            exf_finish(f);
            return r;
        }
        size = csize;
    }
    u8 set[32 * 20];
    u32 total = exf_make_set(f, set, name, is_dir ? EXFAT_ATTR_DIR : 0x20, first, size, size, 0, 0);
    u32 start;
    r = exf_find_free_run(f, dir, dir_parent, total, &start);
    if (r == 0)
        r = exf_write_set(f, dir, start, set, total);
    if (r)
    {
        if (first)
            exf_trim_clusters(f, &first, 0, 1, 0);
        exf_finish(f);
        return r;
    }
    zero(out, sizeof *out);
    out->first_cluster = first;
    out->size = size;
    out->valid = size;
    out->set_index = start;
    out->attr = is_dir ? EXFAT_ATTR_DIR : 0x20;
    out->secondary = (u8)(total - 1);
    return exf_finish(f);
}

/* ---- write ---- */
static int data_write(ExFat *f, u32 first, int nofat, u32 offset, const u8 *buf, u32 len)
{
    u32 csize = f->spc * f->ss, cl, index = offset / csize, pos = offset % csize;
    int r = exf_cluster_at(f, first, nofat, index, &cl);
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
                if (!exf_writable(f))
                    return EXFAT_ERR_RO;
                if (blk_write(f->blk, lba, whole, p) != 0)
                    return EXFAT_ERR_IO;
                f->sec_lba = NONE;
                p += (u64)whole * f->ss;
                left -= whole * f->ss;
                lba += whole;
                continue;
            }
            if (exf_load_sec(f, lba))
                return EXFAT_ERR_IO;
            u32 take = f->ss - head < left ? f->ss - head : left;
            copy(f->sec + head, p, take);
            r = exf_wr_sector(f, lba, f->sec);
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
        r = exf_cluster_at(f, first, nofat, index, &cl);
        if (r <= 0)
            return EXFAT_ERR_IO;
    }
}

int exfat_write(ExFat *f, const ExEntry *dir, ExEntry *e, unsigned offset, const void *buf, unsigned len)
{
    if (!exf_writable(f))
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
        r = exf_trim_clusters(f, &first, e->nofat, one, 0);
        if (r)
            return r;
        e->first_cluster = 0;
    }
    u32 have = e->first_cluster ? exf_clusters_of(f, e->size) : 0;
    u32 orig_have = have, orig_first = e->first_cluster;
    u8 orig_nofat = e->nofat;
    u32 need = exf_clusters_of(f, offset + len);
    u32 first = e->first_cluster;
    u8 nofat = e->nofat;
    while (have < need)
    {
        r = exf_append_cluster(f, &first, &nofat, &have, 0);
        if (r)
        {
            /* out of room: give back what this call took */
            u32 f2 = first;
            exf_trim_clusters(f, &f2, nofat, have, orig_have);
            e->first_cluster = orig_first; /* the entry on the disk was not touched */
            e->nofat = orig_nofat;
            exf_finish(f);
            return r;
        }
    }
    e->first_cluster = first;
    e->nofat = nofat;
    r = data_write(f, first, nofat, offset, (const u8 *)buf, len);
    if (r)
    {
        exf_finish(f);
        return r;
    }
    u32 end = offset + len;
    if (end > e->size)
        e->size = end;
    if (end > e->valid)
        e->valid = end;
    r = exf_update_set(f, dir, e);
    if (r)
        return r;
    r = exf_finish(f);
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
    if (!exf_writable(f))
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
    int r = exf_kill_set(f, dir, e); /* first the entries: a failure after this loses clusters, not a half entry */
    if (r)
        return r;
    if (e->first_cluster >= 2)
    {
        u32 first = e->first_cluster;
        r = exf_trim_clusters(f, &first, e->nofat, exf_clusters_of(f, e->size), 0);
        if (r)
            return r;
    }
    return exf_finish(f);
}

/* put the same file / folder under another name (and maybe in another folder): a new entry set, then the old one goes */
static int relink(ExFat *f, const ExEntry *old_dir, ExEntry *new_dir, const ExEntry *new_dir_parent, ExEntry *e, const char *name)
{
    u8 old[32];
    int r = exf_get_entry(f, old_dir, e->set_index, old);
    if (r <= 0)
        return EXFAT_ERR_IO;
    u8 set[32 * 20];
    u32 total = exf_make_set(f, set, name, old[4] | (old[5] << 8), e->first_cluster, e->size, e->valid, e->nofat, old + 8);
    u32 start;
    r = exf_find_free_run(f, new_dir, new_dir_parent, total, &start);
    if (r)
        return r;
    r = exf_write_set(f, new_dir, start, set, total);
    if (r)
        return r;
    r = exf_kill_set(f, old_dir, e);
    if (r)
        return r;
    e->set_index = start;
    e->secondary = (u8)(total - 1);
    return 0;
}

int exfat_rename(ExFat *f, ExEntry *dir, const ExEntry *dir_parent, ExEntry *e, const char *new_name)
{
    if (!exf_writable(f))
        return EXFAT_ERR_RO;
    if (!exf_name_valid(new_name))
        return EXFAT_ERR_BADNAME;
    int r = exf_name_exists(f, dir, new_name, e->set_index);
    if (r < 0)
        return r;
    if (r)
        return EXFAT_ERR_EXISTS;
    ExEntry same = *dir;
    r = relink(f, &same, dir, dir_parent, e, new_name);
    if (r)
    {
        exf_finish(f);
        return r;
    }
    return exf_finish(f);
}

int exfat_move(ExFat *f, const ExEntry *old_dir, ExEntry *new_dir, const ExEntry *new_dir_parent, ExEntry *e, const char *name)
{
    if (!exf_writable(f))
        return EXFAT_ERR_RO;
    if (!exf_name_valid(name))
        return EXFAT_ERR_BADNAME;
    int r = exf_name_exists(f, new_dir, name, 0xFFFFFFFFu);
    if (r < 0)
        return r;
    if (r)
        return EXFAT_ERR_EXISTS;
    r = relink(f, old_dir, new_dir, new_dir_parent, e, name);
    if (r)
    {
        exf_finish(f);
        return r;
    }
    return exf_finish(f);
}
