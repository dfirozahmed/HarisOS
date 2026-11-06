#include "font.h"
#include "font_bn.h"
#include "font_ext.h"
#include "font_ar.h"
#include "shape_ar.h"
#include "shape_bn.h"
#include "utf8.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

GfxColor gfx_rgb(unsigned char r, unsigned char g, unsigned char b)
{
    return 0xFF000000u | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
}

static int blend_calls;
static unsigned char min_alpha = 255;
static int min_x = 2147483647;

void gfx_blend_pixel(int x, int y, GfxColor color, unsigned char alpha)
{
    (void)y;
    (void)color;
    ++blend_calls;
    if (alpha < min_alpha) min_alpha = alpha;
    if (x < min_x) min_x = x;
}

static GfxClip test_clip = {-100000, -100000, 100000, 100000};
GfxClip gfx_get_clip(void) { return test_clip; }

int main(void)
{
    assert(font_line_height(NULL) == 0);
    assert(font_line_height(&font_ui_14) > 0);
    assert(gfx_text_w(NULL, "Hello") == 0);
    assert(gfx_text_w(&font_ui_14, NULL) == 0);
    assert(gfx_text_w(&font_ui_14, "Hello") > 0);
    assert(gfx_text_w(&font_ui_14, "Hello\nHi") == gfx_text_w(&font_ui_14, "Hello"));
    assert(gfx_text_h(&font_ui_14, "") == 0);
    assert(gfx_text_h(&font_ui_14, "A") == font_line_height(&font_ui_14));
    assert(gfx_text_h(&font_ui_14, "A\nB") > gfx_text_h(&font_ui_14, "A"));
    assert(gfx_text_fit(&font_ui_14, "Hello", 0) == 0);
    assert(gfx_text_fit(&font_ui_14, "Hello", -1) == 0);
    assert(gfx_text_fit(&font_ui_14, "Hello", gfx_text_w(&font_ui_14, "Hello")) == 5);

    gfx_text_alpha(&font_ui_14, 10, 5, "A", gfx_rgb(255, 255, 255), 255);
    assert(blend_calls > 0);
    assert(min_x >= 0 && min_x < 100); /* glyph pixels stay near the requested origin */

    int calls_before = blend_calls;
    gfx_text_alpha(&font_ui_14, 10, 5, "A", gfx_rgb(255, 255, 255), 0);
    assert(blend_calls == calls_before); /* zero alpha is a no-op */

    {   /* text outside the clip is not even visited; text inside is drawn only where the clip allows */
        int full = blend_calls;
        gfx_text_alpha(&font_ui_14, 10, 5, "A", gfx_rgb(255, 255, 255), 255);
        full = blend_calls - full;
        test_clip = (GfxClip){500, 500, 600, 600};
        calls_before = blend_calls;
        gfx_text_alpha(&font_ui_14, 10, 5, "A", gfx_rgb(255, 255, 255), 255);
        assert(blend_calls == calls_before);
        test_clip = (GfxClip){10, 5, 14, 9}; /* a small window onto the glyph */
        calls_before = blend_calls;
        gfx_text_alpha(&font_ui_14, 10, 5, "A", gfx_rgb(255, 255, 255), 255);
        assert(blend_calls - calls_before < full);
    }

    /* ---- UTF-8 ---- */
    {
        const char *p = "A";
        assert(utf8_decode(&p) == 'A' && *p == 0);
        p = "\xe0\xa6\x95"; /* U+0995 */
        assert(utf8_decode(&p) == 0x995 && *p == 0);
        p = "\xf0\x9f\x98\x80"; /* U+1F600 */
        assert(utf8_decode(&p) == 0x1F600 && *p == 0);
        const char *bad[] = {"\xff", "\x80", "\xc0\x80", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe0\xa6", "\xe0\xa6X"};
        for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        {
            p = bad[i];
            assert(utf8_decode(&p) == UTF8_BAD);
            assert(p > bad[i] && p <= bad[i] + strlen(bad[i])); /* consumed something, never past the 0 */
        }
        char buf[8];
        assert(utf8_encode(0x41, buf) == 1 && utf8_encode(0x995, buf) == 3 && utf8_encode(0x1F600, buf) == 4);
        assert(utf8_encode(0xD800, buf) == 3 && (unsigned char)buf[0] == 0xEF); /* surrogate -> U+FFFD */
        unsigned short u16[8];
        assert(utf16_from_utf8("A\xe0\xa6\x95\xf0\x9f\x98\x80", u16, 8) == 4 && u16[1] == 0x995 && u16[2] == 0xD83D && u16[3] == 0xDE00);
        unsigned short small[3];
        assert(utf16_from_utf8("abcd", small, 3) == -1);
        char back[16];
        assert(utf8_from_utf16(u16, 4, back, sizeof back) == 8 && strcmp(back, "A\xe0\xa6\x95\xf0\x9f\x98\x80") == 0);
        assert(utf8_from_utf16(u16, 4, back, 5) == 4 && strcmp(back, "A\xe0\xa6\x95") == 0); /* full output: stops between characters */
        unsigned short lone[1] = {0xD800};
        assert(utf8_from_utf16(lone, 1, back, sizeof back) == 3 && (unsigned char)back[0] == 0xEF);
    }

    /* ---- Bengali: classification and syllables ---- */
    {
        assert(bn_is_bengali(0x995) && bn_is_bengali(0x9CD) && bn_is_bengali(0x200D) && bn_is_bengali(0x964));
        assert(!bn_is_bengali('A') && !bn_is_bengali(' ') && !bn_is_bengali(0x0900) && !bn_is_bengali(0x4E2D));
        unsigned ka_i[] = {0x995, 0x9BF}, conj[] = {0x995, 0x9CD, 0x995, 0x9BE}, reph[] = {0x9B0, 0x9CD, 0x995};
        unsigned stop[] = {0x995, ' '}, digit[] = {0x9E6, 0x9E7}, tail[] = {0x995, 0x9CD}, ou[] = {0x985, 0x982};
        assert(bn_syllable_len(ka_i, 2) == 2 && bn_syllable_len(conj, 4) == 4 && bn_syllable_len(reph, 3) == 3);
        assert(bn_syllable_len(stop, 2) == 1 && bn_syllable_len(digit, 2) == 1 && bn_syllable_len(tail, 2) == 2);
        assert(bn_syllable_len(ou, 2) == 2);
        unsigned lots[BN_SYL_MAX + 8];
        for (int i = 0; i < BN_SYL_MAX + 8; i++)
            lots[i] = i % 2 ? 0x9CD : 0x995;
        assert(bn_syllable_len(lots, BN_SYL_MAX + 8) <= BN_SYL_MAX);
    }

    /* ---- Bengali: shaping (glyph ids come from the font's own cmap) ---- */
    {
        unsigned KA = 0x995, SSA = 0x9B7, MA = 0x9AE, RA = 0x9B0, HAS = 0x9CD, ISIGN = 0x9BF, ESIGN = 0x9C7, AASIGN = 0x9BE;
        BnGlyphPos g[16];
        unsigned ka_gid = 0, i_gid = 0, e_gid = 0, aa_gid = 0, ma_gid = 0;
        for (unsigned i = 0; i < bn_cmap_n; i++)
        {
            if (bn_cmap_cp[i] == KA) ka_gid = bn_cmap_gid[i];
            if (bn_cmap_cp[i] == ISIGN) i_gid = bn_cmap_gid[i];
            if (bn_cmap_cp[i] == ESIGN) e_gid = bn_cmap_gid[i];
            if (bn_cmap_cp[i] == AASIGN) aa_gid = bn_cmap_gid[i];
            if (bn_cmap_cp[i] == MA) ma_gid = bn_cmap_gid[i];
        }
        assert(ka_gid && i_gid && e_gid && aa_gid && ma_gid);

        unsigned ki[] = {KA, ISIGN};
        assert(bn_shape_syllable(ki, 2, 0, 14, g, 16) == 2);
        assert(g[0].gid == i_gid && g[1].gid == ka_gid); /* typed ক ি, drawn ি before ক */

        unsigned ko[] = {KA, 0x9CB};
        assert(bn_shape_syllable(ko, 2, 0, 14, g, 16) == 3);
        assert(g[0].gid == e_gid && g[1].gid == ka_gid && g[2].gid == aa_gid); /* ো = ে + ক + া around the consonant */

        unsigned kssa[] = {KA, HAS, SSA};
        assert(bn_shape_syllable(kssa, 3, 0, 14, g, 16) == 1 && g[0].gid != ka_gid); /* ক্ষ is one ligature glyph */

        unsigned rma[] = {RA, HAS, MA};
        int n = bn_shape_syllable(rma, 3, 0, 14, g, 16);
        assert(n == 2 && g[0].gid == ma_gid && g[1].adv == 0); /* র্ম: ম first, the reph hook after it, without width */

        unsigned plain[] = {KA};
        assert(bn_shape_syllable(plain, 1, 0, 14, g, 16) == 1 && g[0].gid == ka_gid && g[0].adv > 0);
        assert(bn_shape_syllable(plain, 1, 0, 14, g, 0) == 0); /* max_out respected */

        /* the same syllable is wider at a bigger size */
        BnGlyphPos g2[16];
        bn_shape_syllable(plain, 1, 0, 32, g2, 16);
        assert(g2[0].adv > g[0].adv);
    }

    /* ---- Bengali: drawing, measuring, fitting ---- */
    {
        const char *ka = "\xe0\xa6\x95", *ki = "\xe0\xa6\x95\xe0\xa6\xbf", *kiki = "\xe0\xa6\x95\xe0\xa6\xbf\xe0\xa6\x95\xe0\xa6\xbf";
        assert(gfx_text_w(&font_ui_14, ka) > 0);
        assert(gfx_text_w(&font_ui_14, kiki) > gfx_text_w(&font_ui_14, ki));
        assert(gfx_text_w(&font_ui_20, ka) > gfx_text_w(&font_ui_14, ka));
        int calls = blend_calls;
        test_clip = (GfxClip){-100000, -100000, 100000, 100000};
        gfx_text(&font_ui_14, 10, 5, ki, gfx_rgb(255, 255, 255));
        assert(blend_calls > calls);
        /* a Bengali syllable is never cut: fit returns a byte count on a syllable boundary */
        assert(gfx_text_fit(&font_ui_14, kiki, gfx_text_w(&font_ui_14, ki)) == 6);
        assert(gfx_text_fit(&font_ui_14, kiki, gfx_text_w(&font_ui_14, kiki)) == 12);
        assert(gfx_text_fit(&font_ui_14, kiki, 1) == 0);
        /* Latin and Bengali mix; lines still break */
        int mixed = gfx_text_w(&font_ui_14, "A\xe0\xa6\x95");
        int sum = gfx_text_w(&font_ui_14, "A") + gfx_text_w(&font_ui_14, ka);
        assert(mixed >= sum - 1 && mixed <= sum + 1);
        assert(gfx_text_w(&font_ui_14, "\xe0\xa6\x95\nA") == (gfx_text_w(&font_ui_14, ka) > gfx_text_w(&font_ui_14, "A") ? gfx_text_w(&font_ui_14, ka) : gfx_text_w(&font_ui_14, "A")));
        /* anything the fonts cannot show is a '?': bad UTF-8, Chinese, unassigned Bengali code point */
        int q = gfx_text_w(&font_ui_14, "?");
        assert(gfx_text_w(&font_ui_14, "\xff") == q && gfx_text_w(&font_ui_14, "\xe4\xb8\xad") == q);
        assert(gfx_text_w(&font_ui_14, "\xe0\xa6") == q); /* truncated character */
        assert(gfx_text_w(&font_ui_14, "\xe0\xa6\x84") == q); /* U+0984: unassigned */
    }

    /* ---- Bengali in the weight of the Latin font: regular, medium, bold ---- */
    {
        assert(bn_weight_of(&font_ui_14) == BN_REGULAR && bn_weight_of(&font_ui_32) == BN_REGULAR);
        assert(bn_weight_of(&font_med_14) == BN_MEDIUM && bn_weight_of(&font_med_16) == BN_MEDIUM);
        assert(bn_weight_of(&font_bold_20) == BN_BOLD);
        assert(bn_face_for(&font_med_14) == &font_bn_med_14 && bn_face_for(&font_med_16) == &font_bn_med_16);
        assert(bn_face_for(&font_bold_20) == &font_bn_bold_20);
        assert(bn_face_for(&font_ui_12) == &font_bn_12 && bn_face_for(&font_ui_20) == &font_bn_20 && bn_face_for(&font_ui_32) == &font_bn_32);

        /* the glyphs are the same in every weight (same count, same ids); their widths are not */
        unsigned kormo[] = {0x09B0, 0x09CD, 0x09AE}, ko[] = {0x0995, 0x09CB};
        BnGlyphPos r[16], m[16], b[16];
        int nr = bn_shape_syllable_w(kormo, 3, 0, 20, BN_REGULAR, r, 16), nm = bn_shape_syllable_w(kormo, 3, 0, 20, BN_MEDIUM, m, 16),
            nb = bn_shape_syllable_w(kormo, 3, 0, 20, BN_BOLD, b, 16);
        assert(nr == nm && nm == nb && nr == 2);
        for (int i = 0; i < nr; i++)
            assert(r[i].gid == m[i].gid && m[i].gid == b[i].gid);
        nr = bn_shape_syllable_w(ko, 2, 0, 20, BN_REGULAR, r, 16);
        nb = bn_shape_syllable_w(ko, 2, 0, 20, BN_BOLD, b, 16);
        assert(nr == 3 && nb == 3);
        int differ = 0;
        for (int i = 0; i < nr; i++)
            differ |= r[i].adv != b[i].adv;
        assert(differ); /* a bold letter is wider */
        /* the weight-less call is the regular one */
        BnGlyphPos plain[16];
        assert(bn_shape_syllable(ko, 2, 0, 20, plain, 16) == 3);
        for (int i = 0; i < 3; i++)
            assert(plain[i].gid == r[i].gid && plain[i].adv == r[i].adv && plain[i].x_off == r[i].x_off);

        /* text in the medium / bold Latin fonts is drawn with the medium / bold Bengali: wider, and with more ink */
        const char *bangla = "\xe0\xa6\xac\xe0\xa6\xbe\xe0\xa6\x82\xe0\xa6\xb2\xe0\xa6\xbe"; /* বাংলা */
        assert(gfx_text_w(&font_med_14, bangla) != gfx_text_w(&font_ui_14, bangla));
        assert(gfx_text_w(&font_bold_20, bangla) != gfx_text_w(&font_ui_20, bangla));
        test_clip = (GfxClip){-100000, -100000, 100000, 100000};
        int c0 = blend_calls;
        gfx_text(&font_ui_20, 10, 5, bangla, gfx_rgb(255, 255, 255));
        int regular_px = blend_calls - c0;
        c0 = blend_calls;
        gfx_text(&font_bold_20, 10, 5, bangla, gfx_rgb(255, 255, 255));
        assert(regular_px > 0 && blend_calls - c0 > regular_px); /* bold covers more pixels */
        /* mixed with Latin in a bold line: still one line, widths add up */
        char mixed[32];
        strcpy(mixed, "ab");
        strcat(mixed, bangla);
        int sum = gfx_text_w(&font_bold_20, "ab") + gfx_text_w(&font_bold_20, bangla);
        assert(gfx_text_w(&font_bold_20, mixed) >= sum - 2 && gfx_text_w(&font_bold_20, mixed) <= sum + 2);
    }

    /* ---- Arabic, Cyrillic, Greek and Latin-1 in the weight of the Latin font ---- */
    {
        assert(ar_face_for(&font_med_14) == &font_ar_med_14 && ar_face_for(&font_med_16) == &font_ar_med_16);
        assert(ar_face_for(&font_bold_20) == &font_ar_bold_20);
        assert(ar_face_for(&font_ui_14) == &font_ar_14 && ar_face_for(&font_ui_32) == &font_ar_32 && ar_face_for(&font_ui_12) == &font_ar_12);

        /* Arabic: the same glyphs in every weight, wider and with the marks anchored on that weight's own letters */
        unsigned word[] = {0x0628, 0x0650, 0x0633, 0x0652, 0x0645};
        BnGlyphPos r[96], m[96], b[96];
        int nr = ar_shape_run_w(word, 5, 20, AR_REGULAR, r, 96), nm = ar_shape_run_w(word, 5, 20, AR_MEDIUM, m, 96),
            nb = ar_shape_run_w(word, 5, 20, AR_BOLD, b, 96);
        assert(nr == nm && nm == nb && nr > 5);
        int differ = 0;
        for (int i = 0; i < nr; i++)
        {
            assert(r[i].gid == m[i].gid && m[i].gid == b[i].gid);
            differ |= r[i].adv != b[i].adv;
        }
        assert(differ);
        BnGlyphPos plain[96];
        assert(ar_shape_run(word, 5, 20, plain, 96) == nr);
        for (int i = 0; i < nr; i++)
            assert(plain[i].gid == r[i].gid && plain[i].adv == r[i].adv && plain[i].x_off == r[i].x_off && plain[i].y_off == r[i].y_off);

        /* every script: text in the medium / bold Latin font is drawn with the medium / bold face, wider and with more ink */
        const char *samples[] = {"\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7",                                  /* مرحبا */
                                 "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82",                          /* Привет */
                                 "\xce\x95\xce\xbb\xce\xbb\xce\xac\xce\xb4\xce\xb1",                          /* Ελλάδα */
                                 "caf\xc3\xa9 \xc3\x80\xc3\x89\xc3\x8e"};                                     /* café ÀÉÎ */
        test_clip = (GfxClip){-100000, -100000, 100000, 100000};
        for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++)
        {
            /* (medium is less than a pixel wider than regular for a short word, so its width may round to the same: ink tells) */
            assert(gfx_text_w(&font_med_14, samples[i]) >= gfx_text_w(&font_ui_14, samples[i]));
            assert(gfx_text_w(&font_bold_20, samples[i]) > gfx_text_w(&font_ui_20, samples[i]));
            int c0 = blend_calls;
            gfx_text(&font_ui_14, 10, 5, samples[i], gfx_rgb(255, 255, 255));
            int regular14_px = blend_calls - c0;
            c0 = blend_calls;
            gfx_text(&font_med_14, 10, 5, samples[i], gfx_rgb(255, 255, 255));
            assert(regular14_px > 0 && blend_calls - c0 > regular14_px); /* medium covers more pixels */
            c0 = blend_calls;
            gfx_text(&font_ui_20, 10, 5, samples[i], gfx_rgb(255, 255, 255));
            int regular_px = blend_calls - c0;
            c0 = blend_calls;
            gfx_text(&font_bold_20, 10, 5, samples[i], gfx_rgb(255, 255, 255));
            assert(regular_px > 0 && blend_calls - c0 > regular_px); /* bold covers more */
        }
        /* the three weights of the extra faces hold the same characters, so a line measures the same in every weight except for widths */
        assert(font_ext_med_14.glyphs != font_ext_14.glyphs && font_ext_bold_20.bitmap != font_ext_20.bitmap);
        /* a right-to-left line in a medium / bold font: widths still add up, and fit still works */
        char line[64];
        strcpy(line, samples[0]);
        strcat(line, " ");
        strcat(line, samples[0]);
        int w1 = gfx_text_w(&font_bold_20, samples[0]), sp = gfx_text_w(&font_bold_20, " ");
        assert(gfx_text_w(&font_bold_20, line) >= 2 * w1 + sp - 2 && gfx_text_w(&font_bold_20, line) <= 2 * w1 + sp + 2);
        assert(gfx_text_fit(&font_bold_20, line, gfx_text_w(&font_bold_20, line)) == (int)strlen(line));
    }

    /* ---- Latin-1, Latin Extended-A, Greek, Cyrillic: drawn from the extra faces, not '?' ---- */
    {
        int q = gfx_text_w(&font_ui_14, "?");
        const char *e_acute = "\xc3\xa9", *eur = "\xe2\x82\xac", *alpha = "\xce\xb1", *ya = "\xd0\xaf"; /* é € α Я */
        const char *priv = "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";                            /* Привет */
        assert(gfx_text_w(&font_ui_14, e_acute) > 0);
        assert(gfx_text_w(&font_ui_14, priv) > 0);
        assert(gfx_text_w(&font_ui_14, priv) != gfx_text_w(&font_ui_14, "??????")); /* six characters of their own widths, not six '?' */
        /* the glyphs have pixels, and a bigger size is wider */
        int calls = blend_calls;
        test_clip = (GfxClip){-100000, -100000, 100000, 100000};
        gfx_text(&font_ui_14, 10, 5, priv, gfx_rgb(255, 255, 255));
        assert(blend_calls > calls);
        assert(gfx_text_w(&font_ui_20, ya) > gfx_text_w(&font_ui_14, ya));
        assert(gfx_text_w(&font_ui_32, alpha) > gfx_text_w(&font_ui_12, alpha));
        /* fit: whole characters (2 bytes each here) */
        assert(gfx_text_fit(&font_ui_14, priv, gfx_text_w(&font_ui_14, "\xd0\x9f\xd1\x80\xd0\xb8")) == 6);
        assert(gfx_text_fit(&font_ui_14, priv, gfx_text_w(&font_ui_14, priv)) == 12);
        assert(gfx_text_fit(&font_ui_14, priv, 1) == 0);
        /* mixing with ASCII and Bengali: widths add up (within rounding) */
        int mixed = gfx_text_w(&font_ui_14, "a\xc3\xa9\xd0\xaf\xe0\xa6\x95");
        int sum = gfx_text_w(&font_ui_14, "a") + gfx_text_w(&font_ui_14, e_acute) + gfx_text_w(&font_ui_14, ya) +
                  gfx_text_w(&font_ui_14, "\xe0\xa6\x95");
        assert(mixed >= sum - 3 && mixed <= sum + 3);
        /* every size has the characters, and the euro sign and non-breaking space too */
        assert(gfx_text_w(&font_ui_12, eur) > 0 && gfx_text_w(&font_ui_16, eur) > 0 && gfx_text_w(&font_bold_20, eur) > 0);
        assert(gfx_text_w(&font_med_14, "\xc2\xa0") > 0); /* U+00A0 has width but nothing to draw */
        /* what no font has is still '?': Chinese, an emoji, a Greek letter that Noto leaves out of the table, U+FFFD */
        assert(gfx_text_w(&font_ui_14, "\xe4\xb8\xad") == q && gfx_text_w(&font_ui_14, "\xf0\x9f\x98\x80") == q);
        assert(gfx_text_w(&font_ui_14, "\xef\xbf\xbd") == q);
        /* ext_cp is sorted (the lookup is a binary search) and holds no ASCII */
        for (unsigned i = 1; i < ext_n; i++)
            assert(ext_cp[i - 1] < ext_cp[i]);
        assert(ext_cp[0] >= 0x80);
    }

    /* ---- Arabic: joining shapes, marks, right-to-left drawing ---- */
    {
        assert(ar_is_arabic(0x0628) && ar_is_arabic(0x064E) && !ar_is_arabic('A') && !ar_is_arabic(' ') && !ar_is_arabic(0x200D));
        unsigned ba[] = {0x0628}, ba3[] = {0x0628, 0x0628, 0x0628}, ba_alef[] = {0x0628, 0x0627}, alef[] = {0x0627};
        unsigned ba_fatha[] = {0x0628, 0x064E}, sentence[] = {0x0645, 0x0631, ' ', 0x0627};
        BnGlyphPos g[96], h[96], k[96];
        assert(ar_run_len(sentence, 4) == 2); /* a run ends at a space */
        int n1 = ar_shape_run(ba, 1, 14, g, 96), n3 = ar_shape_run(ba3, 3, 14, h, 96);
        assert(n1 == 2 && n3 == 6);           /* ba is a skeleton and a dot mark; three of them */
        /* the four shapes of ba: alone, at the start, in the middle, at the end of a join are four different glyphs */
        assert(h[0].gid != h[2].gid && h[2].gid != h[4].gid && h[0].gid != h[4].gid);
        assert(g[0].gid != h[0].gid && g[0].gid != h[2].gid && g[0].gid != h[4].gid);
        assert(g[1].adv == 0 && h[1].adv == 0 && h[3].adv == 0 && h[5].adv == 0); /* a dot has no width of its own */
        assert(g[0].adv > 0 && h[0].adv > 0 && h[2].adv > 0 && h[4].adv > 0);
        /* alef joins only to the letter before it: after ba it is the final form, a different glyph from the plain alef */
        int na = ar_shape_run(alef, 1, 14, k, 96);
        int nb = ar_shape_run(ba_alef, 2, 14, h, 96);
        assert(na == 1 && nb == 3 && h[2].gid != k[0].gid);
        /* a mark (fatha) takes no width, and is moved to its place over the letter */
        int nf = ar_shape_run(ba_fatha, 2, 14, h, 96);
        assert(nf == 3 && h[2].adv == 0 && (h[2].x_off != 0 || h[2].y_off != 0));
        /* bigger size, bigger glyphs; a size's face is the one for that size */
        ar_shape_run(ba, 1, 32, k, 96);
        assert(k[0].adv > g[0].adv && ar_face_for_size(14) == &font_ar_14 && ar_face_for_size(32) == &font_ar_32);
        assert(ar_shape_run(ba3, 3, 14, h, 2) == 2); /* max_out respected */

        /* drawing and measuring: Arabic is not '?', has pixels, and a line mixes with Latin */
        const char *marhaba = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7";                    /* مرحبا */
        const char *bil = "\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85";        /* بالعالم */
        int wm = gfx_text_w(&font_ui_14, marhaba), wb = gfx_text_w(&font_ui_14, bil);
        assert(wm > 0 && wm != gfx_text_w(&font_ui_14, "?????") && wb > 0);
        int calls = blend_calls;
        test_clip = (GfxClip){-100000, -100000, 100000, 100000};
        gfx_text(&font_ui_14, 10, 5, marhaba, gfx_rgb(255, 255, 255));
        assert(blend_calls > calls);
        char line[64];
        strcpy(line, marhaba);
        strcat(line, " ");
        strcat(line, bil);
        int sp = gfx_text_w(&font_ui_14, " ");
        assert(gfx_text_w(&font_ui_14, line) >= wm + sp + wb - 2 && gfx_text_w(&font_ui_14, line) <= wm + sp + wb + 2);
        char mixed[64];
        strcpy(mixed, "abc ");
        strcat(mixed, marhaba);
        int wabc = gfx_text_w(&font_ui_14, "abc ");
        assert(gfx_text_w(&font_ui_14, mixed) >= wabc + wm - 2 && gfx_text_w(&font_ui_14, mixed) <= wabc + wm + 2);
        assert(gfx_text_w(&font_ui_20, marhaba) > gfx_text_w(&font_ui_12, marhaba));
        /* a line break inside right-to-left text: the width is the longest line */
        char two[64];
        strcpy(two, marhaba);
        strcat(two, "\n");
        strcat(two, bil);
        assert(gfx_text_w(&font_ui_14, two) == (wm > wb ? wm : wb));
        /* fit: the bytes of the line's start (in the order typed) that fit */
        assert(gfx_text_fit(&font_ui_14, line, gfx_text_w(&font_ui_14, line)) == (int)strlen(line));
        assert(gfx_text_fit(&font_ui_14, line, 1) == 0);
        assert(gfx_text_fit(&font_ui_14, line, wm) == (int)strlen(marhaba)); /* the first word fits, the space after it does not */
        /* a long right-to-left line is laid out in chunks and still measures the sum of its parts */
        char longl[1024];
        longl[0] = 0;
        for (int i = 0; i < 40; i++)
        {
            strcat(longl, marhaba);
            strcat(longl, " ");
        }
        assert(gfx_text_w(&font_ui_14, longl) >= 40 * (wm + sp) - 40 && gfx_text_w(&font_ui_14, longl) <= 40 * (wm + sp) + 40);
        /* Arabic digits and punctuation come from the Arabic face too, and nothing is '?' */
        {   /* (their width happens to equal that of '?': what tells them apart is what is drawn) */
            int c0 = blend_calls;
            gfx_text(&font_ui_14, 10, 5, "\xd9\xa1\xd9\xa2\xd9\xa3", gfx_rgb(255, 255, 255));
            int digits_px = blend_calls - c0;
            c0 = blend_calls;
            gfx_text(&font_ui_14, 10, 5, "???", gfx_rgb(255, 255, 255));
            assert(digits_px > 0 && digits_px != blend_calls - c0);
        }
        /* a joiner outside a word and Bengali next to Arabic still work */
        assert(gfx_text_w(&font_ui_14, "\xe0\xa6\x95\xd9\x85\xe0\xa6\x95") > 0);
    }

    puts("Font regression tests passed.");
    return 0;
}
