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

/* ---- overlay: a driver pastes the layer (see include/gfx.h) ---- */
static const GfxOverlayOps *OV_OPS;
static struct
{
    const void *layer;
    int lw, lh, x, y, inset;
    int on;
} OV;
static int AREA_SET, AX0, AY0, AX1, AY1; /* not set: the whole screen */

void gfx_overlay_driver(const GfxOverlayOps *ops)
{
    OV_OPS = ops;
    OV.on = 0;
}

void gfx_overlay_area(int x, int y, int w, int h)
{
    AREA_SET = 1;
    AX0 = x;
    AY0 = y;
    AX1 = w > 0 ? x + w : x;
    AY1 = h > 0 ? y + h : y;
}

void gfx_overlay_hide(void)
{
    OV.on = 0;
}

/* [x0,x1) x [y0,y1) cut down to the screen and the overlay area; 1 when something is left */
static int area_box(int *x0, int *y0, int *x1, int *y1)
{
    int ax0 = AREA_SET ? AX0 : 0, ay0 = AREA_SET ? AY0 : 0, ax1 = AREA_SET ? AX1 : W, ay1 = AREA_SET ? AY1 : H;
    if (ax0 < 0) ax0 = 0;
    if (ay0 < 0) ay0 = 0;
    if (ax1 > W) ax1 = W;
    if (ay1 > H) ay1 = H;
    if (*x0 < ax0) *x0 = ax0;
    if (*y0 < ay0) *y0 = ay0;
    if (*x1 > ax1) *x1 = ax1;
    if (*y1 > ay1) *y1 = ay1;
    return *x1 > *x0 && *y1 > *y0;
}

int gfx_overlay_show(const void *layer, int lw, int lh, int x, int y, int changed, int inset)
{
    int ax0 = 0, ay0 = 0, ax1 = W, ay1 = H;
    if (!OV_OPS || !BACK || !layer || lw <= 0 || lh <= 0 || gfx_brightness() < GFX_BRIGHTNESS_MAX ||
        !area_box(&ax0, &ay0, &ax1, &ay1))
    {
        OV.on = 0;
        return 0;
    }
    if (changed || !OV.on || OV.layer != layer || OV.lw != lw || OV.lh != lh)
        if (OV_OPS->upload(layer, lw, lh) != 0)
        {
            OV.on = 0;
            return 0;
        }
    OV.layer = layer;
    OV.lw = lw;
    OV.lh = lh;
    OV.x = x;
    OV.y = y;
    OV.inset = inset < 0 ? 0 : inset;
    OV.on = 1;
    int x0 = x, y0 = y, x1 = x + lw, y1 = y + lh;
    if (gfx_internal_clip_box(&x0, &y0, &x1, &y1))
        gfx_internal_mark_dirty(x0, y0, x1, y1); /* so gfx_present() pastes it here */
    return 1;
}

/* The paste of the present being made: the part of the layer inside the dirty box, and where the picture below it goes. */
static int PB_ON, PBX0, PBY0, PBX1, PBY1, STAGE_PITCH;
static volatile unsigned int *STAGE;

/* The CPU pastes the layer into the back buffer inside the box (the overlay is off from now on). */
static void cpu_paste(int x0, int y0, int x1, int y1)
{
    OV.on = 0;
    GfxClip saved = gfx_get_clip();
    gfx_set_clip(x0, y0, x1 - x0, y1 - y0);
    gfx_layer_blit(OV.layer, OV.lw, OV.lh, OV.x, OV.y);
    gfx_restore_clip(saved);
}

int gfx_internal_overlay_prepare(int x0, int y0, int x1, int y1)
{
    PB_ON = 0;
    if (!OV.on)
        return 0;
    if (x0 < OV.x) x0 = OV.x;
    if (y0 < OV.y) y0 = OV.y;
    if (x1 > OV.x + OV.lw) x1 = OV.x + OV.lw;
    if (y1 > OV.y + OV.lh) y1 = OV.y + OV.lh;
    if (!area_box(&x0, &y0, &x1, &y1))
        return 0;
    if (gfx_brightness() < GFX_BRIGHTNESS_MAX) /* the screen copy is scaled, a GPU paste would not be */
    {
        cpu_paste(x0, y0, x1, y1);
        return 0;
    }
    STAGE = OV_OPS ? OV_OPS->below(&STAGE_PITCH) : 0;
    if (!STAGE)
    {
        OV_OPS = 0; /* the driver failed: never again */
        cpu_paste(x0, y0, x1, y1);
        return 0;
    }
    PB_ON = 1;
    PBX0 = x0;
    PBY0 = y0;
    PBX1 = x1;
    PBY1 = y1;
    return 1;
}

int gfx_internal_overlay_cols(int y, int *x0, int *x1)
{
    if (!PB_ON || y < PBY0 || y >= PBY1)
        return 0;
    *x0 = PBX0;
    *x1 = PBX1;
    return 1;
}

void gfx_internal_overlay_stage(const volatile unsigned int *row, int y, int x0, int x1)
{
    volatile unsigned int *dst = STAGE + (long long)(y - OV.y) * STAGE_PITCH; /* layer column c is screen x = OV.x + c */
    if (y >= OV.y + OV.inset && y < OV.y + OV.lh - OV.inset)
    {
        /* the opaque inside hides the picture below: leave it out */
        int s0 = OV.x + OV.inset, s1 = OV.x + OV.lw - OV.inset;
        if (s0 < x0) s0 = x0;
        if (s1 > x1) s1 = x1;
        if (s1 > s0)
        {
            gfx_internal_copy_fb(dst + (x0 - OV.x), row + x0, s0 - x0);
            gfx_internal_copy_fb(dst + (s1 - OV.x), row + s1, x1 - s1);
            return;
        }
    }
    gfx_internal_copy_fb(dst + (x0 - OV.x), row + x0, x1 - x0);
}

int gfx_internal_overlay_paste(int *x0, int *y0, int *x1, int *y1)
{
    if (!PB_ON)
        return 0;
    PB_ON = 0;
    *x0 = PBX0;
    *y0 = PBY0;
    *x1 = PBX1;
    *y1 = PBY1;
    if (OV_OPS && OV_OPS->draw(OV.x, OV.y, PBX0, PBY0, PBX1, PBY1) == 0)
        return 0;
    OV_OPS = 0; /* the driver failed: never again. The caller copies the box to the screen. */
    cpu_paste(PBX0, PBY0, PBX1, PBY1);
    return 1;
}
