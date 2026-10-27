/* FAT32: create, delete, rename, move. */
#include "fat32_internal.h"

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
            if (f32_rd_sectors(f, cluster_lba(f, cl) + s, 1, f->sec))
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
        int r = f32_fat_get(f, cl, &next);
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
        int r = f32_slot_read(f, dir, index, e);
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
    u32 per = f32_per_cluster(f);
    while (run < need)
    {
        if (index >= MAX_DIR_ENTRIES)
            return FAT_ERR_FULL;
        u32 c, tail;
        int r = f32_alloc_cluster(f, &c);
        if (r)
            return r;
        r = f32_zero_cluster(f, c);
        if (r)
            return r;
        r = f32_chain_at(f, dir, index / per - 1, &tail);
        if (r <= 0)
            return r < 0 ? r : FAT_ERR_IO;
        r = f32_fat_set(f, tail, c);
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
    int need_lfn = f32_make_sfn(name, sfn, &ntres, 0);
    if (need_lfn)
    {
        for (unsigned tail = 1;; tail++)
        {
            if (tail > 999999)
                return FAT_ERR_FULL;
            f32_make_sfn(name, sfn, &ntres, tail);
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
                f32_make_sfn(name, sfn, &ntres, tail);
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

    u8 sum = f32_sfn_checksum(sfn);
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
        r = f32_slot_write(f, dir, start + (nlfn - k), e);
        if (r)
            return r;
    }
    u8 e[32];
    copy(e, proto, 32);
    for (int i = 0; i < 11; i++)
        e[i] = sfn[i];
    e[12] = ntres;
    r = f32_slot_write(f, dir, start + nlfn, e);
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
    if (!f32_writable(f))
        return FAT_ERR_RO;
    if (!valid_cluster(f, dir))
        return FAT_ERR_ARG;
    if (!f32_name_valid(name) || (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))))
        return FAT_ERR_BADNAME;
    int r = name_exists(f, dir, name, 0xFFFFFFFFu);
    if (r < 0)
        return r;
    if (r)
        return FAT_ERR_EXISTS;

    u8 proto[32];
    zero(proto, 32);
    unsigned d, t;
    f32_now(&d, &t);
    proto[11] = is_dir ? FAT_ATTR_DIR : FAT_ATTR_ARCHIVE;
    wr16(proto + 14, t);
    wr16(proto + 16, d);
    wr16(proto + 18, d);
    wr16(proto + 22, t);
    wr16(proto + 24, d);
    u32 cluster = 0;
    if (is_dir)
    {
        r = f32_alloc_cluster(f, &cluster);
        if (r)
            return r;
        r = f32_zero_cluster(f, cluster);
        if (r)
            return r;
        u8 dots[32];
        copy(dots, proto, 32); /* "." and ".." share the attribute and times */
        for (int i = 0; i < 11; i++)
            dots[i] = ' ';
        dots[0] = '.';
        wr16(dots + 20, cluster >> 16);
        wr16(dots + 26, cluster & 0xFFFF);
        r = f32_slot_write(f, cluster, 0, dots);
        if (r)
            return r;
        dots[1] = '.';
        u32 parent = dir == f->root_cluster ? 0 : dir; /* ".." of a folder in the root points to 0 */
        wr16(dots + 20, parent >> 16);
        wr16(dots + 26, parent & 0xFFFF);
        r = f32_slot_write(f, cluster, 1, dots);
        if (r)
            return r;
        wr16(proto + 20, cluster >> 16);
        wr16(proto + 26, cluster & 0xFFFF);
    }
    r = place_entry(f, dir, name, proto, 0xFFFFFFFFu, out);
    if (r)
    {
        if (cluster)
            f32_free_chain(f, cluster);
        f32_finish(f);
        return r;
    }
    return f32_finish(f);
}

/* mark the entries of `e` (long-name pieces and short entry) as deleted */
static int kill_entries(Fat *f, const FatEntry *e)
{
    for (u32 i = e->index - e->nlfn; i <= e->index; i++)
    {
        u8 raw[32];
        int r = f32_slot_read(f, e->dir_cluster, i, raw);
        if (r <= 0)
            return r < 0 ? r : FAT_ERR_IO;
        raw[0] = 0xE5;
        r = f32_slot_write(f, e->dir_cluster, i, raw);
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
    if (!f32_writable(f))
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
        r = f32_free_chain(f, e->first_cluster);
        if (r)
            return r;
    }
    return f32_finish(f);
}

int fat_rename(Fat *f, FatEntry *e, const char *new_name)
{
    if (!f32_writable(f))
        return FAT_ERR_RO;
    if (!f32_name_valid(new_name) || (new_name[0] == '.' && (new_name[1] == 0 || (new_name[1] == '.' && new_name[2] == 0))))
        return FAT_ERR_BADNAME;
    int r = name_exists(f, e->dir_cluster, new_name, e->index);
    if (r < 0)
        return r;
    if (r)
        return FAT_ERR_EXISTS;
    u8 proto[32];
    r = f32_slot_read(f, e->dir_cluster, e->index, proto);
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
    return f32_finish(f);
}

int fat_move(Fat *f, FatEntry *e, const char *name, unsigned new_dir)
{
    if (!f32_writable(f))
        return FAT_ERR_RO;
    if (!valid_cluster(f, new_dir))
        return FAT_ERR_ARG;
    if (new_dir == e->dir_cluster)
        return 0;
    if (!f32_name_valid(name))
        return FAT_ERR_BADNAME;
    int r = name_exists(f, new_dir, name, 0xFFFFFFFFu);
    if (r < 0)
        return r;
    if (r)
        return FAT_ERR_EXISTS;
    u8 proto[32];
    r = f32_slot_read(f, e->dir_cluster, e->index, proto);
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
        r = f32_slot_read(f, e->first_cluster, 1, dots);
        if (r <= 0)
            return r < 0 ? r : FAT_ERR_IO;
        u32 parent = new_dir == f->root_cluster ? 0 : new_dir;
        wr16(dots + 20, parent >> 16);
        wr16(dots + 26, parent & 0xFFFF);
        r = f32_slot_write(f, e->first_cluster, 1, dots);
        if (r)
            return r;
    }
    *e = neu;
    return f32_finish(f);
}
