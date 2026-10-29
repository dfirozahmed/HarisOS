/* Windows as compositor surfaces (see gfx.h "compositor"): each visible window lives in its own surface, a packed
 * layer of the window and its shadow. A surface is drawn again only when what the window shows changed: its look
 * (size, focus, title-bar buttons, theme, title: the "signature"), or its content (a handler said so, see
 * wm_comp_content_internal). Moving or raising a window changes no surface, only where the GPU puts it. */
#include "wm_internal.h"
#include "ui.h"

#define WMC_MAX 64

typedef struct
{
    int id;              /* the window's id, 0 = free entry */
    int surf, lw, lh, m; /* the surface, its size, the margin around the window (room for the shadow) */
    unsigned sig;        /* the look it was drawn with */
    int full;            /* everything must be drawn again */
    int dirty;           /* content changed inside [dx0,dx1) x [dy0,dy1), relative to the window's corner */
    int dx0, dy0, dx1, dy1;
    int seen;
} Wmc;

static Wmc WMC[WMC_MAX];
static int ON;

static Wmc *entry(const Window *w, int make)
{
    Wmc *free_e = 0;
    for (int i = 0; i < WMC_MAX; i++)
    {
        if (WMC[i].id == w->id)
            return &WMC[i];
        if (!WMC[i].id && !free_e)
            free_e = &WMC[i];
    }
    if (!make || !free_e)
        return 0;
    *free_e = (Wmc){0};
    free_e->id = w->id;
    free_e->full = 1;
    return free_e;
}

void wm_comp_set(int on)
{
    ON = on;
    for (int i = 0; i < WMC_MAX; i++)
        WMC[i].full = 1; /* what changed while it was off is not known */
}

int wm_comp_on(void) { return ON; }

void wm_comp_content_internal(const Window *w, int x0, int y0, int x1, int y1)
{
    if (!ON || !w)
        return;
    Wmc *c = entry(w, 0);
    if (!c || c->full)
        return; /* drawn from scratch anyway */
    x0 -= w->r.x;
    x1 -= w->r.x;
    y0 -= w->r.y;
    y1 -= w->r.y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w->r.w) x1 = w->r.w;
    if (y1 > w->r.h) y1 = w->r.h;
    if (x1 <= x0 || y1 <= y0)
        return;
    if (!c->dirty)
    {
        c->dirty = 1;
        c->dx0 = x0;
        c->dy0 = y0;
        c->dx1 = x1;
        c->dy1 = y1;
        return;
    }
    if (x0 < c->dx0) c->dx0 = x0;
    if (y0 < c->dy0) c->dy0 = y0;
    if (x1 > c->dx1) c->dx1 = x1;
    if (y1 > c->dy1) c->dy1 = y1;
}

void wm_comp_content_screen_internal(int x0, int y0, int x1, int y1)
{
    if (!ON)
        return;
    for (int i = 0; i < WM_ZN; i++)
    {
        Window *w = WM_Z[i];
        if (!w->minimized && x0 < w->r.x + w->r.w && x1 > w->r.x && y0 < w->r.y + w->r.h && y1 > w->r.y)
            wm_comp_content_internal(w, x0, y0, x1, y1);
    }
}

void wm_comp_all_internal(void)
{
    for (int i = 0; i < WMC_MAX; i++)
        WMC[i].full = 1;
}

/* Everything the window's look depends on besides its content. */
static unsigned signature(const Window *w, int i, int top)
{
    unsigned h = 2166136261u;
#define MIX(v) (h = (h ^ (unsigned)(v)) * 16777619u)
    MIX(w->r.w);
    MIX(w->r.h);
    MIX(w->maximized);
    MIX(w->dialog);
    MIX(w->fixed_size);
    MIX(i == top);
    MIX(wm_titlebtn_hover(w) + 8);
    MIX(wm_titlebtn_pressed(w) + 8);
    MIX((unsigned)(unsigned long long)ui_theme());
    for (const char *s = w->title; *s; s++)
        MIX(*s);
#undef MIX
    return h;
}

/* The window drawn at (m, m) of a buffer: once over black and once over white, packed (see gfx_layer_pack). */
static int draw_layer(Window *w, int i, int top, Wmc *c, void *a, void *b)
{
    UiRect real = w->r;
    void *buf[2] = {a, b};
    for (int pass = 0; pass < 2; pass++)
    {
        if (gfx_layer_begin(buf[pass], c->lw, c->lh, pass ? 0x00FFFFFFu : 0) != 0)
            return -1;
        w->r = ui_rect(c->m, c->m, real.w, real.h);
        wm_draw_window_internal(w, i, top);
        gfx_layer_end();
        w->r = real;
    }
    wm_layout_internal(w);
    gfx_layer_pack(a, b, c->lw, c->lh);
    return 0;
}

