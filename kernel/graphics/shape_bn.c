/* HarisOS Bengali shaping: see include/shape_bn.h.
 *
 * Part 1 (top) is a small OpenType layout engine: it reads the font's GSUB / GPOS / GDEF tables as they are in the
 * .ttf, so there is no private table format. The tables are our own embedded constants, so reads are not
 * bounds-checked; everything that depends on the text is (buffer capacity, input counts, recursion depth).
 * Part 2 (bottom) is the Bengali part: syllables, reordering, and the order in which features run. */
#include "shape_bn.h"
#include "font_bn.h"

/* ---------------------------------------------------------------- big-endian reads */

static unsigned u16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static int s16(const unsigned char *p)
{
    unsigned v = u16(p);
    return v & 0x8000u ? (int)v - 0x10000 : (int)v;
}
static unsigned u32(const unsigned char *p) { return (u16(p) << 16) | u16(p + 2); }

/* ---------------------------------------------------------------- coverage and class tables */

/* Index of gid in a Coverage table, -1 if it is not covered. */
static int cov_index(const unsigned char *c, unsigned gid)
{
    unsigned fmt = u16(c), n = u16(c + 2);
    int lo = 0, hi = (int)n - 1;
    if (fmt == 1)
    {
        while (lo <= hi)
        {
            int mid = (lo + hi) / 2;
            unsigned g = u16(c + 4 + 2 * mid);
            if (g == gid)
                return mid;
            if (g < gid)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }
    else if (fmt == 2)
    {
        while (lo <= hi)
        {
            int mid = (lo + hi) / 2;
            const unsigned char *r = c + 4 + 6 * mid;
            if (gid < u16(r))
                hi = mid - 1;
            else if (gid > u16(r + 2))
                lo = mid + 1;
            else
                return (int)(u16(r + 4) + gid - u16(r));
        }
    }
    return -1;
}

/* Class of gid in a ClassDef table (0 = none, or no table). */
static unsigned class_of(const unsigned char *cd, unsigned gid)
{
    if (!cd)
        return 0;
    unsigned fmt = u16(cd);
    if (fmt == 1)
    {
        unsigned start = u16(cd + 2), n = u16(cd + 4);
        return gid >= start && gid < start + n ? u16(cd + 6 + 2 * (gid - start)) : 0;
    }
    if (fmt == 2)
    {
        int lo = 0, hi = (int)u16(cd + 2) - 1;
        while (lo <= hi)
        {
            int mid = (lo + hi) / 2;
            const unsigned char *r = cd + 4 + 6 * mid;
            if (gid < u16(r))
                hi = mid - 1;
            else if (gid > u16(r + 2))
                lo = mid + 1;
            else
                return u16(r + 4);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- GDEF */

static const unsigned char *gdef_sub(unsigned off_at)
{
    unsigned off = u16(bn_gdef + off_at);
    return off ? bn_gdef + off : 0;
}

/* 0 = unclassified, 1 = base, 2 = ligature, 3 = mark, 4 = component */
static unsigned glyph_class(unsigned gid) { return class_of(gdef_sub(4), gid); }
static unsigned mark_attach_class(unsigned gid) { return class_of(gdef_sub(10), gid); }

static int in_mark_set(unsigned set, unsigned gid)
{
    if (u16(bn_gdef + 2) < 2) /* minor version: sets exist from 1.2 */
        return 0;
    const unsigned char *sets = gdef_sub(12);
    if (!sets || set >= u16(sets + 2))
        return 0;
    return cov_index(sets + u32(sets + 4 + 4 * set), gid) >= 0;
}

/* ---------------------------------------------------------------- glyph buffer */

#define CAP 96   /* glyphs in one syllable, with room for substitutions that make more */
#define MAXIN 16 /* longest matched input sequence */

/* Which lookups may touch a glyph (see the stage table at the bottom). */
#define M_RPHF 0x0001u
#define M_BLWF 0x0002u
#define M_HALF 0x0004u
#define M_PSTF 0x0008u
#define M_INIT 0x0010u
#define M_ALL 0x8000u

#define F_BASE 0x01 /* this glyph is (or contains) the base consonant of the syllable */

typedef struct
{
    unsigned short gid, mask;
    unsigned char cls, flags;
    short attach; /* index of the glyph this mark is attached to, -1 = none */
    int xoff, yoff, xadv;  /* GPOS adjustments, font design units */
    int ax, ay;            /* anchor difference to the attached glyph */
} G;

typedef struct
{
    const unsigned char *lookups; /* LookupList of GSUB or GPOS */
    int gpos;
    unsigned lookup_mask; /* glyphs without one of these bits are left alone by the lookup being run */
    int depth;            /* nesting of lookups called from contexts */
    int len;
    G buf[CAP];
} Ctx;

typedef struct
{
    unsigned type, flag, nsub, mset;
    const unsigned char *lk;
} Lk;

static void set_gid(G *g, unsigned gid)
{
    g->gid = (unsigned short)gid;
    g->cls = (unsigned char)glyph_class(gid);
}

static int get_lk(const Ctx *c, unsigned li, Lk *o)
{
    if (li >= u16(c->lookups))
        return 0;
    o->lk = c->lookups + u16(c->lookups + 2 + 2 * li);
    o->type = u16(o->lk);
    o->flag = u16(o->lk + 2);
    o->nsub = u16(o->lk + 4);
    o->mset = (o->flag & 0x10u) ? u16(o->lk + 6 + 2 * o->nsub) : 0;
    return 1;
}

/* LookupFlag: ignore bases (2), ligatures (4), marks (8), marks outside a filtering set (0x10), marks of the
 * wrong attachment class (high byte). */
static int ignorable(const Lk *lk, const G *g)
{
    unsigned flag = lk->flag;
    if (g->cls == 1)
        return (flag & 2u) != 0;
    if (g->cls == 2)
        return (flag & 4u) != 0;
    if (g->cls == 3)
    {
        if (flag & 8u)
            return 1;
        if ((flag & 0x10u) && !in_mark_set(lk->mset, g->gid))
            return 1;
        if ((flag & 0xFF00u) && mark_attach_class(g->gid) != (flag >> 8))
            return 1;
    }
    return 0;
}

static int next_glyph(const Ctx *c, const Lk *lk, int i)
{
    for (i++; i < c->len; i++)
        if (!ignorable(lk, &c->buf[i]))
            return i;
    return -1;
}

static int prev_glyph(const Ctx *c, const Lk *lk, int i)
{
    for (i--; i >= 0; i--)
        if (!ignorable(lk, &c->buf[i]))
            return i;
    return -1;
}

/* ---------------------------------------------------------------- contexts (GSUB 5/6, GPOS 7/8) */

static int apply_lookup_at(Ctx *c, unsigned li, int pos);

typedef struct
{
    int kind; /* 0 glyph ids, 1 classes, 2 coverage tables */
    const unsigned char *arr;
    const unsigned char *cd;      /* ClassDef (kind 1) */
    const unsigned char *covbase; /* coverage offsets count from here (kind 2) */
    int bias;                     /* element k of the sequence is arr[k - bias] */
} Seq;

static int seq_test(const Seq *s, int k, unsigned gid)
{
    const unsigned char *e = s->arr + 2 * (k - s->bias);
    if (s->kind == 0)
        return gid == u16(e);
    if (s->kind == 1)
        return class_of(s->cd, gid) == u16(e);
    return cov_index(s->covbase + u16(e), gid) >= 0;
}

static int try_rule(Ctx *c, const Lk *lk, int i, int *next, const Seq *bt, unsigned btn, const Seq *in, unsigned inn,
                    const Seq *la, unsigned lan, const unsigned char *rec, unsigned nrec)
{
    int mp[MAXIN];
    if (inn == 0 || inn > MAXIN)
        return 0;
    mp[0] = i;
    if (in->bias == 0 && !seq_test(in, 0, c->buf[i].gid))
        return 0;
    for (unsigned k = 1; k < inn; k++)
    {
        int j = next_glyph(c, lk, mp[k - 1]);
        if (j < 0 || !(c->buf[j].mask & c->lookup_mask) || !seq_test(in, (int)k, c->buf[j].gid))
            return 0;
        mp[k] = j;
    }
    int j = i;
    for (unsigned k = 0; k < btn; k++) /* backtrack: element 0 is the glyph just before the match */
    {
        j = prev_glyph(c, lk, j);
        if (j < 0 || !seq_test(bt, (int)k, c->buf[j].gid))
            return 0;
    }
    j = mp[inn - 1];
    for (unsigned k = 0; k < lan; k++)
    {
        j = next_glyph(c, lk, j);
        if (j < 0 || !seq_test(la, (int)k, c->buf[j].gid))
            return 0;
    }
    /* The rule matches: run the nested lookups. They may add or remove glyphs; the matched positions after the
     * one being changed move with them. */
    int end = mp[inn - 1] + 1;
    c->depth++;
    for (unsigned r = 0; r < nrec; r++, rec += 4)
    {
        unsigned seq = u16(rec);
        if (seq >= inn)
            continue;
        int before = c->len;
        if (mp[seq] < c->len)
            apply_lookup_at(c, u16(rec + 2), mp[seq]);
        int d = c->len - before;
        if (d)
        {
            for (unsigned k = seq + 1; k < inn; k++)
                mp[k] += d;
            end += d;
        }
    }
    c->depth--;
    *next = end > c->len ? c->len : end;
    return 1;
}

static int apply_context(Ctx *c, const Lk *lk, int chain, const unsigned char *st, int i, int *next)
{
    unsigned fmt = u16(st);
    unsigned gid = c->buf[i].gid;
    if (fmt == 1 || fmt == 2)
    {
        int ci = cov_index(st + u16(st + 2), gid);
        if (ci < 0)
            return 0;
        const unsigned char *cd_bt = 0, *cd_in = 0, *cd_la = 0, *sets;
        unsigned nsets, pick;
        if (fmt == 1)
        {
            nsets = u16(st + 4);
            sets = st + 6;
            pick = (unsigned)ci;
        }
        else if (!chain)
        {
            cd_in = st + u16(st + 4);
            nsets = u16(st + 6);
            sets = st + 8;
            pick = class_of(cd_in, gid);
        }
        else
        {
            cd_bt = st + u16(st + 4);
            cd_in = st + u16(st + 6);
            cd_la = st + u16(st + 8);
            nsets = u16(st + 10);
            sets = st + 12;
            pick = class_of(cd_in, gid);
        }
        if (pick >= nsets || !u16(sets + 2 * pick))
            return 0;
        const unsigned char *rs = st + u16(sets + 2 * pick);
        unsigned nr = u16(rs);
        int kind = fmt == 1 ? 0 : 1;
        for (unsigned r = 0; r < nr; r++)
        {
            const unsigned char *rule = rs + u16(rs + 2 + 2 * r), *p = rule;
            Seq bt = {kind, 0, cd_bt, 0, 0}, in = {kind, 0, cd_in, 0, 1}, la = {kind, 0, cd_la, 0, 0};
            unsigned btn = 0, inn, lan = 0, nrec;
            if (chain)
            {
                btn = u16(p);
                bt.arr = p + 2;
                p += 2 + 2 * btn;
                inn = u16(p);
                in.arr = p + 2;
                p += 2 + 2 * (inn ? inn - 1 : 0);
                lan = u16(p);
                la.arr = p + 2;
                p += 2 + 2 * lan;
                nrec = u16(p);
                p += 2;
            }
            else
            {
                inn = u16(p);
                nrec = u16(p + 2);
                in.arr = p + 4;
                p += 4 + 2 * (inn ? inn - 1 : 0);
            }
            if (try_rule(c, lk, i, next, &bt, btn, &in, inn, &la, lan, p, nrec))
                return 1;
        }
        return 0;
    }
    if (fmt == 3)
    {
        Seq bt = {2, 0, 0, st, 0}, in = {2, 0, 0, st, 0}, la = {2, 0, 0, st, 0};
        const unsigned char *p = st + 2;
        unsigned btn = 0, inn, lan = 0, nrec;
        if (chain)
        {
            btn = u16(p);
            bt.arr = p + 2;
            p += 2 + 2 * btn;
            inn = u16(p);
            in.arr = p + 2;
            p += 2 + 2 * inn;
            lan = u16(p);
            la.arr = p + 2;
            p += 2 + 2 * lan;
            nrec = u16(p);
            p += 2;
        }
        else
        {
            inn = u16(p);
            nrec = u16(p + 2);
            in.arr = p + 4;
            p += 4 + 2 * inn;
        }
        return try_rule(c, lk, i, next, &bt, btn, &in, inn, &la, lan, p, nrec);
    }
    return 0;
}

/* ---------------------------------------------------------------- GSUB lookups */

static void buf_remove(Ctx *c, int at)
{
    for (int k = at; k + 1 < c->len; k++)
        c->buf[k] = c->buf[k + 1];
    c->len--;
}

static int gsub_single(Ctx *c, const unsigned char *st, int i, int *next)
{
    G *g = &c->buf[i];
    int ci = cov_index(st + u16(st + 2), g->gid);
    if (ci < 0)
        return 0;
    if (u16(st) == 1)
        set_gid(g, (g->gid + (unsigned)s16(st + 4)) & 0xFFFFu);
    else if ((unsigned)ci < u16(st + 4))
        set_gid(g, u16(st + 6 + 2 * ci));
    else
        return 0;
    *next = i + 1;
    return 1;
}

static int gsub_multiple(Ctx *c, const unsigned char *st, int i, int *next)
{
    int ci = cov_index(st + u16(st + 2), c->buf[i].gid);
    if (ci < 0 || (unsigned)ci >= u16(st + 4))
        return 0;
    const unsigned char *seq = st + u16(st + 6 + 2 * ci);
    int n = (int)u16(seq);
    if (c->len - 1 + n > CAP)
        return 0;
    G orig = c->buf[i];
    if (n == 0)
    {
        buf_remove(c, i);
        *next = i;
        return 1;
    }
    for (int k = c->len - 1; k > i; k--) /* open a gap of n-1 glyphs after i */
        c->buf[k + n - 1] = c->buf[k];
    c->len += n - 1;
    for (int k = 0; k < n; k++)
    {
        c->buf[i + k] = orig;
        set_gid(&c->buf[i + k], u16(seq + 2 + 2 * k));
    }
    *next = i + n;
    return 1;
}

static int gsub_ligature(Ctx *c, const Lk *lk, const unsigned char *st, int i, int *next)
{
    int ci = cov_index(st + u16(st + 2), c->buf[i].gid);
    if (ci < 0 || (unsigned)ci >= u16(st + 4))
        return 0;
    const unsigned char *set = st + u16(st + 6 + 2 * ci);
    unsigned nlig = u16(set);
    for (unsigned l = 0; l < nlig; l++)
    {
        const unsigned char *lig = set + u16(set + 2 + 2 * l);
        unsigned ncomp = u16(lig + 2);
        if (ncomp == 0 || ncomp > MAXIN)
            continue;
        int mp[MAXIN];
        mp[0] = i;
        int ok = 1;
        for (unsigned k = 1; k < ncomp && ok; k++)
        {
            int j = next_glyph(c, lk, mp[k - 1]);
            if (j < 0 || !(c->buf[j].mask & c->lookup_mask) || c->buf[j].gid != u16(lig + 2 + 2 * k))
                ok = 0;
            else
                mp[k] = j;
        }
        if (!ok)
            continue;
        unsigned char flags = 0;
        for (unsigned k = 0; k < ncomp; k++)
            flags |= c->buf[mp[k]].flags;
        set_gid(&c->buf[i], u16(lig));
        c->buf[i].flags = flags;
        for (unsigned k = ncomp - 1; k >= 1; k--) /* marks that were skipped between the parts stay */
            buf_remove(c, mp[k]);
        *next = i + 1;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- GPOS lookups */

static int value_size(unsigned fmt)
{
    int n = 0;
    for (; fmt; fmt >>= 1)
        n += (int)(fmt & 1u);
    return n * 2;
}

/* v[0..2] = x placement, y placement, x advance (y advance and device tables are not used). */
static void read_value(const unsigned char *p, unsigned fmt, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (fmt & 1u)
        v[0] = s16(p), p += 2;
    if (fmt & 2u)
        v[1] = s16(p), p += 2;
    if (fmt & 4u)
        v[2] = s16(p);
}

static void add_value(G *g, const int v[3])
{
    g->xoff += v[0];
    g->yoff += v[1];
    g->xadv += v[2];
}

static int gpos_single(Ctx *c, const unsigned char *st, int i, int *next)
{
    G *g = &c->buf[i];
    int ci = cov_index(st + u16(st + 2), g->gid);
    unsigned vf = u16(st + 4);
    int v[3];
    if (ci < 0)
        return 0;
    if (u16(st) == 1)
        read_value(st + 6, vf, v);
    else if ((unsigned)ci < u16(st + 6))
        read_value(st + 8 + value_size(vf) * ci, vf, v);
    else
        return 0;
    add_value(g, v);
    *next = i + 1;
    return 1;
}

static int gpos_pair(Ctx *c, const Lk *lk, const unsigned char *st, int i, int *next)
{
    G *g1 = &c->buf[i];
    int ci = cov_index(st + u16(st + 2), g1->gid);
    if (ci < 0)
        return 0;
    int j = next_glyph(c, lk, i);
    if (j < 0)
        return 0;
    unsigned vf1 = u16(st + 4), vf2 = u16(st + 6);
    int s1 = value_size(vf1), s2 = value_size(vf2), v1[3], v2[3];
    const unsigned char *rec = 0;
    if (u16(st) == 1)
    {
        if ((unsigned)ci >= u16(st + 8))
            return 0;
        const unsigned char *set = st + u16(st + 10 + 2 * ci);
        unsigned n = u16(set);
        for (unsigned k = 0; k < n; k++)
            if (u16(set + 2 + (2 + s1 + s2) * k) == c->buf[j].gid)
            {
                rec = set + 2 + (2 + s1 + s2) * k + 2;
                break;
            }
    }
    else
    {
        unsigned c1 = class_of(st + u16(st + 8), g1->gid), c2 = class_of(st + u16(st + 10), c->buf[j].gid);
        unsigned n1 = u16(st + 12), n2 = u16(st + 14);
        if (c1 < n1 && c2 < n2)
            rec = st + 16 + (s1 + s2) * (c1 * n2 + c2);
    }
    if (!rec)
        return 0;
    read_value(rec, vf1, v1);
    read_value(rec + s1, vf2, v2);
    add_value(g1, v1);
    add_value(&c->buf[j], v2);
    *next = vf2 ? j + 1 : j;
    return 1;
}

/* Anchor: x, y in design units. */
static void read_anchor(const unsigned char *a, int *x, int *y)
{
    *x = s16(a + 2);
    *y = s16(a + 4);
}

/* Mark-to-base (type 4) and mark-to-mark (type 6) attachment share one table layout. */
static int gpos_mark_attach(Ctx *c, const Lk *lk, int is_mark2mark, const unsigned char *st, int i, int *next)
{
    G *m = &c->buf[i];
    int mi = cov_index(st + u16(st + 2), m->gid);
    if (mi < 0)
        return 0;
    int j;
    if (is_mark2mark)
    {
        j = prev_glyph(c, lk, i);
        if (j < 0 || c->buf[j].cls != 3)
            return 0;
    }
    else
    {
        for (j = i - 1; j >= 0 && c->buf[j].cls == 3; j--)
            ;
        if (j < 0)
            return 0;
    }
    int bi = cov_index(st + u16(st + 4), c->buf[j].gid);
    if (bi < 0)
        return 0;
    unsigned nclass = u16(st + 6);
    const unsigned char *marks = st + u16(st + 8), *bases = st + u16(st + 10);
    if ((unsigned)mi >= u16(marks) || (unsigned)bi >= u16(bases))
        return 0;
    unsigned mclass = u16(marks + 2 + 4 * mi);
    if (mclass >= nclass)
        return 0;
    unsigned boff = u16(bases + 2 + 2 * ((unsigned)bi * nclass + mclass));
    if (!boff)
        return 0;
    int mx, my, bx, by;
    read_anchor(marks + u16(marks + 2 + 4 * mi + 2), &mx, &my);
    read_anchor(bases + boff, &bx, &by);
    m->attach = (short)j;
    m->ax = bx - mx;
    m->ay = by - my;
    *next = i + 1;
    return 1;
}

/* ---------------------------------------------------------------- running lookups */

static int apply_sub(Ctx *c, const Lk *lk, unsigned type, const unsigned char *st, int i, int *next)
{
    if (c->gpos)
    {
        if (type == 9) /* extension: the real type and table are one hop away */
            return apply_sub(c, lk, u16(st + 2), st + u32(st + 4), i, next);
        switch (type)
        {
        case 1: return gpos_single(c, st, i, next);
        case 2: return gpos_pair(c, lk, st, i, next);
        case 4: return gpos_mark_attach(c, lk, 0, st, i, next);
        case 6: return gpos_mark_attach(c, lk, 1, st, i, next);
        case 7: return apply_context(c, lk, 0, st, i, next);
        case 8: return apply_context(c, lk, 1, st, i, next);
        }
        return 0;
    }
    if (type == 7)
        return apply_sub(c, lk, u16(st + 2), st + u32(st + 4), i, next);
    switch (type)
    {
    case 1: return gsub_single(c, st, i, next);
    case 2: return gsub_multiple(c, st, i, next);
    case 4: return gsub_ligature(c, lk, st, i, next);
    case 5: return apply_context(c, lk, 0, st, i, next);
    case 6: return apply_context(c, lk, 1, st, i, next);
    }
    return 0;
}

/* Try every subtable of the lookup at glyph i; the first that applies wins. */
static int apply_at(Ctx *c, const Lk *lk, int i, int *next)
{
    if (ignorable(lk, &c->buf[i]))
        return 0;
    for (unsigned s = 0; s < lk->nsub; s++)
        if (apply_sub(c, lk, lk->type, lk->lk + u16(lk->lk + 6 + 2 * s), i, next))
            return 1;
    return 0;
}

/* A lookup called from a context: at one position only. */
static int apply_lookup_at(Ctx *c, unsigned li, int pos)
{
    Lk lk;
    int next;
    if (c->depth > 4 || pos < 0 || pos >= c->len || !get_lk(c, li, &lk))
        return 0;
    return apply_at(c, &lk, pos, &next);
}

/* A lookup over the whole syllable. */
static void run_lookup(Ctx *c, unsigned li, unsigned mask)
{
    Lk lk;
    if (!get_lk(c, li, &lk))
        return;
    c->lookup_mask = mask;
    for (int i = 0; i < c->len;)
    {
        int next = i + 1;
        if ((c->buf[i].mask & mask) && apply_at(c, &lk, i, &next))
            i = next > i ? next : i + 1;
        else
            i++;
    }
}

/* ---------------------------------------------------------------- Bengali: character classes */

enum { K_OTHER, K_C, K_V, K_H, K_N, K_M, K_SM, K_ZWJ, K_ZWNJ };

#define RA 0x09B0u
#define BA 0x09ACu
#define YA 0x09AFu

static int cat(unsigned cp)
{
    if ((cp >= 0x0995 && cp <= 0x09B9) || cp == 0x09CE || cp == 0x09DC || cp == 0x09DD || cp == 0x09DF ||
        cp == 0x09F0 || cp == 0x09F1)
        return K_C;
    if ((cp >= 0x0985 && cp <= 0x0994) || cp == 0x09E0 || cp == 0x09E1)
        return K_V;
    if (cp == 0x09CD)
        return K_H;
    if (cp == 0x09BC)
        return K_N;
    if ((cp >= 0x09BE && cp <= 0x09C4) || cp == 0x09C7 || cp == 0x09C8 || cp == 0x09CB || cp == 0x09CC ||
        cp == 0x09D7 || cp == 0x09E2 || cp == 0x09E3)
        return K_M;
    if (cp >= 0x0981 && cp <= 0x0983)
        return K_SM;
    if (cp == 0x200D)
        return K_ZWJ;
    if (cp == 0x200C)
        return K_ZWNJ;
    return K_OTHER;
}

static int is_joiner(unsigned cp) { return cp == 0x200C || cp == 0x200D; }

/* Vowel signs typed after the consonant but drawn before it: ি ে ৈ. */
static int is_pre_base_sign(unsigned cp) { return cp == 0x09BF || cp == 0x09C7 || cp == 0x09C8; }

static unsigned cmap_gid(unsigned cp)
{
    int lo = 0, hi = (int)bn_cmap_n - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        unsigned v = bn_cmap_cp[mid];
        if (v == cp)
            return bn_cmap_gid[mid];
        if (v < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

int bn_is_bengali(unsigned cp)
{
    if ((cp >= 0x0980 && cp <= 0x09FF) || cp == 0x0964 || cp == 0x0965 || cp == 0x200C || cp == 0x200D)
        return cmap_gid(cp) != 0;
    return 0;
}

/* ---------------------------------------------------------------- Bengali: syllables */

int bn_syllable_len(const unsigned *cp, int n)
{
    int i = 0;
    int k = cat(cp[0]);
    if (k == K_C)
    {
        for (;;)
        {
            i++;                                  /* the consonant */
            if (i < n && cat(cp[i]) == K_N)       /* nukta */
                i++;
            if (i < n && cat(cp[i]) == K_H)       /* hasanta */
            {
                int j = i + 1;
                if (j < n && is_joiner(cp[j]))
                    j++;
                if (j < n && cat(cp[j]) == K_C && j < BN_SYL_MAX)
                {
                    i = j; /* another consonant joins: conjunct */
                    continue;
                }
                i = j; /* hasanta left at the end (visible hasanta) */
                break;
            }
            break;
        }
        while (i < n && cat(cp[i]) == K_M)
            i++;
        while (i < n && cat(cp[i]) == K_SM)
            i++;
    }
    else if (k == K_V)
    {
        i = 1;
        if (i < n && cat(cp[i]) == K_N)
            i++;
        while (i < n && cat(cp[i]) == K_SM)
            i++;
    }
    else
        i = 1;
    return i > BN_SYL_MAX ? BN_SYL_MAX : i;
}

/* ---------------------------------------------------------------- Bengali: shaping */

const Font *bn_face_for_size(int size)
{
    if (size <= 13)
        return &font_bn_12;
    if (size <= 15)
        return &font_bn_14;
    if (size <= 18)
        return &font_bn_16;
    if (size <= 26)
        return &font_bn_20;
    return &font_bn_32;
}

static int scale16(int units, int size)
{
    long v = (long)units * size * 16;
    return (int)(v >= 0 ? (v + BN_UPEM / 2) / BN_UPEM : -((-v + BN_UPEM / 2) / BN_UPEM));
}

/* Lookup mask for each GSUB stage: most features apply to every glyph; the four that depend on where a glyph is in
 * the syllable only to the glyphs that were marked for them. */
static unsigned stage_mask(int stage)
{
    switch (stage)
    {
    case 2: return M_RPHF;
    case 3: return M_BLWF;
    case 4: return M_HALF;
    case 5: return M_PSTF;
    }
    return M_ALL;
}

static void run_gsub_stage(Ctx *c, int stage)
{
    for (unsigned k = bn_gsub_stage[stage]; k < bn_gsub_stage[stage + 1]; k++)
        run_lookup(c, bn_gsub_lookups[k], bn_gsub_init[k] ? M_INIT : stage_mask(stage));
}

int bn_shape_syllable(const unsigned *cp, int n, int word_start, int size, BnGlyphPos *out, int max_out)
{
    Ctx c;
    unsigned u[BN_SYL_MAX * 2 + 2];
    int m = 0;

    /* Split vowel signs: ো = ে + া and ৌ = ে + ৗ (the font has glyphs for the two halves, and the first half
     * is a pre-base sign). */
    for (int k = 0; k < n && m + 2 <= (int)(sizeof u / sizeof u[0]); k++)
    {
        if (cp[k] == 0x09CB)
            u[m++] = 0x09C7, u[m++] = 0x09BE;
        else if (cp[k] == 0x09CC)
            u[m++] = 0x09C7, u[m++] = 0x09D7;
        else
            u[m++] = cp[k];
    }

    int reph = 0, base = -1;
    int order[BN_SYL_MAX * 2 + 2];
    if (cat(u[0]) == K_C)
    {
        /* Reph: a leading "র্" followed by a consonant is drawn as a hook above that consonant. */
        int first_c = 0;
        if (m >= 3 && u[0] == RA && cat(u[1]) == K_H && !is_joiner(u[2]))
            for (int k = 2; k < m; k++)
                if (cat(u[k]) == K_C)
                {
                    reph = 1;
                    first_c = 2;
                    break;
                }
        /* Base: the last consonant, unless it forms a below/post-base shape (র ব য after a hasanta): then the
         * consonant before the hasanta. */
        int cand = -1;
        for (int k = m - 1; k >= first_c; k--)
            if (cat(u[k]) == K_C)
            {
                cand = k;
                break;
            }
        while (cand > first_c && (u[cand] == RA || u[cand] == BA || u[cand] == YA))
        {
            int h = cand - 1;
            if (h > first_c && is_joiner(u[h]))
                h--;
            if (h <= first_c || cat(u[h]) != K_H)
                break;
            int prev = -1;
            for (int k = h - 1; k >= first_c; k--)
                if (cat(u[k]) == K_C)
                {
                    prev = k;
                    break;
                }
            if (prev < 0)
                break;
            cand = prev;
        }
        base = cand;

        /* Order: reph, then the vowel signs drawn before the consonant (ি ে ৈ), then everything else as typed. */
        int o = 0;
        if (reph)
            order[o++] = 0, order[o++] = 1;
        for (int k = first_c; k < m; k++)
            if (is_pre_base_sign(u[k]))
                order[o++] = k;
        for (int k = first_c; k < m; k++)
            if (!is_pre_base_sign(u[k]))
                order[o++] = k;
    }
    else
        for (int k = 0; k < m; k++)
            order[k] = k;

    c.len = 0;
    c.lookups = bn_gsub + u16(bn_gsub + 8);
    c.gpos = 0;
    c.depth = 0;
    for (int k = 0; k < m && c.len < CAP; k++)
    {
        G *g = &c.buf[c.len++];
        int idx = order[k];
        g->mask = M_ALL;
        g->flags = 0;
        g->attach = -1;
        g->xoff = g->yoff = g->xadv = g->ax = g->ay = 0;
        set_gid(g, cmap_gid(u[idx]));
        if (idx == base)
            g->flags |= F_BASE;
        if (reph && idx < 2)
            g->mask |= M_RPHF;
        else if (base >= 0 && idx < base)
            g->mask |= M_HALF;
        else if (base >= 0 && idx > base)
            g->mask |= M_BLWF | M_PSTF;
    }
    if (word_start && c.len)
        c.buf[0].mask |= M_INIT;
    unsigned ra_gid = cmap_gid(u[0]);

    for (int s = 0; s < 8; s++)
        run_gsub_stage(&c, s);

    /* Final reordering: the reph that "রphf" produced moves from the front to just after the base consonant. */
    if (reph && c.len > 1 && c.buf[0].gid != ra_gid)
    {
        int b = -1;
        for (int k = 1; k < c.len; k++)
            if (c.buf[k].flags & F_BASE)
            {
                b = k;
                break;
            }
        if (b > 0)
        {
            G r = c.buf[0];
            for (int k = 0; k < b; k++)
                c.buf[k] = c.buf[k + 1];
            c.buf[b] = r;
        }
    }

    run_gsub_stage(&c, 8);

    /* Positioning. */
    c.lookups = bn_gpos + u16(bn_gpos + 8);
    c.gpos = 1;
    c.depth = 0;
    for (unsigned k = 0; k < bn_gpos_nlookups; k++)
        run_lookup(&c, bn_gpos_lookups[k], M_ALL);

    /* Marks have no width of their own; attached marks move to their anchor, relative to the glyph they sit on
     * (minus the pen movement between that glyph and the mark). */
    for (int i = 0; i < c.len; i++)
        if (c.buf[i].cls == 3)
            c.buf[i].xadv = -(int)bn_adv[c.buf[i].gid];
    for (int i = 0; i < c.len; i++)
    {
        G *g = &c.buf[i];
        if (g->attach < 0 || g->attach >= i)
            continue;
        g->xoff = g->ax + c.buf[g->attach].xoff;
        g->yoff = g->ay + c.buf[g->attach].yoff;
        for (int k = g->attach; k < i; k++)
            g->xoff -= (int)bn_adv[c.buf[k].gid] + c.buf[k].xadv;
    }

    int cnt = c.len < max_out ? c.len : max_out;
    for (int i = 0; i < cnt; i++)
    {
        const G *g = &c.buf[i];
        out[i].gid = g->gid;
        out[i].x_off = scale16(g->xoff, size);
        out[i].y_off = scale16(g->yoff, size);
        out[i].adv = scale16((int)bn_adv[g->gid] + g->xadv, size);
    }
    return cnt;
}
