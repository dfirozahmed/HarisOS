/* Damage tracking and invalidation for the window manager. */
#include "wm_internal.h"

/* Damage: the screen area that must be redrawn, kept as a few separate rectangles that never overlap.
 * One bounding box would also cover the empty corners between two far-apart changes (the old and the
 * new place of a window dragged diagonally), and every pixel of it costs redraw time. */
typedef struct
{
    int x0, y0, x1, y1;
} DmgRect;

static DmgRect DMG[WM_DAMAGE_MAX];
static int DMG_N;
int WM_HAS_DMG; /* DMG_N > 0 */
Window *WM_CALLING; /* the window whose handler runs now, or 0 (see WM_TIMED) */

/* Add [x0,x1) x [y0,y1) minus what DMG[from..] already covers. A rectangle that only partly overlaps
 * one in the list is cut into up to four strips around it, and each strip is added on its own. */
static void add_from(int x0, int y0, int x1, int y1, int from)
{
    for (int i = from; i < DMG_N; i++)
    {
        DmgRect e = DMG[i];
        if (x0 >= e.x1 || x1 <= e.x0 || y0 >= e.y1 || y1 <= e.y0)
            continue; /* apart */
        if (x0 >= e.x0 && x1 <= e.x1 && y0 >= e.y0 && y1 <= e.y1)
            return; /* already covered */
        int cy0 = y0 > e.y0 ? y0 : e.y0, cy1 = y1 < e.y1 ? y1 : e.y1; /* rows shared with e */
        if (y0 < e.y0)
            add_from(x0, y0, x1, e.y0, i + 1); /* above e */
        if (y1 > e.y1)
            add_from(x0, e.y1, x1, y1, i + 1); /* below e */
        if (x0 < e.x0)
            add_from(x0, cy0, e.x0, cy1, i + 1); /* left of e */
        if (x1 > e.x1)
            add_from(e.x1, cy0, x1, cy1, i + 1); /* right of e */
        return;
    }
    if (DMG_N == WM_DAMAGE_MAX)
    {
        /* List full: fall back to one box around everything. */
        for (int i = 1; i < DMG_N; i++)
        {
            if (DMG[i].x0 < DMG[0].x0) DMG[0].x0 = DMG[i].x0;
            if (DMG[i].y0 < DMG[0].y0) DMG[0].y0 = DMG[i].y0;
            if (DMG[i].x1 > DMG[0].x1) DMG[0].x1 = DMG[i].x1;
            if (DMG[i].y1 > DMG[0].y1) DMG[0].y1 = DMG[i].y1;
        }
        DMG_N = 1;
        if (x0 < DMG[0].x0) DMG[0].x0 = x0;
        if (y0 < DMG[0].y0) DMG[0].y0 = y0;
        if (x1 > DMG[0].x1) DMG[0].x1 = x1;
        if (y1 > DMG[0].y1) DMG[0].y1 = y1;
        return;
    }
    DMG[DMG_N++] = (DmgRect){x0, y0, x1, y1};
}

void wm_damage_internal(int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0)
        return;
    add_from(x0, y0, x1, y1, 0);
    WM_HAS_DMG = DMG_N > 0;
}

/* a window plus its shadow */
void wm_damage_window_internal(const Window *w)
{
    wm_damage_internal(w->r.x - WM_SHADOW_MARGIN, w->r.y - WM_SHADOW_MARGIN,
           w->r.x + w->r.w + WM_SHADOW_MARGIN, w->r.y + w->r.h + WM_SHADOW_MARGIN);
}

/* the window looks different (a handler said so): its surface is drawn again, and the screen there */
void wm_damage_content_internal(const Window *w, int why)
{
    wm_comp_why_internal(why);
    wm_comp_content_internal(w, w->r.x, w->r.y, w->r.x + w->r.w, w->r.y + w->r.h);
    wm_damage_window_internal(w);
}

void wm_invalidate(void)
{
    wm_comp_why_internal(WM_WHY_INVALIDATE);
    wm_comp_all_internal(); /* apps ask with this for their own window to be drawn again */
    wm_damage_internal(0, 0, (int)gfx_width(), (int)gfx_height());
}

int wm_take_damage_rects(UiRect *out, int max)
{
    int W = (int)gfx_width(), H = (int)gfx_height();
    int n = 0;
    for (int i = 0; i < DMG_N && n < max; i++)
    {
        int x0 = DMG[i].x0 < 0 ? 0 : DMG[i].x0, y0 = DMG[i].y0 < 0 ? 0 : DMG[i].y0;
        int x1 = DMG[i].x1 > W ? W : DMG[i].x1, y1 = DMG[i].y1 > H ? H : DMG[i].y1;
        if (x1 > x0 && y1 > y0)
            out[n++] = ui_rect(x0, y0, x1 - x0, y1 - y0);
    }
    DMG_N = 0;
    WM_HAS_DMG = 0;
    return n;
}

int wm_take_damage(UiRect *out)
{
    UiRect r[WM_DAMAGE_MAX];
    int n = wm_take_damage_rects(r, WM_DAMAGE_MAX);
    if (n == 0)
        return 0;
    int x0 = r[0].x, y0 = r[0].y, x1 = r[0].x + r[0].w, y1 = r[0].y + r[0].h;
    for (int i = 1; i < n; i++)
    {
        if (r[i].x < x0) x0 = r[i].x;
        if (r[i].y < y0) y0 = r[i].y;
        if (r[i].x + r[i].w > x1) x1 = r[i].x + r[i].w;
        if (r[i].y + r[i].h > y1) y1 = r[i].y + r[i].h;
    }
    *out = ui_rect(x0, y0, x1 - x0, y1 - y0);
    return 1;
}


/* a part of the screen that is not inside a window changed (taskbar, clock, menus): no window surface is drawn again */
void wm_add_screen_damage(UiRect r)
{
    wm_damage_internal(r.x, r.y, r.x + r.w, r.y + r.h);
}

void wm_add_damage(UiRect r)
{
    wm_comp_why_internal(WM_WHY_AREA);
    if (WM_CALLING) /* a window's own handler: its own content changed there */
        wm_comp_content_internal(WM_CALLING, r.x, r.y, r.x + r.w, r.y + r.h);
    else /* from elsewhere: whatever window is there */
        wm_comp_content_screen_internal(r.x, r.y, r.x + r.w, r.y + r.h);
    wm_damage_internal(r.x, r.y, r.x + r.w, r.y + r.h);
}
