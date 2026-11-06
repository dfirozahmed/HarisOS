/* Host test for bidi.c: bidi classes, the levels the Unicode Bidirectional Algorithm gives, the visual order and mirroring. */
#include "bidi.h"
#include "uni_data.h"
#include "utf8.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* the levels of a UTF-8 text as a string of digits ("0011100"); *para gets the paragraph level */
static const char *levels(const char *text, int base, int *para)
{
    static char out[300];
    unsigned cps[256];
    unsigned char lv[256];
    int n = 0;
    while (*text && n < 256)
        cps[n++] = utf8_decode(&text);
    int p;
    bidi_resolve(cps, n, base, lv, &p);
    for (int i = 0; i < n; i++)
        out[i] = (char)('0' + lv[i]);
    out[n] = 0;
    if (para)
        *para = p;
    return out;
}

/* the visual order of items with the given levels (digits), as a string of item numbers */
static const char *order(const char *lv)
{
    static char out[300];
    unsigned char level[256];
    unsigned short vis[256];
    int n = (int)strlen(lv);
    for (int i = 0; i < n; i++)
        level[i] = (unsigned char)(lv[i] - '0');
    bidi_reorder(level, n, vis);
    for (int i = 0; i < n; i++)
        out[i] = (char)('0' + vis[i]);
    out[n] = 0;
    return out;
}

#define LEVELS(text, base, expect, expect_para) \
    do { int p; const char *r = levels(text, base, &p); \
         if (strcmp(r, expect) != 0 || p != (expect_para)) { printf("FAIL line %d: \"%s\" -> %s (paragraph %d), expected %s (%d)\n", __LINE__, text, r, p, expect, expect_para); fails++; } } while (0)

int main(void)
{
    /* ---- classes ---- */
    CHECK(bidi_class('a') == BIDI_L && bidi_class(0x0431) == BIDI_L && bidi_class(0x0995) == BIDI_L); /* Latin, Cyrillic, Bengali */
    CHECK(bidi_class(0x0627) == BIDI_AL && bidi_class(0x0645) == BIDI_AL);                             /* Arabic letters */
    CHECK(bidi_class(0x05D0) == BIDI_R);                                                               /* Hebrew */
    CHECK(bidi_class('1') == BIDI_EN && bidi_class(0x0661) == BIDI_AN);
    CHECK(bidi_class(' ') == BIDI_WS && bidi_class('(') == BIDI_ON && bidi_class('!') == BIDI_ON);
    CHECK(bidi_class(',') == BIDI_CS && bidi_class('$') == BIDI_ET && bidi_class('+') == BIDI_ES && bidi_class('%') == BIDI_ET);
    CHECK(bidi_class(0x064E) == BIDI_NSM && bidi_class(0x200D) == BIDI_BN && bidi_class('\n') == BIDI_B);
    CHECK(bidi_class(0x1F600) == BIDI_L); /* above U+FFFF: left to right */
    CHECK(bidi_is_rtl(0x0627) && bidi_is_rtl(0x05D0) && !bidi_is_rtl('a') && !bidi_is_rtl('1') && !bidi_is_rtl(0x0661) && !bidi_is_rtl(' '));

    /* ---- levels ---- */
    LEVELS("abc", -1, "000", 0);
    LEVELS("", -1, "", 0);
    LEVELS("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7", -1, "11111", 1);                             /* مرحبا: all right to left */
    LEVELS("abc \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 def", -1, "0000111110000", 0);             /* abc مرحبا def: 3 + space + 5 + space + 3 characters */
    LEVELS("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 abc", -1, "111111222", 1);                     /* مرحبا abc: the paragraph is right to left */
    LEVELS("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 123", -1, "111111222", 1);                     /* مرحبا 123: W2 makes the digits Arabic digits, level 2 */
    LEVELS("abc 123", -1, "0000000", 0);                                                           /* W7: digits after Latin stay left to right */
    LEVELS("\xd9\x85\xd8\xb1 (\xd8\xa8\xd8\xa7)", -1, "1111111", 1);                               /* مر (با): the space and both brackets are right to left */
    LEVELS("abc \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 ", -1, "000011111" "0", 0);               /* trailing space: the paragraph's level */
    LEVELS("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7!", -1, "111111", 1);                            /* ! after an Arabic word: right to left */
    LEVELS("abc", 1, "222", 1);                                                                    /* forced right to left paragraph */
    LEVELS("\xd9\x85\xd8\xb1", 0, "11", 0);                                                        /* forced left to right paragraph */
    LEVELS("\xd9\x85\xd9\x8e", -1, "11", 1);                                                       /* a mark takes the level of its letter */
    LEVELS("1+2", -1, "000", 0);
    LEVELS("\xd9\x85 1+2", -1, "11212", 1);                                                        /* م 1+2: the digits are Arabic digits (W2), the + between them is not joined (W4 is for European digits) */

    /* ---- the visual order ---- */
    CHECK(strcmp(order("000"), "012") == 0);
    CHECK(strcmp(order("001110"), "014325") == 0);   /* a right to left run in the middle is reversed */
    CHECK(strcmp(order("111"), "210") == 0);
    CHECK(strcmp(order("11221"), "42310") == 0);     /* level 2 inside level 1: reversed twice, i.e. kept in order */
    CHECK(strcmp(order("0112"), "0321") == 0);       /* level 2 alone is not reversed; the level 1 run (items 1-3) is */
    CHECK(strcmp(order(""), "") == 0);
    CHECK(strcmp(order("1"), "0") == 0);

    /* ---- mirroring ---- */
    CHECK(bidi_mirror('(') == ')' && bidi_mirror(')') == '(' && bidi_mirror('<') == '>' && bidi_mirror('[') == ']');
    CHECK(bidi_mirror(0xAB) == 0xBB && bidi_mirror(0xBB) == 0xAB); /* « » */
    CHECK(bidi_mirror('a') == 'a' && bidi_mirror(0x0627) == 0x0627);

    /* ---- the tables ---- */
    for (unsigned i = 1; i < uni_bidi_n; i++)
        CHECK(uni_bidi_start[i - 1] < uni_bidi_start[i]);
    for (unsigned i = 1; i < uni_join_n; i++)
        CHECK(uni_join_end[i - 1] < uni_join_start[i]);
    for (unsigned i = 1; i < uni_mirror_n; i++)
        CHECK(uni_mirror_from[i - 1] < uni_mirror_from[i]);

    if (fails == 0)
        printf("bidi tests passed\n");
    return fails ? 1 : 0;
}
