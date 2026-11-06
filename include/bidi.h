/* ============================================================
 * HarisOS bidirectional text (the Unicode Bidirectional Algorithm, UAX #9, without the explicit embeddings and isolates)
 *
 * Arabic runs right to left, Latin and Bengali left to right, numbers left to right inside a right-to-left run. The algorithm
 * gives every character an embedding level (even = left to right, odd = right to left), and says in what order the
 * characters (or, for text that is shaped first, the shaped runs) stand on the line.
 *
 *   bidi_resolve:  the level of each character of a paragraph (a line of text): rules P2-P3 (direction from the first strong
 *                  letter), W1-W7 (numbers, marks, separators), N1-N2 (neutrals such as spaces and punctuation), I1-I2, L1
 *   bidi_reorder:  rule L2: the visual order of items whose levels are given (reverse every run at each level, from the highest
 *                  down to the lowest odd one)
 *   bidi_mirror:   rule L4: the character that stands in place of a mirrored one in a right-to-left run: ( -> ), < -> >
 * ============================================================ */
#ifndef HARIS_BIDI_H
#define HARIS_BIDI_H

/* The bidi class of a character (BIDI_* of uni_data.h; up to U+FFFF, larger code points are L). */
int bidi_class(unsigned cp);

/* 1 if the character is strongly right to left (class R or AL): the text it is in needs the algorithm. */
int bidi_is_rtl(unsigned cp);

/* The levels of cp[0..n) (n <= 256), written to level[0..n). base: -1 = from the first strong letter (left to right if there is none),
 * 0 = left to right, 1 = right to left. *para_level gets the paragraph's level (0 or 1). */
void bidi_resolve(const unsigned *cp, int n, int base, unsigned char *level, int *para_level);

/* vis[k] = the index of the item that stands k-th from the left, for items 0..n-1 with the given levels (n <= 256). */
void bidi_reorder(const unsigned char *level, int n, unsigned short *vis);

/* The mirrored form of a character, or the character itself. */
unsigned bidi_mirror(unsigned cp);

#endif
