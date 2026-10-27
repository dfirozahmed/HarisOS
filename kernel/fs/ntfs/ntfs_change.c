/* NTFS write side: names and collation, timestamps, begin / end of a change, volume checks. */
#include "ntfs_internal.h"

/* ---- names and collation -------------------------------------------------- */
static u32 up_ch(Ntfs *nt, u32 ch)
{
    if (ch < 0x80)
        return (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
    if (S_UP.found && !S_UP.resident && (u64)ch * 2 + 2 <= S_UP.size)
    {
        u8 b[2];
        if (stream_read(nt, &S_UP, (u64)ch * 2, b, 2) == 2)
            return rd16(b);
    }
    return ch;
}

/* order of two $FILE_NAME keys: the names compared letter by letter in upper case (the $UpCase table) */
int key_cmp(Ntfs *nt, const u8 *a, const u8 *b)
{
    u32 la = a[64], lb = b[64], n = la < lb ? la : lb;
    for (u32 i = 0; i < n; i++)
    {
        u32 x = up_ch(nt, rd16(a + 66 + 2 * i)), y = up_ch(nt, rd16(b + 66 + 2 * i));
        if (x != y)
            return x < y ? -1 : 1;
    }
    return la == lb ? 0 : la < lb ? -1 : 1;
}

/* is `name` a name we create? 0 = yes, else NTFS_ERR_BADNAME */
int name_ok(const char *name)
{
    if (!name)
        return NTFS_ERR_BADNAME;
    u32 n = slen(name);
    if (n == 0 || n > 255 || name[n - 1] == '.' || name[n - 1] == ' ')
        return NTFS_ERR_BADNAME;
    for (u32 i = 0; i < n; i++)
    {
        u8 c = (u8)name[i];
        if (c < 0x20 || c >= 0x7F || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' ||
            c == '|')
            return NTFS_ERR_BADNAME;
    }
    return 0;
}

/* ---- timestamps and the $FILE_NAME key ------------------------------------- */
/* Fill a $FILE_NAME (the key of a folder index entry, the same bytes as the attribute): parent, times, sizes,
 * flags, name. Returns its length. */
u32 make_key(u8 *key, u64 parent_ref, const char *name, u64 t, u64 alloc, u64 real, u32 flags, u32 ns)
{
    u32 nl = slen(name);
    zero(key, 66 + nl * 2);
    wr64(key, parent_ref);
    for (int i = 0; i < 4; i++)
        wr64(key + 8 + 8 * i, t);
    wr64(key + 40, alloc);
    wr64(key + 48, real);
    wr32(key + 56, flags);
    key[64] = (u8)nl;
    key[65] = (u8)ns;
    for (u32 i = 0; i < nl; i++)
        wr16(key + 66 + 2 * i, (u8)name[i]);
    return 66 + nl * 2;
}

/* set the modified / MFT-changed / accessed times in $STANDARD_INFORMATION */
void touch_si(u8 *rec, u32 rs, u64 t)
{
    u8 *si = rec_find(rec, rs, AT_STANDARD_INFORMATION, 0);
    if (si && si[8] == 0 && rd32(si + 16) >= 32)
    {
        u8 *v = si + rd16(si + 20);
        wr64(v + 8, t);
        wr64(v + 16, t);
        wr64(v + 24, t);
    }
}

/* ---- begin / end of a change ------------------------------------------------- */
/* $Volume's flags (the dirty bit and friends) */
static int vol_flags(Ntfs *nt, u32 set, u32 clear, u32 *old)
{
    if (record_read(nt, 3, MREC))
        return -1;
    u8 *a = rec_find(MREC, nt->rec_size, 0x70, 0);
    if (!a || a[8] != 0 || rd32(a + 16) < 12 || rd16(a + 20) + 12u > rd32(a + 4))
        return -1;
    u8 *f = a + rd16(a + 20) + 10;
    u32 cur = rd16(f);
    if (old)
        *old = cur;
    u32 now = (cur | set) & ~clear;
    if (now == cur)
        return 0;
    wr16(f, now);
    return rec_store(nt, 3, MREC);
}

#define VOL_DIRTY 0x0001u
#define VOL_RESIZE_LOG 0x0002u
#define VOL_UPGRADE 0x0004u
#define VOL_DELETE_USN 0x0010u

/* One restart page of $LogFile at byte `pos`: 1 if it is a good one (then *lsn is its current LSN, *clean says if it
 * has the "volume is clean" flag), else 0 (all 0xFF = empty, or damaged). */
static int log_page(Ntfs *nt, u64 pos, u64 *lsn, int *clean)
{
    u8 p[0x20], r[16];
    if (stream_read(nt, &S_LOG, pos, p, sizeof p) != (int)sizeof p)
        return 0;
    if (!(p[0] == 'R' && p[1] == 'S' && p[2] == 'T' && p[3] == 'R'))
        return 0;
    u32 ra = rd16(p + 0x18);
    if (ra < 0x20 || stream_read(nt, &S_LOG, pos + ra, r, sizeof r) != (int)sizeof r)
        return 0;
    *lsn = rd64(r);
    *clean = (rd16(r + 14) & 0x0002) != 0; /* RESTART_VOLUME_IS_CLEAN */
    return 1;
}

/* Is the journal clean? Windows writes the restart area alternately to two pages, so the one with the higher LSN is
 * the current one; the other may be older and still say "not clean". An empty log (all 0xFF) is clean. */
static int log_clean(Ntfs *nt)
{
    if (load_stream(nt, 2, AT_DATA, 0, &S_LOG) != 0 || !S_LOG.found)
        return 0;
    if (S_LOG.size == 0)
        return 1; /* nothing in it (tests, a fresh volume) */
    if (S_LOG.flags & (FL_COMPRESSED | FL_ENCRYPTED))
        return 0;
    u8 h[24];
    if (stream_read(nt, &S_LOG, 0, h, sizeof h) != (int)sizeof h)
        return 0;
    u32 sps = (h[0] == 'R' && h[1] == 'S' && h[2] == 'T' && h[3] == 'R') ? rd32(h + 16) : 4096; /* system page size */
    if (sps < 512 || sps > 65536 || S_LOG.size < 2ULL * sps)
        return 0;
    u64 lsn0 = 0, lsn1 = 0;
    int c0 = 0, c1 = 0;
    int v0 = log_page(nt, 0, &lsn0, &c0), v1 = log_page(nt, sps, &lsn1, &c1);
    if (v0 && v1)
        return lsn1 > lsn0 ? c1 : c0;
    if (v0 || v1)
        return v0 ? c0 : c1;
    u8 a[4], b[4]; /* no restart page at all: clean only if the log was reset (all 0xFF) */
    return stream_read(nt, &S_LOG, 0, a, 4) == 4 && stream_read(nt, &S_LOG, sps, b, 4) == 4 && rd32(a) == 0xFFFFFFFFu &&
           rd32(b) == 0xFFFFFFFFu;
}

/* ntfs_is_clean: is this volume safe to write to? Also looks for a hibernated Windows (Fast Startup): its
 * hiberfil.sys starts with "hibr" / "wake" and the volume looks clean while Windows still holds changes in RAM. */
static int hiber_cb(const char *name, const NtEntry *e, void *user)
{
    static const char WANT[] = "hiberfil.sys";
    for (int i = 0; i < (int)sizeof WANT; i++)
        if ((name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]) != WANT[i])
            return 0;
    *(NtEntry *)user = *e;
    return 1;
}

