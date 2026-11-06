/* HarisOS Bengali shaping: see include/shape_bn.h.
 *
 * The OpenType part (GSUB / GPOS / GDEF) is the engine in ot.c; this file is the Bengali part: character classes, syllables,
 * reordering, and the order in which the features run (the Indic shaper of HarfBuzz). */
#include "shape_bn.h"
#include "font_bn.h"
#include "ot.h"

/* Which lookups may touch a glyph (see stage_mask). OT_MASK_ALL is set on every glyph. */
#define M_RPHF 0x0001u
#define M_BLWF 0x0002u
#define M_HALF 0x0004u
#define M_PSTF 0x0008u
#define M_INIT 0x0010u

#define F_BASE 0x01 /* this glyph is (or contains) the base consonant of the syllable */

/* The font's tables, as the engine wants them. (Made on the stack: the generated data are pointer variables, not constants.) */
static OtFont bn_otfont(void)
{
    OtFont f = {bn_gsub, bn_gpos, bn_gdef, bn_gsub_lookups, bn_gsub_stage, bn_gsub_init, bn_gpos_lookups, bn_gpos_nlookups,
                bn_adv, BN_UPEM, bn_cmap_cp, bn_cmap_gid, bn_cmap_n};
    return f;
}

/* ---------------------------------------------------------------- Bengali: character classes */

enum { K_OTHER, K_C, K_V, K_H, K_N, K_M, K_SM, K_ZWJ, K_ZWNJ };

#define RA 0x09B0u
#define BA 0x09ACu
#define YA 0x09AFu

static int cat(unsigned cp)
{
    if ((cp >= 0x0995 && cp <= 0x09B9) || cp == 0x09CE || cp == 0x09DC || cp == 0x09DD || cp == 0x09DF ||
        cp == 0x09F0 || cp == 0x09F1)
        return K_C;
    if ((cp >= 0x0985 && cp <= 0x0994) || cp == 0x09E0 || cp == 0x09E1)
        return K_V;
    if (cp == 0x09CD)
        return K_H;
    if (cp == 0x09BC)
        return K_N;
    if ((cp >= 0x09BE && cp <= 0x09C4) || cp == 0x09C7 || cp == 0x09C8 || cp == 0x09CB || cp == 0x09CC ||
        cp == 0x09D7 || cp == 0x09E2 || cp == 0x09E3)
        return K_M;
    if (cp >= 0x0981 && cp <= 0x0983)
        return K_SM;
    if (cp == 0x200D)
        return K_ZWJ;
    if (cp == 0x200C)
        return K_ZWNJ;
    return K_OTHER;
}

static int is_joiner(unsigned cp) { return cp == 0x200C || cp == 0x200D; }

/* Vowel signs typed after the consonant but drawn before it: ি ে ৈ. */
static int is_pre_base_sign(unsigned cp) { return cp == 0x09BF || cp == 0x09C7 || cp == 0x09C8; }

