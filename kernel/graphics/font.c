/* HarisOS text rendering: see include/font.h.
 * Glyph bitmaps are 8-bit coverage; each pixel is blended with the background.
 *
 * Strings are UTF-8. ASCII (and every character the Latin font has no glyph for, shown as '?') is drawn one byte
 * at a time from the Latin font; a run of Bengali is cut into syllables, each shaped by shape_bn.c, and drawn from
 * the Bengali font of the same size. Drawing, measuring and fitting all walk the text through lay_next, so they
 * always agree on where every glyph is. */
#include "font.h"
#include "font_ext.h"
#include "shape_bn.h"
#include "utf8.h"

static const FontGlyph *glyph_for(const Font *f, unsigned ch)
{
    if (ch < f->first || ch > f->last)
        ch = '?';
    return &f->glyphs[ch - f->first];
}

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

/* The extra face of the size nearest to the Latin font's (the same five sizes as the Bengali faces). */
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

typedef struct
{
    const char *s;     /* next character to look at */
    int prev_bn;       /* the character before s was part of a Bengali run */
    BnGlyphPos q[BN_SYL_MAX * 2 + 4]; /* glyphs of the syllable being handed out */
    int qn, qi;
    const Font *qface;
    const char *q_end; /* where the text continues after the queued syllable */
} Lay;

static void lay_start(Lay *l, const char *s)
{
    l->s = s;
    l->prev_bn = 0;
    l->qn = l->qi = 0;
    l->qface = 0;
    l->q_end = s;
}

/* Next glyph of the text, 0 at the end. After it, *end is where the text continues once everything made from the
 * same character group (one Latin character, one Bengali syllable) has been handed out. */
static int lay_next(Lay *l, const Font *f, LayItem *it, const char **end)
{
    if (l->qi < l->qn)
    {
        const BnGlyphPos *p = &l->q[l->qi++];
        it->font = l->qface;
        it->g = &l->qface->glyphs[p->gid];
        it->xo = p->x_off;
        it->yo = p->y_off;
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
        it->newline = 1;
        it->font = f;
        it->g = 0;
        it->adv = 0;
        *end = p;
        return 1;
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
        l->qface = bn_face_for_size(f->size);
        l->qn = bn_shape_syllable(cps, len, !l->prev_bn, l->qface->size, l->q, (int)(sizeof l->q / sizeof l->q[0]));
        l->qi = 0;
        l->q_end = ends[len - 1];
        l->s = l->q_end;
        l->prev_bn = 1;
        if (l->qn > 0)
            return lay_next(l, f, it, end);
        *end = l->q_end; /* nothing to draw for it */
        return lay_next(l, f, it, end);
    }
    l->s = p;
    l->prev_bn = 0;
    *end = p;
    if ((cp < f->first || cp > f->last) && cp < 0x10000) /* not in the Latin font: Latin-1, Greek, Cyrillic ... from the extra face */
    {
        int e = ext_index(cp);
        if (e >= 0)
        {
            const Font *face = ext_face_for_size(f->size);
            it->font = face;
            it->g = &face->glyphs[e];
            it->adv = it->g->advance;
            return 1;
        }
    }
    it->font = f;
    it->g = glyph_for(f, cp); /* '?' for what no font has */
    it->adv = it->g->advance;
    return 1;
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

/* Bytes of `s` that fit into max_w pixels, cut between characters (a Bengali syllable is never split). */
int gfx_text_fit(const Font *f, const char *s, int max_w)
{
    if (!f || !s || max_w <= 0)
        return 0;
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