/* ntfs_is_windows: a folder "Windows" in the root, together with "Users" or "Program Files" */
typedef struct
{
    int windows, others;
} WinFind;

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a >= 'A' && *a <= 'Z' ? *a + 32 : *a) != (*b >= 'A' && *b <= 'Z' ? *b + 32 : *b))
            return 0;
    return *a == *b;
}

static int win_cb(const char *name, const NtEntry *e, void *user)
{
    WinFind *w = (WinFind *)user;
    if (e->is_dir)
    {
        if (ieq(name, "Windows"))
            w->windows = 1;
        else if (ieq(name, "Users") || ieq(name, "Program Files"))
            w->others = 1;
    }
    return w->windows && w->others;
}

int ntfs_is_windows(Ntfs *nt)
{
    WinFind w = {0, 0};
    NtEntry root;
    ntfs_root(&root);
    ntfs_list(nt, &root, win_cb, &w);
    return w.windows && w.others;
}

int ntfs_is_clean(Ntfs *nt)
{
    u32 fl;
    if (vol_flags(nt, 0, 0, &fl)) /* (no change asked for: nothing is written) */
        return 0;
    if ((fl & (VOL_DIRTY | VOL_RESIZE_LOG | VOL_UPGRADE | VOL_DELETE_USN)) || !log_clean(nt))
        return 0;
    NtEntry root, hf;
    ntfs_root(&root);
    zero(&hf, sizeof hf);
    if (ntfs_list(nt, &root, hiber_cb, &hf) > 0 && hf.recno)
    {
        u8 m[4];
        if (ntfs_read(nt, &hf, 0, m, 4) == 4)
        {
            u32 magic = (u32)(m[0] | 0x20) | (u32)(m[1] | 0x20) << 8 | (u32)(m[2] | 0x20) << 16 | (u32)(m[3] | 0x20) << 24;
            if (magic == 0x72626968u /* "hibr" */ || magic == 0x656b6177u /* "wake" */)
                return 0;
        }
    }
    return 1;
}

