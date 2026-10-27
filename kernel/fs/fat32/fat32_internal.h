/* Shared by the files of the FAT32 driver (kernel/fs/fat32/): types, constants and the small helpers every part needs. The public interface is include/fat32.h. */
#ifndef HARIS_FAT32_INTERNAL_H
#define HARIS_FAT32_INTERNAL_H

#include "blk.h"
#include "fat32.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define EOC 0x0FFFFFF8u /* values >= this end a chain */
#define FAT_MASK 0x0FFFFFFFu
#define MAX_DIR_ENTRIES 65536u

static inline u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static inline void wr16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
static inline void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

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

static inline int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static inline unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}

static inline int same_name(const char *a, const char *b)
{
    while (*a && lower((u8)*a) == lower((u8)*b))
        a++, b++;
    return *a == *b;
}
static inline u64 cluster_lba(const Fat *f, u32 cl) { return f->data_lba + (u64)(cl - 2) * f->spc; }
static inline int valid_cluster(const Fat *f, u32 cl) { return cl >= 2 && cl < f->clusters + 2; }

/* ---- types, constants and variables used by more than one file ---- */
static const u8 LFN_OFFS[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

/* ---- fat32_core.c ---- */
extern FatClockFn f32_CLOCK;
int f32_writable(const Fat *f);
int f32_rd_sectors(Fat *f, u64 lba, unsigned n, void *buf);
int f32_wr_sectors(Fat *f, u64 lba, unsigned n, const void *buf);

/* ---- fat32_fat.c ---- */
int f32_fat_get(Fat *f, u32 cl, u32 *out);
int f32_fat_set(Fat *f, u32 cl, u32 value);
int f32_chain_at(Fat *f, u32 first, u32 index, u32 *out);
void f32_forget_cursor(Fat *f);
int f32_alloc_cluster(Fat *f, u32 *out);
int f32_free_chain(Fat *f, u32 c);
int f32_zero_cluster(Fat *f, u32 cl);
int f32_finish(Fat *f);

/* ---- fat32_mount.c ---- */
u32 f32_per_cluster(const Fat *f);
int f32_slot_read(Fat *f, u32 dir, u32 index, u8 out[32]);
int f32_slot_write(Fat *f, u32 dir, u32 index, const u8 in[32]);

/* ---- fat32_name.c ---- */
u8 f32_sfn_checksum(const u8 *n);
int f32_name_valid(const char *name);
int f32_make_sfn(const char *name, u8 sfn[11], u8 *ntres, unsigned tail);
void f32_sfn_to_name(const u8 *e, char *out);

/* ---- fat32_data.c ---- */
int f32_chain_io(Fat *f, u32 first, u32 offset, u8 *buf, u32 len, int write);

/* ---- fat32_entry.c ---- */
void f32_now(unsigned *date, unsigned *time);

#endif
