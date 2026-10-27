/* Shared by the files of the NTFS driver (kernel/fs/ntfs/): types, on-disk constants and the helpers every part needs. The public interface is include/ntfs.h. */
#ifndef HARIS_NTFS_INTERNAL_H
#define HARIS_NTFS_INTERNAL_H

/* NTFS file system. See include/ntfs.h. The first half reads; the second half (after ntfs_read) writes.
 *
 * NTFS in short: everything is a file, described by a 1 KiB "record" in the Master File Table ($MFT, itself a file;
 * record 5 is the root folder). A record holds attributes: $FILE_NAME (0x30), $DATA (0x80: the content),
 * $INDEX_ROOT (0x90) and $INDEX_ALLOCATION (0xA0) for folders, $ATTRIBUTE_LIST (0x20) when a file has too much for
 * one record. An attribute is "resident" (its content is right there) or "non-resident": then it holds data runs,
 * a compact list of (length, start cluster) pieces. On disk each 512-byte sector of a record or index block ends
 * with a copy of a sequence number instead of the real last two bytes ("fixups"); the real bytes sit in an array at
 * the start of the record. A folder is a B-tree of $FILE_NAME entries (name, size, record of the file): the root of
 * the tree is in $INDEX_ROOT, the rest in 4 KiB "INDX" blocks of $INDEX_ALLOCATION. To list a folder we do not follow
 * the tree: every block that is marked used in the index's $BITMAP holds some of the entries, so we read them all. */
#include "blk.h"
#include "ntfs.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;

#define NONE (~0ULL)
#define RES_MAX 4096 /* resident data we can hold */

enum
{
    AT_STANDARD_INFORMATION = 0x10,
    AT_ATTRIBUTE_LIST = 0x20,
    AT_FILE_NAME = 0x30,
    AT_VOLUME_NAME = 0x60,
    AT_DATA = 0x80,
    AT_INDEX_ROOT = 0x90,
    AT_INDEX_ALLOCATION = 0xA0,
    AT_BITMAP = 0xB0
};
#define AT_END 0xFFFFFFFFu /* ends the attributes of a record */

#define FL_COMPRESSED 0x0001
#define FL_ENCRYPTED 0x4000

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

/* One attribute's content, as found in the MFT. */
typedef struct
{
    int resident;
    u8 res[RES_MAX]; /* resident content */
    u32 res_len;
    NtRuns runs;     /* non-resident */
    u64 size;        /* real size in bytes */
    u64 init;        /* initialized size: bytes after it read as zero */
    u16 flags;       /* compressed / encrypted / sparse */
    int found;
} NtStream;
#define REF_MASK 0xFFFFFFFFFFFFULL /* the record number part of a 64-bit file reference */
#define MAX_NAMES 4 /* names of one file that are kept apart (the $FILE_NAME attributes) */
#define ROOT_BLK (~0ULL)
#define MAXD 12

typedef struct
{
    u32 rec;
    int dirty;           /* DREC has to be written */
    int has_ia;
    NtRuns ia;           /* $INDEX_ALLOCATION runs */
    u64 ia_size;
    int ib_res;          /* the index $BITMAP is resident (in DREC) */
    NtRuns ib;
    u64 ib_size;
} Idx;

typedef struct
{
    u8 *buf;
    u32 hdr; /* offset of the index header in buf: 16 in the root, 24 in a block */
    int root;
} Node;

typedef struct
{
    u64 blk; /* ROOT_BLK or the block number */
    u32 pos; /* bytes from the first entry to the entry we went through / insert before */
} Lvl;

static inline void wr16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static inline void wr32(u8 *p, u32 v)
{
    wr16(p, v);
    wr16(p + 2, v >> 16);
}

static inline void wr64(u8 *p, u64 v)
{
    wr32(p, (u32)v);
    wr32(p + 4, (u32)(v >> 32));
}

static inline u32 al8(u32 v) { return (v + 7) & ~7u; }

static inline u32 slen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

/* ---- ntfs_core.c ---- */
extern NtStream S_MAIN;
extern NtStream S_AUX;
extern NtStream S_FILE;
extern unsigned S_FILE_REC;
extern const Ntfs * S_FILE_NT;
int unsup(int line);
int unsup_why(int line, const char *why);
extern u8 REC[4096];
extern u8 ROOTBUF[RES_MAX];
extern u8 IDXBUF[16384];
extern u8 NAMEBUF[NTFS_NAME_MAX * 2];
int runs_read(Ntfs *nt, const NtRuns *rs, u64 offset, u8 *buf, u32 len);
int parse_runs(const u8 *p, const u8 *end, u64 start_vcn, NtRuns *rs);
int apply_fixups(u8 *b, u32 len);
int record_read(Ntfs *nt, u32 recno, u8 *buf);
const u8 *attr_first(const u8 *rec, u32 rec_size);
const u8 *attr_next(const u8 *rec, u32 rec_size, const u8 *a);
int attr_is(const u8 *a, u32 type, const char *name);
int load_stream(Ntfs *nt, u32 recno, u32 type, const char *name, NtStream *s);
int stream_read(Ntfs *nt, const NtStream *s, u64 offset, u8 *buf, u32 len);

