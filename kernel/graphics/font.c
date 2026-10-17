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

static void draw_glyph(const Font *f, const FontGlyph *g, int x, int y, GfxColor c, int alpha)
{
    const unsigned char *src = f->bitmap + g->offset;
    for (int row = 0; row < g->h; row++)
        for (int col = 0; col < g->w; col++)
        {
            int cov = src[row * g->w + col];
            if (cov)
                gfx_blend_pixel(x + g->bx + col, y + g->by + row, c, (unsigned char)(cov * alpha / 255));
        }
}

void gfx_text_alpha(const Font *f, int x, int y, const char *s, GfxColor c, unsigned char alpha)
{
    if (!f || !s || alpha == 0)
        return;
    int pen = x * 16; /* pen position in 1/16 px: exact spacing, no rounding drift */
    int lh = font_line_height(f) + (f->size >= 20 ? 4 : 3);
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
            draw_glyph(f, g, (pen + 8) >> 4, y, c, alpha);
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
    int lh = font_line_height(f) + (f->size >= 20 ? 4 : 3);
    return lines * lh - (f->size >= 20 ? 4 : 3);
}

int gfx_text_fit(const Font *f, const char *s, int max_w)
{
    if (!f || !s)
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
