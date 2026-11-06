/* HarisOS Arabic shaping: see include/shape_ar.h.
 *
 * The OpenType part is the engine in ot.c; this file is the Arabic part: joining types, the shape each letter takes, and the
 * order in which the features run (the Arabic shaper of HarfBuzz). */
#include "shape_ar.h"
#include "font_ar.h"
#include "ot.h"
#include "uni_data.h"

/* which shape a letter takes (a lookup of the stage for that shape runs only on glyphs with its bit) */
#define M_FINA 0x0001u
#define M_MEDI 0x0002u
#define M_INIT 0x0004u

static OtFont ar_otfont(int weight)
{
    if (weight < 0 || weight >= AR_WEIGHTS)
        weight = AR_REGULAR;
    OtFont f = {ar_gsub, ar_gpos_w[weight], ar_gdef, ar_gsub_lookups, ar_gsub_stage, 0, ar_gpos_lookups_w[weight],
                ar_gpos_nlookups_w[weight], ar_adv_w[weight], AR_UPEM, ar_cmap_cp, ar_cmap_gid, ar_cmap_n};
    return f;
}

static int is_joiner(unsigned cp) { return cp == 0x200C || cp == 0x200D; }

int ar_is_arabic(unsigned cp)
{
    if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F))
    {
        OtFont f = ar_otfont(AR_REGULAR); /* the cmap is the same in every weight */
        return ot_cmap(&f, cp) != 0;
    }
    return 0;
}

int ar_run_len(const unsigned *cp, int n)
{
    int i = 1;
    while (i < n && i < AR_RUN_MAX && (ar_is_arabic(cp[i]) || is_joiner(cp[i])))
        i++;
    return i;
}

const Font *ar_face_for(const Font *latin)
{
    /* the weight of the Latin font (the same rule as for Bengali), if the Arabic faces have its size */
    switch (bn_weight_of(latin))
    {
    case AR_MEDIUM:
        if (latin->size <= 15)
            return &font_ar_med_14;
        if (latin->size <= 18)
            return &font_ar_med_16;
        break;
    case AR_BOLD:
        if (latin->size >= 19 && latin->size <= 26)
            return &font_ar_bold_20;
        break;
    }
    return ar_face_for_size(latin->size);
}

const Font *ar_face_for_size(int size)
{
    if (size <= 13)
        return &font_ar_12;
    if (size <= 15)
        return &font_ar_14;
    if (size <= 18)
        return &font_ar_16;
    if (size <= 26)
        return &font_ar_20;
    return &font_ar_32;
}

/* the joining type of a character (JOIN_U if it is not stored) */
static int join_type(unsigned cp)
{
    int lo = 0, hi = (int)uni_join_n - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (cp < uni_join_start[mid])
            hi = mid - 1;
        else if (cp > uni_join_end[mid])
            lo = mid + 1;
        else
            return uni_join_type[mid];
    }
    return JOIN_U;
}

static int joins_forward(int t) { return t == JOIN_D || t == JOIN_L || t == JOIN_C; }  /* a letter that joins the one after it */
static int joins_backward(int t) { return t == JOIN_D || t == JOIN_R || t == JOIN_C; } /* a letter that joins the one before it */

/* design units -> 1/16 pixel at the size the Arabic glyphs are drawn at (a fraction of the Latin font's) */
static int scale16(int units, int size)
{
    long v = (long)units * size * 16 * AR_SCALE_PERMILLE;
    long d = (long)AR_UPEM * 1000;
    return (int)(v >= 0 ? (v + d / 2) / d : -((-v + d / 2) / d));
}

int ar_shape_run(const unsigned *cp, int n, int size, BnGlyphPos *out, int max_out)
{
    return ar_shape_run_w(cp, n, size, AR_REGULAR, out, max_out);
}

int ar_shape_run_w(const unsigned *cp, int n, int size, int weight, BnGlyphPos *out, int max_out)
{
    OtFont font = ar_otfont(weight);
    OtBuf b;
    int jt[AR_RUN_MAX];
    if (n > AR_RUN_MAX)
        n = AR_RUN_MAX;
    for (int i = 0; i < n; i++)
        jt[i] = join_type(cp[i]);

    ot_begin(&b, &font);
    for (int i = 0; i < n; i++)
    {
        unsigned mask = 0;
        int t = jt[i];
        if (t != JOIN_T) /* a mark takes no shape of its own; it does not interrupt the join of the letters around it either */
        {
            int prev = 0, next = 0;
            for (int p = i - 1; p >= 0; p--)
                if (jt[p] != JOIN_T)
                {
                    prev = joins_forward(jt[p]);
                    break;
                }
            for (int q = i + 1; q < n; q++)
                if (jt[q] != JOIN_T)
                {
                    next = joins_backward(jt[q]);
                    break;
                }
            int to_prev = prev && joins_backward(t), to_next = next && joins_forward(t);
            mask = to_prev && to_next ? M_MEDI : to_prev ? M_FINA : to_next ? M_INIT : 0; /* none: the isolated form, the plain letter */
        }
        if (!ot_add(&b, ot_cmap(&font, cp[i]), mask, 0))
            break;
    }

    ot_gsub_stage(&b, 0, OT_MASK_ALL, 0); /* ccmp */
    ot_gsub_stage(&b, 1, M_FINA, 0);      /* fina: letters at the end of a join */
    ot_gsub_stage(&b, 2, M_MEDI, 0);      /* medi: in the middle */
    ot_gsub_stage(&b, 3, M_INIT, 0);      /* init: at the start */
    ot_gsub_stage(&b, 4, OT_MASK_ALL, 0); /* rlig, calt, liga: ligatures such as lam + alef */
    ot_gpos(&b);
    ot_finish(&b, 1); /* the glyphs are drawn right to left */

    int cnt = b.len < max_out ? b.len : max_out;
    for (int i = 0; i < cnt; i++)
    {
        const OtGlyph *g = &b.buf[i];
        out[i].gid = g->gid;
        out[i].x_off = scale16(g->xoff, size);
        out[i].y_off = scale16(g->yoff, size);
        out[i].adv = scale16((int)font.adv[g->gid] + g->xadv, size);
    }
    return cnt;
}
