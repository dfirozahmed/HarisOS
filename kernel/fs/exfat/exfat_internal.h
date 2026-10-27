/* Shared by the files of the exFAT driver (kernel/fs/exfat/): types, constants and the small helpers every part needs. The public interface is include/exfat.h. */
#ifndef HARIS_EXFAT_INTERNAL_H
#define HARIS_EXFAT_INTERNAL_H

#include "blk.h"
#include "exfat.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define EOC 0xFFFFFFF8u
#define NONE (~0ULL)

static inline u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static inline u64 rd64(const u8 *p) { return (u64)rd32(p) | (u64)rd32(p + 4) << 32; }

static inline void zero(void *p, unsigned long n)
{
    volatile u8 *b = (volatile u8 *)p;
    for (unsigned long i = 0; i < n; i++)
        b[i] = 0;
}

static inline void copy(void *dst, const void *src, unsigned long n)
{
    volatile u8 *d = (volatile u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)src;
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
}

static inline u32 clamp32(u64 v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)v; }

static inline int valid_cluster(const ExFat *f, u32 cl) { return cl >= 2 && cl < f->clusters + 2; }
static inline u64 cluster_lba(const ExFat *f, u32 cl) { return f->heap_lba + (u64)(cl - 2) * f->spc; }

/* ---- exfat_core.c ---- */
int exf_load_sec(ExFat *f, u64 lba);
int exf_fat_flush(ExFat *f);
int exf_fat_get(ExFat *f, u32 cl, u32 *out);
int exf_cluster_at(ExFat *f, u32 first, int nofat, u32 index, u32 *out);

/* ---- exfat_read.c ---- */
int exf_cluster_read(ExFat *f, u32 cl, u32 pos, u8 *buf, u32 n);

/* ---- exfat_dir.c ---- */
int exf_get_entry(ExFat *f, const ExEntry *dir, u32 index, u8 out[32]);

/* ---- exfat_fat.c ---- */
extern ExFatClockFn exf_CLOCK;
void exf_wr16(u8 *p, u32 v);
void exf_wr32(u8 *p, u32 v);
void exf_wr64(u8 *p, u64 v);
int exf_writable(const ExFat *f);
int exf_wr_sector(ExFat *f, u64 lba, const void *buf);
int exf_fat_flush(ExFat *f);
int exf_fat_put(ExFat *f, u32 cl, u32 value);
int exf_bm_get(ExFat *f, u32 cl, int *used);
int exf_bm_put(ExFat *f, u32 cl, int used);
int exf_finish(ExFat *f);
int exf_alloc_cluster(ExFat *f, u32 hint, u32 *out);
void exf_forget_cursor(ExFat *f);

/* ---- exfat_cluster.c ---- */
u32 exf_clusters_of(const ExFat *f, u32 size);
int exf_append_cluster(ExFat *f, u32 *first, u8 *nofat, u32 *n, int zero_it);
int exf_trim_clusters(ExFat *f, u32 *first, u8 nofat, u32 n, u32 keep);
u16 exf_name_hash(ExFat *f, const char *name);
u16 exf_set_checksum(const u8 *set, u32 n);
void exf_now(u32 *stamp);

/* ---- exfat_name.c ---- */
int exf_name_valid(const char *name);
int exf_name_exists(ExFat *f, const ExEntry *dir, const char *name, u32 skip);
int exf_update_set(ExFat *f, const ExEntry *container, const ExEntry *e);
int exf_find_free_run(ExFat *f, ExEntry *dir, const ExEntry *dir_parent, u32 need, u32 *start);
u32 exf_make_set(ExFat *f, u8 *set, const char *name, u32 attr, u32 first, u32 size, u32 valid, int nofat, const u8 *times);
int exf_write_set(ExFat *f, const ExEntry *dir, u32 index, const u8 *set, u32 total);
int exf_kill_set(ExFat *f, const ExEntry *dir, const ExEntry *e);

#endif