int bn_is_bengali(unsigned cp)
{
    if ((cp >= 0x0980 && cp <= 0x09FF) || cp == 0x0964 || cp == 0x0965 || cp == 0x200C || cp == 0x200D)
    {
        OtFont f = bn_otfont();
        return ot_cmap(&f, cp) != 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- Bengali: syllables */

int bn_syllable_len(const unsigned *cp, int n)
{
    int i = 0;
    int k = cat(cp[0]);
    if (k == K_C)
    {
        for (;;)
        {
            i++;                                  /* the consonant */
            if (i < n && cat(cp[i]) == K_N)       /* nukta */
                i++;
            if (i < n && cat(cp[i]) == K_H)       /* hasanta */
            {
                int j = i + 1;
                if (j < n && is_joiner(cp[j]))
                    j++;
                if (j < n && cat(cp[j]) == K_C && j < BN_SYL_MAX)
                {
                    i = j; /* another consonant joins: conjunct */
                    continue;
                }
                i = j; /* hasanta left at the end (visible hasanta) */
                break;
            }
            break;
        }
        while (i < n && cat(cp[i]) == K_M)
            i++;
        while (i < n && cat(cp[i]) == K_SM)
            i++;
    }
    else if (k == K_V)
    {
        i = 1;
        if (i < n && cat(cp[i]) == K_N)
            i++;
        while (i < n && cat(cp[i]) == K_SM)
            i++;
    }
    else
        i = 1;
    return i > BN_SYL_MAX ? BN_SYL_MAX : i;
}

/* ---------------------------------------------------------------- Bengali: shaping */

const Font *bn_face_for_size(int size)
{
    if (size <= 13)
        return &font_bn_12;
    if (size <= 15)
        return &font_bn_14;
    if (size <= 18)
        return &font_bn_16;
    if (size <= 26)
        return &font_bn_20;
    return &font_bn_32;
}

static int scale16(int units, int size)
{
    long v = (long)units * size * 16;
    return (int)(v >= 0 ? (v + BN_UPEM / 2) / BN_UPEM : -((-v + BN_UPEM / 2) / BN_UPEM));
}

/* Lookup mask for each GSUB stage: most features apply to every glyph; the four that depend on where a glyph is in
 * the syllable only to the glyphs that were marked for them. */
static unsigned stage_mask(int stage)
{
    switch (stage)
    {
    case 2: return M_RPHF;
    case 3: return M_BLWF;
    case 4: return M_HALF;
    case 5: return M_PSTF;
    }
    return OT_MASK_ALL;
}

int bn_shape_syllable(const unsigned *cp, int n, int word_start, int size, BnGlyphPos *out, int max_out)
{
    OtFont font = bn_otfont();
    OtBuf c;
    unsigned u[BN_SYL_MAX * 2 + 2];
    int m = 0;

    /* Split vowel signs: ো = ে + া and ৌ = ে + ৗ (the font has glyphs for the two halves, and the first half
     * is a pre-base sign). */
    for (int k = 0; k < n && m + 2 <= (int)(sizeof u / sizeof u[0]); k++)
    {
        if (cp[k] == 0x09CB)
            u[m++] = 0x09C7, u[m++] = 0x09BE;
        else if (cp[k] == 0x09CC)
            u[m++] = 0x09C7, u[m++] = 0x09D7;
        else
            u[m++] = cp[k];
    }

    int reph = 0, base = -1;
    int order[BN_SYL_MAX * 2 + 2];
    if (cat(u[0]) == K_C)
    {
        /* Reph: a leading "র্" followed by a consonant is drawn as a hook above that consonant. */
        int first_c = 0;
        if (m >= 3 && u[0] == RA && cat(u[1]) == K_H && !is_joiner(u[2]))
            for (int k = 2; k < m; k++)
                if (cat(u[k]) == K_C)
                {
                    reph = 1;
                    first_c = 2;
                    break;
                }
        /* Base: the last consonant, unless it forms a below/post-base shape (র ব য after a hasanta): then the
         * consonant before the hasanta. */
        int cand = -1;
        for (int k = m - 1; k >= first_c; k--)
            if (cat(u[k]) == K_C)
            {
                cand = k;
                break;
            }
        while (cand > first_c && (u[cand] == RA || u[cand] == BA || u[cand] == YA))
        {
            int h = cand - 1;
            if (h > first_c && is_joiner(u[h]))
                h--;
            if (h <= first_c || cat(u[h]) != K_H)
                break;
            int prev = -1;
            for (int k = h - 1; k >= first_c; k--)
                if (cat(u[k]) == K_C)
                {
                    prev = k;
                    break;
                }
            if (prev < 0)
                break;
            cand = prev;
        }
        base = cand;

        /* Order: reph, then the vowel signs drawn before the consonant (ি ে ৈ), then everything else as typed. */
        int o = 0;
        if (reph)
            order[o++] = 0, order[o++] = 1;
        for (int k = first_c; k < m; k++)
            if (is_pre_base_sign(u[k]))
                order[o++] = k;
        for (int k = first_c; k < m; k++)
            if (!is_pre_base_sign(u[k]))
                order[o++] = k;
    }
    else
        for (int k = 0; k < m; k++)
            order[k] = k;

    ot_begin(&c, &font);
    for (int k = 0; k < m; k++)
    {
        int idx = order[k];
        unsigned mask = 0;
        unsigned char flags = idx == base ? F_BASE : 0;
        if (reph && idx < 2)
            mask |= M_RPHF;
        else if (base >= 0 && idx < base)
            mask |= M_HALF;
        else if (base >= 0 && idx > base)
            mask |= M_BLWF | M_PSTF;
        if (!ot_add(&c, ot_cmap(&font, u[idx]), mask, flags))
            break;
    }
    if (word_start && c.len)
        c.buf[0].mask |= M_INIT;
    unsigned ra_gid = ot_cmap(&font, u[0]);

    for (int s = 0; s < 8; s++)
        ot_gsub_stage(&c, s, stage_mask(s), M_INIT);

    /* Final reordering: the reph that "রphf" produced moves from the front to just after the base consonant. */
    if (reph && c.len > 1 && c.buf[0].gid != ra_gid)
    {
        int b = -1;
        for (int k = 1; k < c.len; k++)
            if (c.buf[k].flags & F_BASE)
            {
                b = k;
                break;
            }
        if (b > 0)
        {
            OtGlyph r = c.buf[0];
            for (int k = 0; k < b; k++)
                c.buf[k] = c.buf[k + 1];
            c.buf[b] = r;
        }
    }

    ot_gsub_stage(&c, 8, stage_mask(8), M_INIT);
    ot_gpos(&c);
    ot_finish(&c, 0);

    int cnt = c.len < max_out ? c.len : max_out;
    for (int i = 0; i < cnt; i++)
    {
        const OtGlyph *g = &c.buf[i];
        out[i].gid = g->gid;
        out[i].x_off = scale16(g->xoff, size);
        out[i].y_off = scale16(g->yoff, size);
        out[i].adv = scale16((int)bn_adv[g->gid] + g->xadv, size);
    }
    return cnt;
}
