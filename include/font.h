/* ============================================================
 * HarisOS font + text rendering
 *
 * Smooth (anti-aliased) text. Fonts are pre-rendered 8-bit coverage bitmaps
 * produced by tools/genfont.py from a TrueType file, so there is no font
 * library in the kernel: drawing a glyph is just "blend color by coverage".
 *
 * Use it through the gfx layer:
 *     gfx_text(&font_ui_14, x, y, "Hello", gfx_rgb(255,255,255));
 *
 * Built-in fonts (Poppins):
 *     font_ui_12 font_ui_14 font_ui_16 font_ui_20 font_ui_32   regular
 *     font_med_14 font_med_16                                  medium
 *     font_bold_20                                             bold
 * ============================================================ */
#ifndef HARIS_FONT_H
#define HARIS_FONT_H

#include "gfx.h"

typedef struct
{
    unsigned short offset;  /* start of this glyph's bitmap in Font.bitmap */
    unsigned char w, h;     /* bitmap size in pixels (0 = nothing to draw, e.g. space) */
    signed char bx;         /* bitmap left edge relative to the pen position */
    signed char by;         /* bitmap top edge, measured from the top of the line */
    unsigned short advance; /* pen advance in 1/16 pixel */
} FontGlyph;

typedef struct
{
    const char *name;
    unsigned char size;        /* nominal pixel size */
    unsigned char ascent;      /* baseline is `ascent` pixels below the top of the line */
    unsigned char descent;
    unsigned char first, last; /* first/last character code covered */
    const FontGlyph *glyphs;
    const unsigned char *bitmap;
} Font;

extern const Font font_ui_12, font_ui_14, font_ui_16, font_ui_20, font_ui_32;
extern const Font font_med_14, font_med_16;
extern const Font font_bold_20;

/* Height of one line of text in this font (ascent + descent). */
int font_line_height(const Font *f);

/* ---- drawing (x, y = top-left corner of the text box) -------- */
void gfx_text(const Font *f, int x, int y, const char *s, GfxColor c);
void gfx_text_alpha(const Font *f, int x, int y, const char *s, GfxColor c, unsigned char alpha);

/* ---- measuring ---------------------------------------------- */
int gfx_text_w(const Font *f, const char *s); /* width of the longest line */
int gfx_text_h(const Font *f, const char *s); /* height of all lines */

/* How many characters of `s` fit into max_w pixels (for clipping/ellipsis). */
int gfx_text_fit(const Font *f, const char *s, int max_w);

#endif
