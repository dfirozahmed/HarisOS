/* HarisOS layers: draw into a separate buffer, then paste it with transparency. See include/gfx.h. */
#include "gfx_internal.h"

static struct
{
    volatile unsigned int *draw;
    int dpitch, w, h, back;
    GfxClip clip;
    int on;
} SAVED;

int gfx_layer_begin(void *buf, int w, int h, unsigned fill)
{
    if (SAVED.on || !buf || w <= 0 || h <= 0)
        return -1;
    SAVED.draw = DRAW;
    SAVED.dpitch = DPITCH;
    SAVED.w = W;
    SAVED.h = H;
    SAVED.back = BACK;
    SAVED.clip = gfx_get_clip();
    SAVED.on = 1;

    volatile unsigned int *p = (volatile unsigned int *)buf;
    for (long long i = 0; i < (long long)w * h; i++)
        p[i] = fill;
    DRAW = p;
    DPITCH = w;
    W = w;
    H = h;
    BACK = 0; /* nothing drawn here may be marked dirty or presented */
    gfx_reset_clip();
    return 0;
}

void gfx_layer_end(void)
{
    if (!SAVED.on)
        return;
    DRAW = SAVED.draw;
    DPITCH = SAVED.dpitch;
    W = SAVED.w;
    H = SAVED.h;
    BACK = SAVED.back;
    SAVED.on = 0;
    gfx_restore_clip(SAVED.clip);
}


/* For every row of the packed layer, its longest run of fully opaque pixels. That run is copied in one go;
 * only the edges (shadow, rounded corners) need per-pixel blending. */
#define LAYER_FAINT 248 /* at least this much background left = under 3% covered: not worth blending */
#define SPAN_ROWS 2048
static unsigned short SPAN_X[SPAN_ROWS], SPAN_N[SPAN_ROWS];
static const void *SPAN_LAYER;
static int SPAN_W, SPAN_H;

/* Drawn over black a pixel is a*c; over white it is a*c + (1-a)*255. Their difference is (1-a)*255:
 * how much background shows through. The three channels give the same answer for normal blending, so
 * their average is used. */
void gfx_layer_pack(void *over_black, const void *over_white, int w, int h)
{
    unsigned int *b = (unsigned int *)over_black;
    const unsigned int *q = (const unsigned int *)over_white;
    int spans = h <= SPAN_ROWS && w <= 65535;
    for (int y = 0; y < h; y++)
    {
        int best_x = 0, best_n = 0, run_x = 0, run_n = 0;
        for (int x = 0; x < w; x++)
        {
            long long i = (long long)y * w + x;
            unsigned int p = b[i], v = q[i];
            int t = 0;
            for (int s = 0; s < 24; s += 8)
            {
                int d = (int)((v >> s) & 0xFF) - (int)((p >> s) & 0xFF);
                t += d > 0 ? d : 0;
            }
            t /= 3;
            if (t >= LAYER_FAINT)
            {
                t = 255; /* background shows through almost fully (outer edge of a shadow): skip it when pasting */
                p = 0;
            }
            b[i] = (p & 0x00FFFFFFu) | ((unsigned)t << 24);
            if (t == 0)
            {
                if (run_n == 0)
                    run_x = x;
                if (++run_n > best_n)
                {
                    best_n = run_n;
                    best_x = run_x;
                }
            }
            else
                run_n = 0;
        }
        if (spans)
        {
            SPAN_X[y] = (unsigned short)best_x;
            SPAN_N[y] = (unsigned short)best_n;
        }
    }
    SPAN_LAYER = spans ? over_black : 0;
    SPAN_W = w;
    SPAN_H = h;
}

/* v / 255 for v = channel * t, t <= 254: off by at most one level, which is invisible and much cheaper */
static unsigned div255(unsigned v)
{
    return (v + (v >> 8) + 1) >> 8;
}

/* blend n packed pixels over the picture */
static void blend_run(volatile unsigned int *dp, const unsigned int *sp, int n)
{
    for (; n > 0; n--, sp++, dp++)
    {
        unsigned int p = *sp;
        unsigned int t = p >> 24; /* 255 = untouched background, 0 = opaque */
        if (t == 255)
            continue;
        if (t == 0)
        {
            *dp = p & 0x00FFFFFFu;
            continue;
        }
        unsigned int d = *dp;
        unsigned int r0 = (p & 0xFF) + div255((d & 0xFF) * t);
        unsigned int r1 = ((p >> 8) & 0xFF) + div255(((d >> 8) & 0xFF) * t);
        unsigned int r2 = ((p >> 16) & 0xFF) + div255(((d >> 16) & 0xFF) * t);
        *dp = (r0 > 255 ? 255 : r0) | ((r1 > 255 ? 255 : r1) << 8) | ((r2 > 255 ? 255 : r2) << 16);
    }
}

void gfx_layer_blit(const void *layer, int lw, int lh, int x, int y)
{
    int x0 = x, y0 = y, x1 = x + lw, y1 = y + lh;
    if (!layer || !gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        return;
    const unsigned int *src = (const unsigned int *)layer;
    int use_spans = layer == SPAN_LAYER && lw == SPAN_W && lh == SPAN_H;
    for (int yy = y0; yy < y1; yy++)
    {
        const unsigned int *row = src + (long long)(yy - y) * lw;
        volatile unsigned int *dst = DRAW + (long long)yy * DPITCH;
        int c0 = x0 - x, c1 = x1 - x; /* columns of the layer to paste */
        int s0 = c0, s1 = c0;         /* opaque run inside them (empty by default) */
        if (use_spans)
        {
            s0 = SPAN_X[yy - y];
            s1 = s0 + SPAN_N[yy - y];
            if (s0 < c0) s0 = c0;
            if (s1 > c1) s1 = c1;
            if (s1 <= s0) s0 = s1 = c0;
        }
        blend_run(dst + x + c0, row + c0, s0 - c0);
        gfx_internal_copy32(dst + x + s0, row + s0, s1 - s0); /* opaque: top byte is 0, so the word is the pixel */
        blend_run(dst + x + s1, row + s1, c1 - s1);
    }
    gfx_internal_mark_dirty(x0, y0, x1, y1);
}
