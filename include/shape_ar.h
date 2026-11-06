/* ============================================================
 * HarisOS Arabic text shaping
 *
 * Arabic letters join: a letter has up to four shapes (alone, at the start, in the middle or at the end of a word), some pairs
 * are ligatures (لا), and marks (fatha, shadda ...) sit on the letters. The font's own OpenType tables know all of that; this
 * module follows the Arabic shaper of HarfBuzz:
 *
 *   1. look up each letter's joining type (Unicode ArabicShaping.txt: dual, right, transparent ...)
 *   2. from the neighbours decide the shape each letter takes: isolated, initial, medial or final
 *   3. run the font's GSUB (ccmp, then fina / medi / init each only on the letters in that shape, then rlig / liga),
 *      and GPOS (kerning, marks) with the engine in ot.c
 *
 * The glyphs come out in logical order (the order typed); drawing them right to left is the job of the caller (font.c), which
 * also decides, with the bidi algorithm (bidi.h), where an Arabic word stands among Latin text.
 * ============================================================ */
#ifndef HARIS_SHAPE_AR_H
#define HARIS_SHAPE_AR_H

#include "font.h"
#include "shape_bn.h" /* BnGlyphPos: a glyph with its offsets and advance */

#define AR_RUN_MAX 40 /* the longest run of Arabic characters shaped at once; a longer word is cut here */

/* 1 if the code point is drawn from the Arabic font: the Arabic letters, marks, digits and punctuation it has glyphs for, and
 * the zero width joiners. */
int ar_is_arabic(unsigned cp);

/* Number of code points (>= 1, <= AR_RUN_MAX) of the Arabic run at the start of cp[0..n): characters for which ar_is_arabic is
 * true. n must be >= 1 and cp[0] must be Arabic. */
int ar_run_len(const unsigned *cp, int n);

/* Shape a run (as cut by ar_run_len) for a Latin font of `size` pixels. The glyphs of the Arabic face (ar_face_for_size) are written
 * in logical order, first letter first, with offsets and advances in 1/16 pixel for drawing from right to left: a mark has no
 * advance and shares the pen position of its letter. Writes at most max_out glyphs and returns how many. */
int ar_shape_run(const unsigned *cp, int n, int size, BnGlyphPos *out, int max_out);

/* The Arabic face for a Latin font of this nominal size: the nearest of the five sizes. */
const Font *ar_face_for_size(int size);

#endif
