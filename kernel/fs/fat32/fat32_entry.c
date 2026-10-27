/* FAT32: changing entries. */
#include "fat32_internal.h"

/* ---- changing entries ------------------------------------------- */
void f32_now(unsigned *date, unsigned *time)
{
    if (f32_CLOCK)
        f32_CLOCK(date, time);
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
    int r = f32_slot_read(f, e->dir_cluster, e->index, raw);
    if (r <= 0)
        return r < 0 ? r : FAT_ERR_IO;
    wr16(raw + 20, e->first_cluster >> 16);
    wr16(raw + 26, e->first_cluster & 0xFFFF);
    wr32(raw + 28, (e->attr & FAT_ATTR_DIR) ? 0 : e->size);
    if (touch)
    {
        unsigned d, t;
        f32_now(&d, &t);
        wr16(raw + 22, t);
        wr16(raw + 24, d);
        wr16(raw + 18, d);
        raw[11] |= FAT_ATTR_ARCHIVE;
    }
    return f32_slot_write(f, e->dir_cluster, e->index, raw);
}

int fat_write(Fat *f, FatEntry *e, unsigned offset, const void *buf, unsigned len)
{
    if (!f32_writable(f))
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
        int fr = f32_free_chain(f, e->first_cluster);
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
        r = f32_alloc_cluster(f, &c);
        if (r)
        {
            /* out of room (or an error): give back what this call took, so the file keeps its old size and chain */
            if (have > orig_have)
            {
                u32 tail, extra;
                if (orig_have == 0)
                    f32_free_chain(f, e->first_cluster);
                else if (f32_chain_at(f, e->first_cluster, orig_have - 1, &tail) > 0 && f32_fat_get(f, tail, &extra) == 0)
                {
                    f32_fat_set(f, tail, 0x0FFFFFFF);
                    if (valid_cluster(f, extra))
                        f32_free_chain(f, extra);
                }
                f32_forget_cursor(f);
            }
            e->first_cluster = orig_first;
            f32_finish(f);
            return r;
        }
        if (have == 0)
            e->first_cluster = c;
        else
        {
            u32 tail;
            r = f32_chain_at(f, e->first_cluster, have - 1, &tail);
            if (r <= 0)
                return r < 0 ? r : FAT_ERR_IO;
            r = f32_fat_set(f, tail, c);
            if (r)
                return r;
        }
        have++;
    }
    r = f32_chain_io(f, e->first_cluster, offset, (u8 *)buf, len, 1);
    if (r)
    {
        f32_finish(f);
        return r;
    }
    if (offset + len > e->size)
        e->size = offset + len;
    r = update_entry(f, e, 1);
    if (r)
        return r;
    r = f32_finish(f);
    return r ? r : (int)len;
}

int fat_shrink(Fat *f, FatEntry *e, unsigned new_size)
{
    if (!f32_writable(f))
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
            r = f32_free_chain(f, e->first_cluster);
            e->first_cluster = 0;
        }
        else
        {
            u32 last, after;
            r = f32_chain_at(f, e->first_cluster, keep - 1, &last);
            if (r <= 0)
                return r < 0 ? r : FAT_ERR_IO;
            r = f32_fat_get(f, last, &after);
            if (r == 0 && after < EOC && valid_cluster(f, after))
            {
                r = f32_fat_set(f, last, 0x0FFFFFFF);
                if (r == 0)
                    r = f32_free_chain(f, after);
            }
        }
    }
    if (r)
        return r;
    e->size = new_size;
    f32_forget_cursor(f);
    r = update_entry(f, e, 1);
    return r ? r : f32_finish(f);
}

/* is `name` (ignoring case) already used in folder `dir` by an entry other than `skip` (index, or ~0)? */
