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
    char title[24];               /* for the report */
    unsigned st_n;                /* drawn since wm_comp_busiest(): times, pixels */
    unsigned long long st_px;
} Wmc;

static Wmc WMC[WMC_MAX];
static int ON;
static unsigned DRAWN_FULL, DRAWN_PART; /* surfaces drawn again since wm_comp_counts() */

void wm_comp_counts(unsigned *full, unsigned *part)
{
    *full = DRAWN_FULL;
    *part = DRAWN_PART;
    DRAWN_FULL = DRAWN_PART = 0;
}

static unsigned WHY[WM_WHY_COUNT];
static const char *const WHY_NAME[WM_WHY_COUNT] = {"press", "right press", "drag", "window buttons", "hover left", "hover",
                                                   "tick", "wheel", "key", "app area", "invalidate"};

const char *wm_why_name(int why) { return why >= 0 && why < WM_WHY_COUNT ? WHY_NAME[why] : "?"; }

void wm_comp_why_internal(int why)
{
    if (ON && why >= 0 && why < WM_WHY_COUNT)
        WHY[why]++;
}

void wm_comp_why(unsigned out[WM_WHY_COUNT])
{
    for (int i = 0; i < WM_WHY_COUNT; i++)
    {
        out[i] = WHY[i];
        WHY[i] = 0;
    }
}

/* A window's surface was drawn (px pixels): counted for wm_comp_busiest(). */
static void tally(Wmc *c, const Window *w, unsigned long long px)
{
    int k = 0;
    for (; k < (int)sizeof c->title - 1 && w->title[k]; k++)
        c->title[k] = w->title[k];
    c->title[k] = 0;
    c->st_n++;
    c->st_px += px;
}

int wm_comp_busiest(WmBusy *out, int max)
{
    int n = 0;
    for (; n < max; n++)
    {
        Wmc *best = 0;
        for (int i = 0; i < WMC_MAX; i++)
            if (WMC[i].id && WMC[i].st_n && (!best || WMC[i].st_px > best->st_px))
                best = &WMC[i];
        if (!best)
            break;
        for (int k = 0; k < (int)sizeof out[n].title; k++)
            out[n].title[k] = best->title[k];
        out[n].redraws = best->st_n;
        out[n].pixels = best->st_px;
        best->st_n = 0; /* taken: the next round finds the next one */
        best->st_px = 0;
    }
    for (int i = 0; i < WMC_MAX; i++)
        WMC[i].st_n = 0, WMC[i].st_px = 0; /* the rest start again too */
    return n;
}

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

/* Apps ask with wm_invalidate() for their own window to be drawn again: everything a window shows (its look, when it
 * changed, the signature catches). */
void wm_comp_all_internal(void)
{
    for (int i = 0; i < WMC_MAX; i++)
    {
        Wmc *c = &WMC[i];
        if (!c->id || c->full || !c->surf)
            continue;
        c->dirty = 1;
        c->dx0 = c->dy0 = 0;
        c->dx1 = c->lw - 2 * c->m;
        c->dy1 = c->lh - 2 * c->m;
    }
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
    /* drawing stops at the screen's left and top edges: while the window reaches past them, where it is matters */
    int m = w->maximized ? 0 : WM_SHADOW_MARGIN;
    if (w->r.x - m < 0)
        MIX(w->r.x - m);
    if (w->r.y - m < 0)
        MIX(w->r.y - m);
#undef MIX
    return h;
}

/* The whole window and its shadow drawn into a buffer (its corner at (m, m)): once over black and once over white, packed
 * (see gfx_layer_pack). Only when its look changed (or its surface is new): content changes take one pass (wm_comp_update). */
static int draw_layer(Window *w, int i, int top, Wmc *c, void *a, void *b)
{
    void *buf[2] = {a, b};
    for (int pass = 0; pass < 2; pass++)
    {
        /* where the window is: what it remembers while drawing (its buttons, text boxes) stays in screen coordinates */
        if (gfx_layer_begin_at(buf[pass], w->r.x - c->m, w->r.y - c->m, c->lw, c->lh, pass ? 0x00FFFFFFu : 0) != 0)
            return -1;
        wm_draw_window_internal(w, i, top);
        gfx_layer_end();
    }
    gfx_layer_pack(a, b, c->lw, c->lh);
    return 0;
}

/* Where the window is opaque for sure (relative to its corner), as up to 3 rectangles x0, y0, x1, y1: inside it, away
 * from the rounded corners and the 1-pixel outline. There one drawing pass gives the final pixels; what is outside them
 * (the shadow, the corners, the outline) only changes with the window's look, and then the whole layer is drawn again.
 * Returns how many. */
static int opaque_rects(const Window *w, int out[3][4])
{
    int ww = w->r.w, hh = w->r.h;
    if (w->maximized) /* flat: no corners, no outline, no shadow */
    {
        out[0][0] = 0; out[0][1] = 0; out[0][2] = ww; out[0][3] = hh;
        return 1;
    }
    int r = ui_theme()->radius_l + 2;
    out[0][0] = r;      out[0][1] = 1; out[0][2] = ww - r; out[0][3] = hh - 1;  /* the middle, top to bottom */
    out[1][0] = 1;      out[1][1] = r; out[1][2] = r;      out[1][3] = hh - r;  /* the left strip between the corners */
    out[2][0] = ww - r; out[2][1] = r; out[2][2] = ww - 1; out[2][3] = hh - r;  /* the right one */
    return 3;
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
        if (c->full)
        {
            if (draw_layer(w, i, top, c, a, b) != 0 || ops->upload(c->surf, a, lw, 0, 0, lw, lh, 0) != 0)
                return -1;
            DRAWN_FULL++;
            tally(c, w, (unsigned long long)lw * (unsigned long long)lh * 2ULL); /* two passes */
        }
        else if (c->dirty)
        {
            /* The look is the same, only what it shows changed: one pass over whatever the buffer holds, inside the
             * changed part, and of that only the opaque pieces go up (the shadow and the corners stay as they are) */
            int op[3][4], no = opaque_rects(w, op);
            if (gfx_layer_begin_at_keep(a, w->r.x - m, w->r.y - m, lw, lh) != 0)
                return -1;
            gfx_clip_intersect(w->r.x + c->dx0, w->r.y + c->dy0, c->dx1 - c->dx0, c->dy1 - c->dy0);
            wm_draw_window_internal(w, i, top);
            gfx_layer_end();
            for (int k = 0; k < no; k++)
            {
                int x0 = op[k][0] > c->dx0 ? op[k][0] : c->dx0, y0 = op[k][1] > c->dy0 ? op[k][1] : c->dy0;
                int x1 = op[k][2] < c->dx1 ? op[k][2] : c->dx1, y1 = op[k][3] < c->dy1 ? op[k][3] : c->dy1;
                if (x1 <= x0 || y1 <= y0)
                    continue;
                x0 += m; y0 += m; x1 += m; y1 += m; /* in the layer */
                if (ops->upload(c->surf, (const unsigned int *)a + (long long)y0 * lw + x0, lw, x0, y0, x1 - x0, y1 - y0, 1) != 0)
                    return -1;
            }
            DRAWN_PART++;
            tally(c, w, (unsigned long long)(c->dx1 - c->dx0) * (unsigned long long)(c->dy1 - c->dy0));
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
