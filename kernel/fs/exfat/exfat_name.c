/* exFAT write side: names and the entries of a folder. */
#include "exfat_internal.h"

/* ---- names ---- */
int exf_name_valid(const char *name)
{
    u32 n = 0;
    while (name[n])
        n++;
    if (n == 0 || n > 255 || name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.')
        return 0;
    for (u32 i = 0; i < n; i++)
    {
        u8 c = (u8)name[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
            c == '\\' || c == '|')
            return 0;
    }
    return !(name[0] == '.' && (n == 1 || (n == 2 && name[1] == '.')));
}

static int same_name(const char *a, const char *b)
{
    while (*a && ((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a) == ((*b >= 'A' && *b <= 'Z') ? *b + 32 : *b))
        a++, b++;
    return *a == *b;
}

typedef struct
{
    const char *name;
    u32 skip;
    int found;
} NameCheck;

static int name_cb(const char *name, const ExEntry *e, void *user)
{
    NameCheck *c = (NameCheck *)user;
    if (e->set_index != c->skip && same_name(name, c->name))
        c->found = 1;
    return c->found;
}

int exf_name_exists(ExFat *f, const ExEntry *dir, const char *name, u32 skip)
{
    NameCheck c = {name, skip, 0};
    int r = exfat_list(f, dir, name_cb, &c);
    return r < 0 ? EXFAT_ERR_IO : c.found;
}

/* ---- entries of a folder ---- */
static int put_entry(ExFat *f, const ExEntry *dir, u32 index, const u8 e[32])
{
    u64 byte = (u64)index * 32;
    u32 csize = f->spc * f->ss, cl;
    int r = exf_cluster_at(f, dir->first_cluster, dir->nofat, (u32)(byte / csize), &cl);
    if (r <= 0)
        return EXFAT_ERR_IO;
    u32 pos = (u32)(byte % csize);
    u64 lba = cluster_lba(f, cl) + pos / f->ss;
    if (exf_load_sec(f, lba))
        return EXFAT_ERR_IO;
    copy(f->sec + pos % f->ss, e, 32);
    return exf_wr_sector(f, lba, f->sec);
}

/* rewrite the stream fields (first cluster, sizes, NoFatChain) and modify time of an entry set, and its checksum */
int exf_update_set(ExFat *f, const ExEntry *container, const ExEntry *e)
{
    u8 set[32 * 20];
    u32 total = (u32)e->secondary + 1;
    if (total < 2 || total > 20)
        return EXFAT_ERR_ARG;
    for (u32 i = 0; i < total; i++)
    {
        int r = exf_get_entry(f, container, e->set_index + i, set + 32 * i);
        if (r <= 0)
            return EXFAT_ERR_IO;
    }
    u32 stamp;
    exf_now(&stamp);
    exf_wr32(set + 12, stamp);
    set[1 * 32 + 1] = (u8)(1 | (e->nofat ? 2 : 0));
    exf_wr64(set + 32 + 8, e->valid);
    exf_wr32(set + 32 + 20, e->first_cluster);
    exf_wr64(set + 32 + 24, e->size);
    exf_wr16(set + 2, exf_set_checksum(set, total));
    int r = put_entry(f, container, e->set_index, set);
    if (r)
        return r;
    return put_entry(f, container, e->set_index + 1, set + 32);
}

static u32 count_clusters(ExFat *f, const ExEntry *e)
{
    if (e->is_root)
    {
        u32 n = 0, cl;
        while (exf_cluster_at(f, e->first_cluster, 0, n, &cl) > 0 && n < f->clusters)
            n++;
        return n;
    }
    return exf_clusters_of(f, e->size);
}

/* a folder gets one more (empty) cluster; its own entry set (in dir_parent) learns the new length */
static int dir_grow(ExFat *f, ExEntry *dir, const ExEntry *dir_parent)
{
    u32 first = dir->first_cluster, n = count_clusters(f, dir);
    u8 nofat = dir->nofat;
    int r = exf_append_cluster(f, &first, &nofat, &n, 1);
    if (r)
        return r;
    dir->first_cluster = first;
    dir->nofat = nofat;
    if (!dir->is_root)
    {
        u32 csize = f->spc * f->ss;
        if ((u64)dir->size + csize > 0xFFFFFFFFull)
            return EXFAT_ERR_FULL;
        dir->size += csize;
        dir->valid = dir->size;
        if (!dir_parent)
            return EXFAT_ERR_ARG;
        r = exf_update_set(f, dir_parent, dir);
        if (r)
            return r;
    }
    return 0;
}

#define MAX_DIR_ENTRIES 262144u

/* `need` consecutive unused entries in the folder (it grows if it must). Returns the first index. */
int exf_find_free_run(ExFat *f, ExEntry *dir, const ExEntry *dir_parent, u32 need, u32 *start)
{
    u32 run = 0, first = 0, index = 0;
    for (;;)
    {
        u8 e[32];
        int r = exf_get_entry(f, dir, index, e);
        if (r < 0)
            return EXFAT_ERR_IO;
        if (r == 0)
            break;
        if (!(e[0] & 0x80)) /* unused (0x00 and the deleted ones) */
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
    u32 per = f->spc * f->ss / 32;
    while (run < need)
    {
        if (index >= MAX_DIR_ENTRIES)
            return EXFAT_ERR_FULL;
        int r = dir_grow(f, dir, dir_parent);
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

/* build the entry set of a new entry; returns the number of entries */
u32 exf_make_set(ExFat *f, u8 *set, const char *name, u32 attr, u32 first, u32 size, u32 valid, int nofat, const u8 *times)
{
    u32 nlen = 0;
    while (name[nlen])
        nlen++;
    u32 names = (nlen + 14) / 15, total = 2 + names;
    zero(set, total * 32);
    set[0] = 0x85;
    set[1] = (u8)(total - 1);
    exf_wr16(set + 4, attr);
    if (times)
        copy(set + 8, times, 24);
    else
    {
        u32 stamp;
        exf_now(&stamp);
        exf_wr32(set + 8, stamp);
        exf_wr32(set + 12, stamp);
        exf_wr32(set + 16, stamp);
    }
    u8 *st = set + 32;
    st[0] = 0xC0;
    st[1] = (u8)(1 | (nofat ? 2 : 0));
    st[3] = (u8)nlen;
    exf_wr16(st + 4, exf_name_hash(f, name));
    exf_wr64(st + 8, valid);
    exf_wr32(st + 20, first);
    exf_wr64(st + 24, size);
    for (u32 n = 0; n < names; n++)
    {
        u8 *ne = set + 32 * (2 + n);
        ne[0] = 0xC1;
        for (u32 c = 0; c < 15; c++)
        {
            u32 idx = n * 15 + c;
            exf_wr16(ne + 2 + 2 * c, idx < nlen ? (u8)name[idx] : 0);
        }
    }
    exf_wr16(set + 2, exf_set_checksum(set, total));
    return total;
}

int exf_write_set(ExFat *f, const ExEntry *dir, u32 index, const u8 *set, u32 total)
{
    for (u32 i = 0; i < total; i++)
    {
        int r = put_entry(f, dir, index + i, set + 32 * i);
        if (r)
            return r;
    }
    return 0;
}

/* mark an entry set as deleted: the "in use" bit of each entry goes */
int exf_kill_set(ExFat *f, const ExEntry *dir, const ExEntry *e)
{
    for (u32 i = 0; i <= e->secondary; i++)
    {
        u8 b[32];
        int r = exf_get_entry(f, dir, e->set_index + i, b);
        if (r <= 0)
            return EXFAT_ERR_IO;
        b[0] &= 0x7F;
        r = put_entry(f, dir, e->set_index + i, b);
        if (r)
            return r;
    }
    return 0;
}
