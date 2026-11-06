/* ============================================================
 * HarisOS OpenType layout engine (GSUB / GPOS / GDEF), for the shapers of the complex scripts
 *
 * A font's own tables, embedded unchanged, tell which glyphs to use (GSUB: letters in their joined forms, ligatures, conjuncts)
 * and where to put them (GPOS: marks over letters, kerning). This engine runs those tables on a short buffer of glyphs;
 * a script shaper (shape_bn.c: Bengali, shape_ar.c: Arabic) decides the order of the glyphs, which features run in which
 * order and on which glyphs (the per-glyph mask), exactly as HarfBuzz's shapers do.
 *
 * The tables are our own embedded constants: reads are not bounds-checked. What depends on the text is (buffer capacity,
 * input counts, nesting depth).
 * ============================================================ */
#ifndef HARIS_OT_H
#define HARIS_OT_H

#define OT_CAP 96   /* glyphs in a buffer */
#define OT_MASK_ALL 0x8000u /* every glyph has this bit; a feature that applies to all glyphs runs with it */

/* A font's tables and the lists that say which lookups run when (made by the font generators, include/font_*.h). */
typedef struct
{
    const unsigned char *gsub, *gpos, *gdef;
    const unsigned short *gsub_lookups; /* lookup indices of all stages, one after the other */
    const unsigned short *gsub_stage;   /* stage s runs gsub_lookups[gsub_stage[s] .. gsub_stage[s+1]) */
    const unsigned char *gsub_init;     /* parallel to gsub_lookups: 1 = the lookup runs with the "init" mask (may be 0 = none) */
    const unsigned short *gpos_lookups;
    unsigned gpos_n;
    const unsigned short *adv; /* advance of every glyph, font design units */
    int upem;                  /* design units per em */
    const unsigned short *cmap_cp, *cmap_gid; /* code point (< 0x10000) -> glyph id, sorted */
    unsigned cmap_n;
} OtFont;

typedef struct
{
    unsigned short gid, mask; /* mask: the glyph takes part in a lookup only if it shares a bit with the lookup's mask */
    unsigned char cls;        /* GDEF class: 0 none, 1 base, 2 ligature, 3 mark, 4 component */
    unsigned char flags;      /* the shaper's own marks (they survive substitutions: a ligature has the flags of all its parts) */
    short attach;             /* GPOS: index of the glyph this mark is attached to, -1 = none */
    int xoff, yoff, xadv;     /* GPOS adjustments, design units */
    int ax, ay;               /* GPOS: anchor difference to the glyph it is attached to */
} OtGlyph;

typedef struct
{
    const OtFont *font;
    const unsigned char *lookups; /* LookupList of GSUB or GPOS (whichever runs now) */
    int gpos;
    unsigned lookup_mask;
    int depth;
    int len;
    OtGlyph buf[OT_CAP];
} OtBuf;

/* The glyph of a code point (0 = the font has none). */
unsigned ot_cmap(const OtFont *f, unsigned cp);

void ot_begin(OtBuf *b, const OtFont *f);
/* Append a glyph; 0 if the buffer is full. */
int ot_add(OtBuf *b, unsigned gid, unsigned mask, unsigned char flags);

/* Run the GSUB lookups of one stage. They run in the order of their index; each only on glyphs that share a bit with `mask`
 * (a lookup flagged "init" in the font's list uses init_mask instead). */
void ot_gsub_stage(OtBuf *b, int stage, unsigned mask, unsigned init_mask);
/* Run all GPOS lookups of the list, on every glyph. */
void ot_gpos(OtBuf *b);
/* After GPOS: marks have no width of their own, and attached marks move to their anchor. rtl = 1 if the text runs right to left
 * (the glyphs are in logical order; a mark and its base share one pen position when the glyphs are drawn right to left). */
void ot_finish(OtBuf *b, int rtl);

#endif
