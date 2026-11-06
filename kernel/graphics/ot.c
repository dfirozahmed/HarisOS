/* HarisOS OpenType layout engine: see include/ot.h.
 *
 * It reads the font's GSUB / GPOS / GDEF tables as they are in the .ttf, so there is no private table format. Supported:
 * GSUB single, multiple, ligature, context and chaining context (all formats); GPOS single, pair, mark-to-base,
 * mark-to-ligature, mark-to-mark, context and chaining context, and extension lookups of both. Alternate substitution,
 * cursive attachment and device tables are not used by the fonts we embed and are skipped. */
#include "ot.h"

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

static const unsigned char *gdef_sub(const unsigned char *gdef, unsigned off_at)
{
    if (!gdef)
        return 0;
    unsigned off = u16(gdef + off_at);
    return off ? gdef + off : 0;
}

/* 0 = unclassified, 1 = base, 2 = ligature, 3 = mark, 4 = component */
static unsigned glyph_class(const unsigned char *gdef, unsigned gid) { return class_of(gdef_sub(gdef, 4), gid); }
static unsigned mark_attach_class(const unsigned char *gdef, unsigned gid) { return class_of(gdef_sub(gdef, 10), gid); }

static int in_mark_set(const unsigned char *gdef, unsigned set, unsigned gid)
{
    if (!gdef || u16(gdef + 2) < 2) /* minor version: sets exist from 1.2 */
        return 0;
    const unsigned char *sets = gdef_sub(gdef, 12);
    if (!sets || set >= u16(sets + 2))
        return 0;
    return cov_index(sets + u32(sets + 4 + 4 * set), gid) >= 0;
}

/* ---------------------------------------------------------------- the buffer */

typedef struct
{
    unsigned type, flag, nsub, mset;
    const unsigned char *lk;
    const unsigned char *gdef;
} Lk;

static void set_gid(OtBuf *c, OtGlyph *g, unsigned gid)
{
    g->gid = (unsigned short)gid;
    g->cls = (unsigned char)glyph_class(c->font->gdef, gid);
}

static int get_lk(const OtBuf *c, unsigned li, Lk *o)
{
    if (li >= u16(c->lookups))
        return 0;
    o->lk = c->lookups + u16(c->lookups + 2 + 2 * li);
    o->type = u16(o->lk);
    o->flag = u16(o->lk + 2);
    o->nsub = u16(o->lk + 4);
    o->mset = (o->flag & 0x10u) ? u16(o->lk + 6 + 2 * o->nsub) : 0;
    o->gdef = c->font->gdef;
    return 1;
}

/* LookupFlag: ignore bases (2), ligatures (4), marks (8), marks outside a filtering set (0x10), marks of the wrong attachment
 * class (high byte). */
static int ignorable(const Lk *lk, const OtGlyph *g)
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
        if ((flag & 0x10u) && !in_mark_set(lk->gdef, lk->mset, g->gid))
            return 1;
        if ((flag & 0xFF00u) && mark_attach_class(lk->gdef, g->gid) != (flag >> 8))
            return 1;
    }
    return 0;
}

static int next_glyph(const OtBuf *c, const Lk *lk, int i)
{
    for (i++; i < c->len; i++)
        if (!ignorable(lk, &c->buf[i]))
            return i;
    return -1;
}

static int prev_glyph(const OtBuf *c, const Lk *lk, int i)
{
    for (i--; i >= 0; i--)
        if (!ignorable(lk, &c->buf[i]))
            return i;
    return -1;
}