int wr_begin(Ntfs *nt)
{
    BlkDev *d = blk_get(nt->blk);
    if (!d || d->locked) /* (a partition mirrors the lock of its disk) */
        return NTFS_ERR_RO;
    if (nt->wstate < 0)
        return nt->wstate;
    S_FILE_NT = 0; /* what was cached for reading may change now */
    if (nt->wstate == 0)
    {
        u32 fl;
        if (vol_flags(nt, 0, 0, &fl))
            return nt->wstate = unsup(__LINE__);
        if (fl & (VOL_DIRTY | VOL_RESIZE_LOG | VOL_UPGRADE | VOL_DELETE_USN))
            return nt->wstate = NTFS_ERR_DIRTY;
        if (!log_clean(nt))
            return nt->wstate = NTFS_ERR_DIRTY;
        nt->wstate = 1;
    }
    if (bm_load(nt) || mb_load(nt))
        return unsup(__LINE__);
    S_UP.found = 0;
    load_stream(nt, 10, AT_DATA, 0, &S_UP); /* $UpCase; without it only ASCII letters fold */
    if (vol_flags(nt, VOL_DIRTY, 0, 0))
    {
        nt->poisoned = 0; /* the very first write failed: nothing changed (a read-only device) */
        return NTFS_ERR_RO;
    }
    return 0;
}

/* finish: r is the result of the change. The dirty bit goes off only if every write worked. */
int wr_end(Ntfs *nt, int r)
{
    S_FILE_NT = 0;
    if (nt->poisoned)
    {
        nt->wstate = NTFS_ERR_DIRTY; /* leave the dirty bit on: chkdsk has to look at this */
        blk_flush(nt->blk);
        return r >= 0 ? NTFS_ERR_IO : r;
    }
    if (blk_flush(nt->blk) != 0 || vol_flags(nt, 0, VOL_DIRTY, 0) || blk_flush(nt->blk) != 0)
    {
        nt->wstate = NTFS_ERR_DIRTY;
        return r >= 0 ? NTFS_ERR_IO : r;
    }
    return r;
}