/* ---- ntfs_wio.c ---- */
u64 now_ft(void);
void move_bytes(void *dst, const void *src, u32 n);
extern NtStream S_BM;
extern NtStream S_MB;
extern NtStream S_UP;
extern NtStream S_LOG;
extern u8 FREC[4096];
extern u8 DREC[4096];
extern u8 MREC[4096];
int runs_write(Ntfs *nt, const NtRuns *rs, u64 offset, const u8 *buf, u32 len);
int runs_zero(Ntfs *nt, const NtRuns *rs, u64 offset, u64 n);
int rec_store(Ntfs *nt, u32 recno, u8 *rec);
u32 rec_slot_seq(Ntfs *nt, u32 recno);
u8 *rec_find(u8 *rec, u32 rs, u32 type, const char *name);
u8 *rec_insert(u8 *rec, u32 rs, const u8 *attr);
void rec_remove(u8 *rec, u8 *a);
u32 build_res(u8 *out, u32 type, const char *name, const u8 *val, u32 vlen, u32 res_flags);
int res_set(u8 *rec, u32 rs, u8 *a, const u8 *val, u32 vlen);
u64 runs_clusters(const NtRuns *rs);
u32 build_nonres(u8 *out, u32 cap, u32 type, const char *name, const NtRuns *rs, u64 alloc, u64 real, u64 init);
int nonres_set(u8 *rec, u32 rs, u8 *a, const NtRuns *runs, u64 alloc, u64 real, u64 init);
int runs_append(NtRuns *rs, u64 lcn, u64 len);

/* ---- ntfs_bitmap.c ---- */
int bm_load(Ntfs *nt);
int bm_get(Ntfs *nt, u64 c);
int alloc_clusters(Ntfs *nt, u64 want, u64 hint, NtRuns *out);
int free_runs(Ntfs *nt, const NtRuns *rs, int from);
int mb_load(Ntfs *nt);
int mb_set(Ntfs *nt, u32 recno, int val);
int mb_find_free(Ntfs *nt, u32 *recno);

/* ---- ntfs_change.c ---- */
int key_cmp(Ntfs *nt, const u8 *a, const u8 *b);
int name_ok(const char *name);
u32 make_key(u8 *key, u64 parent_ref, const char *name, u64 t, u64 alloc, u64 real, u32 flags, u32 ns);
void touch_si(u8 *rec, u32 rs, u64 t);
int wr_begin(Ntfs *nt);
int wr_end(Ntfs *nt, int r);

/* ---- ntfs_index.c ---- */
extern Idx IX;
extern int IDX_WROTE;
extern Lvl PATH[MAXD + 2];
extern int PDEPTH;
extern u8 NB1[16384];
extern NtRuns NEWR;
u32 nd_ent0(const Node *n);
int idx_open(Ntfs *nt, u32 recno);
int idx_close(Ntfs *nt, int ok, u64 touch);
int node_load(Ntfs *nt, Node *n, u8 *buf, u64 blk);
int idx_locate(Ntfs *nt, const u8 *key, int *found);
int idx_add(Ntfs *nt, const u8 *ent, u32 elen);
int idx_remove(Ntfs *nt, const u8 *key);
int idx_update(Ntfs *nt, const u8 *key, const u8 *newkey);
int idx_is_empty(Ntfs *nt, int *empty);

/* ---- ntfs_rec.c ---- */
extern u8 KNEW[600];
extern u8 ENT[640];
extern u8 ATMP[1024];
extern u8 KOLD[MAX_NAMES][600];
extern u8 RESV[4096];
extern u8 RESN[4096];
extern NtRuns FREED;
extern NtRuns DSR;
u32 make_entry(u8 *out, u64 ref, const u8 *key, u32 klen);
int fn_attrs(u8 *rec, u32 rs, u8 **out, int max);
u8 *attr_val(u8 *a);
void names_refresh(Ntfs *nt, u8 *rec);
int names_index_sync(Ntfs *nt, u8 *rec);
int free_record_clusters(Ntfs *nt, u8 *rec);
int index_room(Ntfs *nt, int levels);
int parent_room(Ntfs *nt, u32 dir, const u8 *key);

/* ---- ntfs_write.c ---- */
int names_of_file(Ntfs *nt, u32 *parent, u32 *klens);

#endif
