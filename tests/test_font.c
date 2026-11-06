#include "font.h"
#include "font_bn.h"
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

    puts("Font regression tests passed.");
    return 0;
}
