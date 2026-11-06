/* ============================================================
 * HarisOS Bengali text shaping
 *
 * Bengali is not drawn one character at a time: vowel signs sit before or around the consonant they belong to
 * (the "i" and "e" signs are typed after it but drawn before it), "র্" turns into a hook above the next consonant,
 * and consonant + hasanta + consonant joins into a conjunct. The font describes all that in its OpenType tables;
 * this module follows the same steps as the HarfBuzz "Indic" shaper:
 *
 *   1. split the text into syllables            bn_syllable_len
 *   2. reorder inside a syllable (reph, pre-base vowel signs, split vowels)
 *   3. run the font's GSUB lookups (which glyphs to use), then GPOS (where to put them)
 *
 * Text rendering (font.c) cuts a UTF-8 string into code points, hands each Bengali run to these functions and
 * draws the resulting glyphs from font_bn_<size> (include/font_bn.h).
 * ============================================================ */
#ifndef HARIS_SHAPE_BN_H
#define HARIS_SHAPE_BN_H

#include "font.h"

#define BN_SYL_MAX 32 /* longest syllable handed to bn_shape_syllable; longer ones are cut here */

typedef struct
{
    unsigned short gid; /* glyph id in font_bn_<size> */
    int x_off;          /* extra horizontal offset of the glyph from the pen, 1/16 pixel */
    int y_off;          /* vertical offset, 1/16 pixel, positive = up */
    int adv;            /* how far the pen moves after this glyph, 1/16 pixel (0 for marks) */
} BnGlyphPos;

/* 1 if the code point belongs to a Bengali run: the Bengali letters, signs and digits the font has glyphs
 * for, the danda signs and the zero width joiners. Everything else is drawn by the Latin font. */
int bn_is_bengali(unsigned cp);

/* Number of code points (>= 1, <= BN_SYL_MAX) in the syllable at the start of cp[0..n). n must be >= 1. */
int bn_syllable_len(const unsigned *cp, int n);

/* Shape one syllable (as cut by bn_syllable_len) for a font of `size` pixels (12, 14, 16, 20 or 32: any other
 * value is used as is for scaling, but only those have bitmaps). word_start = 1 if the syllable begins a word
 * (the font uses a different "e" sign there). Writes at most max_out glyphs and returns how many. */
int bn_shape_syllable(const unsigned *cp, int n, int word_start, int size, BnGlyphPos *out, int max_out);

/* The Bengali face for a Latin font of this nominal size: the nearest of the five sizes. */
const Font *bn_face_for_size(int size);

#endif
