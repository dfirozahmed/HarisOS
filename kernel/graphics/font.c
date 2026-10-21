/* HarisOS text rendering: see include/font.h.
 * Glyph bitmaps are 8-bit coverage; each pixel is blended with the background. */
#include "font.h"

static const FontGlyph *glyph_for(const Font *f, unsigned char ch)
{
    if (ch < f->first || ch > f->last)
        ch = '?';
    return &f->glyphs[ch - f->first];
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
    for (; *s; s++)
    {
        if (*s == '\n')
        {
            pen = x * 16;
            y += lh;
            continue;
        }
        const FontGlyph *g = glyph_for(f, (unsigned char)*s);
        if (g->w)
            draw_glyph(f, g, round_pen_16(pen), y, c, alpha);
        pen += g->advance;
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
    for (; *s; s++)
    {
        if (*s == '\n')
        {
            cur = 0;
            continue;
        }
        cur += glyph_for(f, (unsigned char)*s)->advance;
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

int gfx_text_fit(const Font *f, const char *s, int max_w)
{
    if (!f || !s || max_w <= 0)
        return 0;
    int cur = 0, n = 0;
    for (; *s && *s != '\n'; s++, n++)
    {
        cur += glyph_for(f, (unsigned char)*s)->advance;
        if (((cur + 8) >> 4) > max_w)
            break;
    }
    return n;
}