/* Is [x0,x1) x [y0,y1) (relative to the window) where the window is opaque for sure: inside it, away from the
 * rounded corners and the outline? There a single drawing pass gives the final pixels. */
static int opaque_part(const Window *w, int x0, int y0, int x1, int y1)
{
    if (w->maximized)
        return 1; /* flat: no corners, no outline, no shadow */
    int r = ui_theme()->radius_l + 2;
    int in_a = x0 >= r && x1 <= w->r.w - r && y0 >= 1 && y1 <= w->r.h - 1;
    int in_b = x0 >= 1 && x1 <= w->r.w - 1 && y0 >= r && y1 <= w->r.h - r;
    return in_a || in_b;
}

int wm_comp_update(const GfxCompOps *ops, void *a, void *b, unsigned long long bytes)
{
    if (!ops || !a || !b)
        return -1;
    int top = wm_top_index_internal();
    for (int k = 0; k < WMC_MAX; k++)
        WMC[k].seen = 0;
    for (int i = 0; i < WM_ZN; i++)
    {
        Window *w = WM_Z[i];
        Wmc *c = entry(w, 1);
        if (!c)
            return -1;
        c->seen = 1;
        if (w->minimized)
            continue;
        int m = w->maximized ? 0 : WM_SHADOW_MARGIN;
        int lw = w->r.w + 2 * m, lh = w->r.h + 2 * m;
        if ((unsigned long long)lw * (unsigned long long)lh * 4ULL > bytes)
            return -1; /* too big for the drawing buffers */
        unsigned sig = signature(w, i, top);
        if (c->surf && (c->lw != lw || c->lh != lh))
        {
            ops->surface_free(c->surf);
            c->surf = 0;
        }
        if (!c->surf)
        {
            c->surf = ops->surface_new(lw, lh);
            if (!c->surf)
                return -1;
            c->lw = lw;
            c->lh = lh;
            c->full = 1;
        }
        c->m = m;
        if (sig != c->sig)
            c->full = 1;
        if (c->dirty && !c->full && !opaque_part(w, c->dx0, c->dy0, c->dx1, c->dy1))
            c->full = 1;
        if (c->full)
        {
            if (draw_layer(w, i, top, c, a, b) != 0 || ops->upload(c->surf, a, lw, 0, 0, lw, lh, 0) != 0)
                return -1;
        }
        else if (c->dirty)
        {
            /* one pass over whatever the buffer holds, only inside the changed part; that part is opaque */
            int x0 = c->dx0 + m, y0 = c->dy0 + m, cw = c->dx1 - c->dx0, ch = c->dy1 - c->dy0;
            UiRect real = w->r;
            if (gfx_layer_begin_keep(a, lw, lh) != 0)
                return -1;
            gfx_set_clip(x0, y0, cw, ch);
            w->r = ui_rect(m, m, real.w, real.h);
            wm_draw_window_internal(w, i, top);
            gfx_layer_end();
            w->r = real;
            wm_layout_internal(w);
            if (ops->upload(c->surf, (const unsigned int *)a + (long long)y0 * lw + x0, lw, x0, y0, cw, ch, 1) != 0)
                return -1;
        }
        c->sig = sig;
        c->full = 0;
        c->dirty = 0;
    }
    for (int k = 0; k < WMC_MAX; k++)
        if (WMC[k].id && !WMC[k].seen)
        {
            if (WMC[k].surf)
                ops->surface_free(WMC[k].surf); /* the window was closed */
            WMC[k] = (Wmc){0};
        }
    return 0;
}

int wm_comp_items(GfxCompItem *out, int max)
{
    int n = 0;
    for (int i = 0; i < WM_ZN && n < max; i++)
    {
        Window *w = WM_Z[i];
        Wmc *c = entry(w, 0);
        if (w->minimized || !c || !c->surf)
            continue;
        out[n++] = (GfxCompItem){c->surf, w->r.x - c->m, w->r.y - c->m, c->lw, c->lh, 1};
    }
    return n;
}

void wm_comp_drop(const GfxCompOps *ops)
{
    for (int k = 0; k < WMC_MAX; k++)
    {
        if (WMC[k].id && WMC[k].surf && ops)
            ops->surface_free(WMC[k].surf);
        WMC[k] = (Wmc){0};
    }
    ON = 0;
}
