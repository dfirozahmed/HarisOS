/* HarisOS text rendering: see include/font.h.
 * Glyph bitmaps are 8-bit coverage; each pixel is blended with the background.
 *
 * Strings are UTF-8. ASCII is drawn one byte at a time from the Latin font; Latin-1, Greek, Cyrillic and some symbols come from a
 * second face (font_ext.h); a run of Bengali is cut into syllables, each shaped by shape_bn.c; a run of Arabic letters is shaped
 * as a word by shape_ar.c. A line with strong right-to-left letters goes through the Unicode bidi algorithm (bidi.h) first, which
 * says where each shaped run stands on the line. Drawing, measuring and fitting all walk the text through lay_next, so they
 * always agree on where every glyph is. */
#include "bidi.h"
#include "font.h"
#include "font_ext.h"
#include "ot.h"
#include "shape_ar.h"
#include "shape_bn.h"
#include "utf8.h"

/* The position of character cp in the extra faces (font_ext.h), -1 if they do not have it. ext_cp is sorted: a binary search. */
static int ext_index(unsigned cp)
{
    int lo = 0, hi = (int)ext_n - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        unsigned v = ext_cp[mid];
        if (v == cp)
            return mid;
        if (v < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* The extra face of the size nearest to the Latin font's (the same five sizes as the Bengali and Arabic faces). */
static const Font *ext_face_for_size(int size)
{
    if (size <= 13)
        return &font_ext_12;
    if (size <= 15)
        return &font_ext_14;
    if (size <= 18)
        return &font_ext_16;
    if (size <= 26)
        return &font_ext_20;
    return &font_ext_32;
}

/* One glyph to draw. newline = 1: a line break (no glyph). */
typedef struct
{
    const Font *font;
    const FontGlyph *g;
    int xo, yo; /* offset from the pen, 1/16 pixel (yo: positive = up) */
    int adv;    /* pen advance after the glyph, 1/16 pixel */
    int newline;
} LayItem;

/* A glyph waiting to be handed out: which face it is from, its index there, and its offsets (compact: a line holds hundreds). */
enum { Q_LATIN, Q_EXT, Q_BN, Q_AR };
typedef struct
{
    unsigned short idx;
    unsigned char face;
    short xo, yo, adv;
} QItem;

/* No function here may use more than 4 KB of stack: the kernel is linked without a stack probe (__chkstk). So the big buffers are
 * few and small, and the shapers' own buffers live in functions of their own (shape_*_items) that return before the next call. */
/* Keeps the compiler from merging a function with a big frame into its caller (their frames would add up). */
#define NOINLINE __attribute__((noinline))

#define QMAX 192      /* glyphs handed out from a queue: one Bengali syllable, or one chunk of a right-to-left line */
#define CHUNK_MAX 100 /* characters of a right-to-left line laid out at once (a longer line goes on in the next chunk) */

typedef struct
{
    const char *s;  /* next character to look at */
    int prev_bn;    /* the character before s was part of a Bengali run */
    int line_start; /* s is at the start of a line (or of the next chunk of a long one): check it for right-to-left letters */
    int line_rtl;   /* the line being laid out has strong right-to-left letters: it goes through the bidi algorithm */
    QItem q[QMAX];
    int qn, qi;
    const char *q_end; /* where the text continues after the queued glyphs */
} Lay;

static void lay_start(Lay *l, const char *s)
{
    l->s = s;
    l->prev_bn = 0;
    l->line_start = 1;
    l->line_rtl = 0;
    l->qn = l->qi = 0;
    l->q_end = s;
}

static short sat16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (short)v; }

static const Font *q_font(const Font *f, int face)
{
    switch (face)
    {
    case Q_EXT: return ext_face_for_size(f->size);
    case Q_BN: return bn_face_for(f); /* of the weight of the Latin font: regular, medium or bold */
    case Q_AR: return ar_face_for_size(f->size);
    }
    return f;
}

/* The glyph of a character that needs no shaping: the Latin font, the extra face, or '?' if no font has it. */
static void plain_item(const Font *f, unsigned cp, QItem *q)
{
    int e = (cp < f->first || cp > f->last) && cp < 0x10000 ? ext_index(cp) : -1;
    q->xo = q->yo = 0;
    if (e >= 0)
    {
        q->face = Q_EXT;
        q->idx = (unsigned short)e;
        q->adv = sat16(ext_face_for_size(f->size)->glyphs[e].advance);
    }
    else
    {
        q->face = Q_LATIN;
        q->idx = (unsigned short)(cp >= f->first && cp <= f->last ? cp - f->first : '?' - f->first);
        q->adv = sat16(f->glyphs[q->idx].advance);
    }
}

static void shaped_items(QItem *q, int face, const BnGlyphPos *g, int n)
{
    for (int i = 0; i < n; i++)
    {
        q[i].idx = g[i].gid;
        q[i].face = (unsigned char)face;
        q[i].xo = sat16(g[i].x_off);
        q[i].yo = sat16(g[i].y_off);
        q[i].adv = sat16(g[i].adv);
    }
}

/* Shape an Arabic run / a Bengali syllable into queue items. They write nothing if the glyphs are more than `room`; either way
 * they return how many glyphs the run has. (Separate functions: their glyph buffer is 1.5 KB of stack that is free again at once.) */
NOINLINE static int shape_ar_items(const Font *f, const unsigned *cps, int len, QItem *out, int room)
{
    BnGlyphPos tmp[OT_CAP];
    int cnt = ar_shape_run(cps, len, f->size, tmp, OT_CAP);
    if (cnt <= room)
        shaped_items(out, Q_AR, tmp, cnt);
    return cnt;
}

NOINLINE static int shape_bn_items(const Font *f, const unsigned *cps, int len, int word_start, QItem *out, int room)
{
    BnGlyphPos tmp[OT_CAP];
    int cnt = bn_shape_syllable_w(cps, len, word_start, bn_face_for(f)->size, bn_weight_of(f), tmp, OT_CAP);
    if (cnt <= room)
        shaped_items(out, Q_BN, tmp, cnt);
    return cnt;
}

/* 1 if the line that starts at s (up to the next line break) needs the bidi path: it has a strong right-to-left letter, or any
 * character of the Arabic script (even digits or a comma, which have no letter around them but are drawn from the Arabic face). */
static int line_has_rtl(const char *s)
{
    while (*s && *s != '\n')
    {
        unsigned cp = utf8_decode(&s);
        if (bidi_is_rtl(cp) || (cp >= 0x0600 && cp <= 0x077F && ar_is_arabic(cp)))
            return 1;
    }
    return 0;
}

/* A line with right-to-left letters: shape its runs, put them in visual order (bidi.h), queue the glyphs. At most CHUNK_MAX
 * characters; the queue then holds what was laid out and l->s is after it. */
NOINLINE static void lay_bidi_chunk(Lay *l, const Font *f)
{
    unsigned cps[CHUNK_MAX];
    unsigned short ends[CHUNK_MAX]; /* byte offset from l->s of the end of each character */
    unsigned char level[CHUNK_MAX];
    int n = 0, para;
    for (const char *q = l->s; *q && n < CHUNK_MAX;)
    {
        const char *a = q;
        unsigned c = utf8_decode(&a);
        if (c == '\n')
            break;
        cps[n] = c;
        ends[n++] = (unsigned short)(a - l->s);
        q = a;
    }
    bidi_resolve(cps, n, -1, level, &para);

    /* the runs, in logical order: an Arabic word, a Bengali syllable, or one character; each shaped on its own, into lg */
    QItem lg[QMAX];
    struct
    {
        unsigned short g0;
        unsigned char gn, level;
    } seg[CHUNK_MAX];
    int nseg = 0, ng = 0, i = 0, prev_bn = l->prev_bn;
    while (i < n)
    {
        unsigned cp = cps[i];
        int len = 1, cnt, room = QMAX - ng;
        if (ar_is_arabic(cp))
        {
            len = ar_run_len(cps + i, n - i);
            for (int k = 1; k < len; k++) /* a run is one level: digits inside an Arabic word are cut off */
                if (level[i + k] != level[i])
                {
                    len = k;
                    break;
                }
            cnt = shape_ar_items(f, cps + i, len, lg + ng, room);
            prev_bn = 0;
        }
        else if (bn_is_bengali(cp))
        {
            int m = 0;
            while (m < n - i && m < BN_SYL_MAX + 1 && bn_is_bengali(cps[i + m]))
                m++;
            len = bn_syllable_len(cps + i, m);
            for (int k = 1; k < len; k++)
                if (level[i + k] != level[i])
                {
                    len = k;
                    break;
                }
            cnt = shape_bn_items(f, cps + i, len, !prev_bn, lg + ng, room);
            prev_bn = 1;
        }
        else
        {
            cnt = 1;
            if (room >= 1)
                plain_item(f, (level[i] & 1) ? bidi_mirror(cp) : cp, &lg[ng]); /* ( ) < > ... swap their shape in right-to-left text */
            prev_bn = 0;
        }
        if (cnt > room || cnt > 255) /* the queue is full: this run goes in the next chunk (the first run always fits) */
        {
            if (nseg > 0)
                break;
            cnt = room < 255 ? room : 255; /* a single run that is more than the queue holds: cut */
        }
        seg[nseg].g0 = (unsigned short)ng;
        seg[nseg].gn = (unsigned char)cnt;
        seg[nseg].level = level[i];
        nseg++;
        ng += cnt;
        i += len;
    }

    /* the runs in visual order; the glyphs of a right-to-left run run backwards */
    unsigned char slevel[CHUNK_MAX];
    unsigned short vis[CHUNK_MAX];
    for (int k = 0; k < nseg; k++)
        slevel[k] = seg[k].level;
    bidi_reorder(slevel, nseg, vis);
    int qn = 0;
    for (int k = 0; k < nseg; k++)
    {
        int s = vis[k];
        if (seg[s].level & 1)
            for (int g = seg[s].g0 + seg[s].gn - 1; g >= seg[s].g0; g--)
                l->q[qn++] = lg[g];
        else
            for (int g = seg[s].g0; g < seg[s].g0 + seg[s].gn; g++)
                l->q[qn++] = lg[g];
    }
    l->qn = qn;
    l->qi = 0;
    l->q_end = l->s + ends[i - 1];
    l->s = l->q_end;
    l->prev_bn = prev_bn;
    l->line_start = 1; /* a line longer than a chunk goes on: its next chunk is looked at afresh */
}

/* Next glyph of the text, 0 at the end. After it, *end is where the text continues once everything made from the
 * same character group (one Latin character, one Bengali syllable, one chunk of a right-to-left line) has been handed out. */
static int lay_next(Lay *l, const Font *f, LayItem *it, const char **end)
{
    for (;;)
    {
        if (l->qi < l->qn)
        {
            const QItem *p = &l->q[l->qi++];
            const Font *face = q_font(f, p->face);
            it->font = face;
            it->g = &face->glyphs[p->idx];
            it->xo = p->xo;
            it->yo = p->yo;
            it->adv = p->adv;
            it->newline = 0;
            *end = l->qi == l->qn ? l->q_end : 0;
            return 1;
        }
        if (!*l->s)
            return 0;
        const char *p = l->s;
        unsigned cp = utf8_decode(&p);
        it->xo = it->yo = 0;
        it->newline = 0;
        if (cp == '\n')
        {
            l->s = p;
            l->prev_bn = 0;
            l->line_start = 1;
            it->newline = 1;
            it->font = f;
            it->g = 0;
            it->adv = 0;
            *end = p;
            return 1;
        }
        if (l->line_start)
        {
            l->line_start = 0;
            l->line_rtl = line_has_rtl(l->s);
        }
        if (l->line_rtl)
        {
            lay_bidi_chunk(l, f);
            continue; /* the queue has the glyphs now */
        }
        if (bn_is_bengali(cp))
        {
            unsigned cps[BN_SYL_MAX + 1]; /* one more than the longest syllable, to see where it ends */
            const char *ends[BN_SYL_MAX + 1];
            int n = 0;
            const char *q = l->s;
            while (*q && n < BN_SYL_MAX + 1)
            {
                const char *after = q;
                unsigned c = utf8_decode(&after);
                if (!bn_is_bengali(c))
                    break;
                cps[n] = c;
                ends[n++] = after;
                q = after;
            }
            int len = bn_syllable_len(cps, n);
            int cnt = shape_bn_items(f, cps, len, !l->prev_bn, l->q, QMAX);
            if (cnt > QMAX)
                cnt = 0;
            l->qn = cnt;
            l->qi = 0;
            l->q_end = ends[len - 1];
            l->s = l->q_end;
            l->prev_bn = 1;
            if (cnt == 0) /* nothing to draw for it */
            {
                *end = l->q_end;
                continue;
            }
            continue;
        }
        l->s = p;
        l->prev_bn = 0;
        *end = p;
        QItem one;
        plain_item(f, cp, &one);
        const Font *face = q_font(f, one.face);
        it->font = face;
        it->g = &face->glyphs[one.idx];
        it->adv = one.adv;
        return 1;
    }
}

int font_line_height(const Font *f) { return f ? f->ascent + f->descent : 0; }

/* Keep multiline drawing and measurement on the same baseline grid. */
static int text_line_step(const Font *f)
{
    if (!f)
        return 0;
    return font_line_height(f) + (f->size >= 20 ? 4 : 3);
}

static int text_line_gap(const Font *f)
{
    return f && f->size >= 20 ? 4 : 3;
}

/* Round a 1/16-pixel pen position to the nearest pixel, including negatives. */
static int round_pen_16(int pen)
{
    int pixel = pen / 16;
    int remainder = pen % 16;
    if (remainder >= 8)
        ++pixel;
    else if (remainder <= -8)
        --pixel;
    return pixel;
}

/* Round coverage multiplication instead of systematically biasing it down. */
static unsigned char scale_coverage(unsigned char coverage, unsigned char alpha)
{
    return (unsigned char)(((unsigned int)coverage * alpha + 127u) / 255u);
}

static void draw_glyph(const Font *f, const FontGlyph *g, int x, int y, GfxColor c, int alpha)
{
    const unsigned char *src = f->bitmap + g->offset;
    /* Only visit the part of the glyph inside the clip: a window redrawn in a small area still asks for all of
     * its text, and most of it lies outside. */
    GfxClip clip = gfx_get_clip();
    int gx = x + g->bx, gy = y + g->by;
    int row0 = clip.y0 - gy > 0 ? clip.y0 - gy : 0, row1 = clip.y1 - gy < g->h ? clip.y1 - gy : g->h;
    int col0 = clip.x0 - gx > 0 ? clip.x0 - gx : 0, col1 = clip.x1 - gx < g->w ? clip.x1 - gx : g->w;
    for (int row = row0; row < row1; row++)
        for (int col = col0; col < col1; col++)
        {
            int cov = src[row * g->w + col];
            if (cov)
                gfx_blend_pixel(x + g->bx + col, y + g->by + row, c,
                                scale_coverage((unsigned char)cov, (unsigned char)alpha));
        }
}

void gfx_text_alpha(const Font *f, int x, int y, const char *s, GfxColor c, unsigned char alpha)
{
    if (!f || !s || alpha == 0)
        return;
    int pen = x * 16; /* pen position in 1/16 px: exact spacing, no rounding drift */
    int lh = text_line_step(f);
    Lay l;
    LayItem it;
    const char *end;
    lay_start(&l, s);
    while (lay_next(&l, f, &it, &end))
    {
        if (it.newline)
        {
            pen = x * 16;
            y += lh;
            continue;
        }
        if (it.g->w)
            draw_glyph(it.font, it.g, round_pen_16(pen + it.xo), y - round_pen_16(it.yo), c, alpha);
        pen += it.adv;
    }
}

void gfx_text(const Font *f, int x, int y, const char *s, GfxColor c)
{
    gfx_text_alpha(f, x, y, s, c, 255);
}

int gfx_text_w(const Font *f, const char *s)
{
    if (!f || !s)
        return 0;
    int best = 0, cur = 0;
    Lay l;
    LayItem it;
    const char *end;
    lay_start(&l, s);
    while (lay_next(&l, f, &it, &end))
    {
        if (it.newline)
        {
            cur = 0;
            continue;
        }
        cur += it.adv;
        if (cur > best)
            best = cur;
    }
    return (best + 8) >> 4;
}

int gfx_text_h(const Font *f, const char *s)
{
    if (!f || !s || !*s)
        return 0;
    int lines = 1;
    for (; *s; s++)
        if (*s == '\n')
            lines++;
    return lines * text_line_step(f) - text_line_gap(f);
}

/* The bytes of a line with right-to-left letters that fit: the text that goes is the end of the line in the order typed, so the
 * answer is a prefix in logical order, found by measuring prefixes (their layout differs from the whole line's). */
NOINLINE static int fit_rtl(const Font *f, const char *s, int max_w)
{
    char tmp[CHUNK_MAX * 4 + 1];
    int ends[CHUNK_MAX], n = 0;
    for (const char *q = s; *q && *q != '\n' && n < CHUNK_MAX;)
    {
        utf8_decode(&q);
        ends[n++] = (int)(q - s);
    }
    int lo = 0, hi = n; /* the most characters that fit: width grows with the prefix, so a binary search */
    while (lo < hi)
    {
        int mid = (lo + hi + 1) / 2;
        for (int i = 0; i < ends[mid - 1]; i++)
            tmp[i] = s[i];
        tmp[ends[mid - 1]] = 0;
        if (gfx_text_w(f, tmp) <= max_w)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo ? ends[lo - 1] : 0;
}

/* Bytes of `s` that fit into max_w pixels, cut between characters (a Bengali syllable is never split). */
int gfx_text_fit(const Font *f, const char *s, int max_w)
{
    if (!f || !s || max_w <= 0)
        return 0;
    if (line_has_rtl(s))
        return fit_rtl(f, s, max_w);
    int cur = 0, fit = 0;
    Lay l;
    LayItem it;
    const char *end;
    lay_start(&l, s);
    while (lay_next(&l, f, &it, &end))
    {
        if (it.newline)
            break;
        cur += it.adv;
        if (((cur + 8) >> 4) > max_w)
            break;
        if (end)
            fit = (int)(end - s);
    }
    return fit;
}