unsigned ot_cmap(const OtFont *f, unsigned cp)
{
    int lo = 0, hi = (int)f->cmap_n - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        unsigned v = f->cmap_cp[mid];
        if (v == cp)
            return f->cmap_gid[mid];
        if (v < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

void ot_begin(OtBuf *b, const OtFont *f)
{
    b->font = f;
    b->lookups = f->gsub + u16(f->gsub + 8);
    b->gpos = 0;
    b->lookup_mask = 0;
    b->depth = 0;
    b->len = 0;
}

int ot_add(OtBuf *b, unsigned gid, unsigned mask, unsigned char flags)
{
    if (b->len >= OT_CAP)
        return 0;
    OtGlyph *g = &b->buf[b->len++];
    g->mask = (unsigned short)(mask | OT_MASK_ALL);
    g->flags = flags;
    g->attach = -1;
    g->xoff = g->yoff = g->xadv = g->ax = g->ay = 0;
    set_gid(b, g, gid);
    return 1;
}

/* ---------------------------------------------------------------- contexts (GSUB 5/6, GPOS 7/8) */

static int apply_lookup_at(OtBuf *c, unsigned li, int pos);

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

#define MAXIN 16 /* longest matched input sequence */

static int try_rule(OtBuf *c, const Lk *lk, int i, int *next, const Seq *bt, unsigned btn, const Seq *in, unsigned inn,
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
    /* The rule matches: run the nested lookups. They may add or remove glyphs; the matched positions after the one being
     * changed move with them. */
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

static int apply_context(OtBuf *c, const Lk *lk, int chain, const unsigned char *st, int i, int *next)
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

static void buf_remove(OtBuf *c, int at)
{
    for (int k = at; k + 1 < c->len; k++)
        c->buf[k] = c->buf[k + 1];
    c->len--;
}

static int gsub_single(OtBuf *c, const unsigned char *st, int i, int *next)
{
    OtGlyph *g = &c->buf[i];
    int ci = cov_index(st + u16(st + 2), g->gid);
    if (ci < 0)
        return 0;
    if (u16(st) == 1)
        set_gid(c, g, (g->gid + (unsigned)s16(st + 4)) & 0xFFFFu);
    else if ((unsigned)ci < u16(st + 4))
        set_gid(c, g, u16(st + 6 + 2 * ci));
    else
        return 0;
    *next = i + 1;
    return 1;
}

static int gsub_multiple(OtBuf *c, const unsigned char *st, int i, int *next)
{
    int ci = cov_index(st + u16(st + 2), c->buf[i].gid);
    if (ci < 0 || (unsigned)ci >= u16(st + 4))
        return 0;
    const unsigned char *seq = st + u16(st + 6 + 2 * ci);
    int n = (int)u16(seq);
    if (c->len - 1 + n > OT_CAP)
        return 0;
    OtGlyph orig = c->buf[i];
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
        set_gid(c, &c->buf[i + k], u16(seq + 2 + 2 * k));
    }
    *next = i + n;
    return 1;
}

static int gsub_ligature(OtBuf *c, const Lk *lk, const unsigned char *st, int i, int *next)
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
        set_gid(c, &c->buf[i], u16(lig));
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

static void add_value(OtGlyph *g, const int v[3])
{
    g->xoff += v[0];
    g->yoff += v[1];
    g->xadv += v[2];
}

static int gpos_single(OtBuf *c, const unsigned char *st, int i, int *next)
{
    OtGlyph *g = &c->buf[i];
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

static int gpos_pair(OtBuf *c, const Lk *lk, const unsigned char *st, int i, int *next)
{
    OtGlyph *g1 = &c->buf[i];
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

/* Mark-to-base (type 4) and mark-to-mark (type 6) attachment share one table layout; mark-to-ligature (type 5) has the
 * component of the ligature in between. A mark after a ligature is attached to its last component. */
static int gpos_mark_attach(OtBuf *c, const Lk *lk, int kind /* 4, 5 or 6 */, const unsigned char *st, int i, int *next)
{
    OtGlyph *m = &c->buf[i];
    int mi = cov_index(st + u16(st + 2), m->gid);
    if (mi < 0)
        return 0;
    int j;
    if (kind == 6)
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
    unsigned boff;
    const unsigned char *base_rec;
    if (kind == 5)
    {
        unsigned lo = u16(bases + 2 + 2 * (unsigned)bi);
        if (!lo)
            return 0;
        const unsigned char *lig = bases + lo; /* LigatureAttach: componentCount, then offsets[nclass] per component */
        unsigned ncomp = u16(lig);
        if (ncomp == 0)
            return 0;
        base_rec = lig; /* anchors count from the LigatureAttach */
        boff = u16(lig + 2 + 2 * ((ncomp - 1) * nclass + mclass));
    }
    else
    {
        base_rec = bases;
        boff = u16(bases + 2 + 2 * ((unsigned)bi * nclass + mclass));
    }
    if (!boff)
        return 0;
    int mx, my, bx, by;
    read_anchor(marks + u16(marks + 2 + 4 * mi + 2), &mx, &my);
    read_anchor(base_rec + boff, &bx, &by);
    m->attach = (short)j;
    m->ax = bx - mx;
    m->ay = by - my;
    *next = i + 1;
    return 1;
}

/* ---------------------------------------------------------------- running lookups */

static int apply_sub(OtBuf *c, const Lk *lk, unsigned type, const unsigned char *st, int i, int *next)
{
    if (c->gpos)
    {
        if (type == 9) /* extension: the real type and table are one hop away */
            return apply_sub(c, lk, u16(st + 2), st + u32(st + 4), i, next);
        switch (type)
        {
        case 1: return gpos_single(c, st, i, next);
        case 2: return gpos_pair(c, lk, st, i, next);
        case 4:
        case 5:
        case 6: return gpos_mark_attach(c, lk, (int)type, st, i, next);
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
static int apply_at(OtBuf *c, const Lk *lk, int i, int *next)
{
    if (ignorable(lk, &c->buf[i]))
        return 0;
    for (unsigned s = 0; s < lk->nsub; s++)
        if (apply_sub(c, lk, lk->type, lk->lk + u16(lk->lk + 6 + 2 * s), i, next))
            return 1;
    return 0;
}

/* A lookup called from a context: at one position only. */
static int apply_lookup_at(OtBuf *c, unsigned li, int pos)
{
    Lk lk;
    int next;
    if (c->depth > 4 || pos < 0 || pos >= c->len || !get_lk(c, li, &lk))
        return 0;
    return apply_at(c, &lk, pos, &next);
}

/* A lookup over the whole buffer. */
static void run_lookup(OtBuf *c, unsigned li, unsigned mask)
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

void ot_gsub_stage(OtBuf *b, int stage, unsigned mask, unsigned init_mask)
{
    const OtFont *f = b->font;
    b->gpos = 0;
    b->lookups = f->gsub + u16(f->gsub + 8);
    b->depth = 0;
    for (unsigned k = f->gsub_stage[stage]; k < f->gsub_stage[stage + 1]; k++)
        run_lookup(b, f->gsub_lookups[k], f->gsub_init && f->gsub_init[k] ? init_mask : mask);
}

void ot_gpos(OtBuf *b)
{
    const OtFont *f = b->font;
    b->gpos = 1;
    b->lookups = f->gpos + u16(f->gpos + 8);
    b->depth = 0;
    for (unsigned k = 0; k < f->gpos_n; k++)
        run_lookup(b, f->gpos_lookups[k], OT_MASK_ALL);
}

void ot_finish(OtBuf *c, int rtl)
{
    const OtFont *f = c->font;
    for (int i = 0; i < c->len; i++)
        if (c->buf[i].cls == 3)
            c->buf[i].xadv = -(int)f->adv[c->buf[i].gid]; /* marks have no width of their own */
    for (int i = 0; i < c->len; i++)
    {
        OtGlyph *g = &c->buf[i];
        if (g->attach < 0 || g->attach >= i)
            continue;
        g->xoff = g->ax + c->buf[g->attach].xoff;
        g->yoff = g->ay + c->buf[g->attach].yoff;
        if (!rtl) /* left to right: the pen has moved past the base and the marks between: take that back */
            for (int k = g->attach; k < i; k++)
                g->xoff -= (int)f->adv[c->buf[k].gid] + c->buf[k].xadv;
        /* right to left the glyphs are drawn in the opposite order: the mark comes first and has no width, so it and its base
         * are at the same pen position and nothing is taken back */
    }
}
