/* The desktop through the GPU compositor (gfx.h "compositor"): the wallpaper, each window, the open menus and the
 * taskbar are surfaces, and for the parts of the screen that changed the GPU puts them together, in the order the
 * CPU draws them (desktop_draw_region): wallpaper, windows, start menu, app menu, taskbar. The CPU draws a window
 * only when what it shows changed (wm_comp.c), a menu or the taskbar only when something on it changed, the
 * wallpaper only once per theme. When anything fails the desktop is drawn by the CPU as before; going back to the
 * CPU draws the whole scene once, because the back buffer did not follow. */
#include "desktop.h"
#include "taskbar.h"
#include "startmenu.h"
#include "appmenu.h"
#include "ui.h"
#include "wm.h"
#include "desktop_internal.h"
#include "timer.h"

extern int WALL_VALID;
extern unsigned WALL_GEN; /* desktop.c: counts full redraws (the wallpaper may have changed) */

/* A part of the desktop drawn on top of the windows, as a packed layer where it is on the screen. */
typedef struct
{
    int (*is_open)(void);
    UiRect (*bounds)(void); /* everything it draws (shadows included) */
    void (*draw)(void);
    int surf;
    UiRect r;  /* where its surface is */
    int shown; /* it was in the last frame */
} Top;

static UiRect taskbar_rect(void)
{
    int x, y, w, h;
    taskbar_bounds(&x, &y, &w, &h);
    return ui_rect(x, y, w, h);
}
static int always(void) { return 1; }

static Top TOPS[3] = {
    {startmenu_is_open, startmenu_bounds, startmenu_draw, 0, {0, 0, 0, 0}, 0},
    {appmenu_is_open, appmenu_bounds, appmenu_draw, 0, {0, 0, 0, 0}, 0},
    {always, taskbar_rect, taskbar_draw, 0, {0, 0, 0, 0}, 0},
};

static int ON; /* the last frame was composited */
static GfxCompStats STATS;   /* this report's frames so far */
static unsigned TOPS_DRAWN;
static int REPORTS;          /* reports sent: at most REPORT_MAX, so the log stays small */
#define REPORT_FRAMES 300
#define REPORT_MAX 10
static int WALL_SURF;
static unsigned WALL_UP; /* WALL_GEN of the uploaded wallpaper */
#define PTR_MAX 64        /* the pointer's pictures are at most 64 x 64 */
static int PTR_SURF;
static unsigned PTR_UP;   /* gfx_cursor_layer's gen of the uploaded picture, 0 = none */

/* gfx: the pointer moved (or changed). Its old and new boxes are put together again with it on top, by the GPU. They are kept
 * apart from the window manager's damage: nothing changed there, so no menu or taskbar is drawn again for them. */
#define PTR_RECTS 8
static int PRECT[PTR_RECTS][4], NPRECT;
static void pointer_changed(int x, int y, int w, int h)
{
    if (NPRECT == PTR_RECTS) /* full: one box around them all */
    {
        for (int i = 1; i < NPRECT; i++)
        {
            if (PRECT[i][0] < PRECT[0][0]) PRECT[0][0] = PRECT[i][0];
            if (PRECT[i][1] < PRECT[0][1]) PRECT[0][1] = PRECT[i][1];
            if (PRECT[i][2] > PRECT[0][2]) PRECT[0][2] = PRECT[i][2];
            if (PRECT[i][3] > PRECT[0][3]) PRECT[0][3] = PRECT[i][3];
        }
        NPRECT = 1;
    }
    PRECT[NPRECT][0] = x;
    PRECT[NPRECT][1] = y;
    PRECT[NPRECT][2] = x + w;
    PRECT[NPRECT][3] = y + h;
    NPRECT++;
}

int desktop_comp_pointer_pending(void) { return ON && NPRECT > 0; }

/* Back to the CPU: the back buffer does not hold the windows' latest pictures, so draw everything once. */
static void leave(void)
{
    if (!ON)
        return;
    ON = 0;
    gfx_backbuffer_detach(0); /* the CPU draws into it again (all of it, below) */
    gfx_scroll_hook(0);
    gfx_cursor_external(0); /* the CPU draws the pointer again (gfx_present below) */
    const GfxCompOps *ops = gfx_comp();
    wm_comp_drop(ops);
    if (ops && WALL_SURF)
        ops->surface_free(WALL_SURF);
    if (ops && PTR_SURF)
        ops->surface_free(PTR_SURF);
    PTR_SURF = 0;
    PTR_UP = 0;
    WALL_SURF = 0;
    for (int i = 0; i < 3; i++)
    {
        if (ops && TOPS[i].surf)
            ops->surface_free(TOPS[i].surf);
        TOPS[i].surf = TOPS[i].shown = 0;
    }
    desktop_draw();
    gfx_present();
}

static int overlaps(UiRect a, UiRect b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

/* Brings one top part's surface up to date. 0 = done (or nothing to do), -1 = failed. */
static int update_top(Top *t, const GfxCompOps *ops, void *a, void *b, unsigned long long bytes, const UiRect *areas,
                      int n, int full)
{
    if (!t->is_open())
    {
        t->shown = 0;
        return 0;
    }
    UiRect r = t->bounds();
    if (r.w <= 0 || r.h <= 0 || (unsigned long long)r.w * (unsigned long long)r.h * 4ULL > bytes)
        return -1;
    int redraw = full || !t->shown || r.x != t->r.x || r.y != t->r.y;
    if (t->surf && (r.w != t->r.w || r.h != t->r.h))
    {
        ops->surface_free(t->surf);
        t->surf = 0;
    }
    if (!t->surf)
    {
        if (!(t->surf = ops->surface_new(r.w, r.h)))
            return -1;
        redraw = 1;
    }
    for (int i = 0; i < n && !redraw; i++)
        redraw = overlaps(areas[i], r);
    t->r = r;
    t->shown = 1;
    if (!redraw)
        return 0;
    /* drawn twice (over black, over white: menus and the taskbar are partly see-through), where it is on the screen:
     * what it remembers while drawing (where its buttons and rows are) is then right for the clicks */
    void *buf[2] = {a, b};
    for (int pass = 0; pass < 2; pass++)
    {
        if (gfx_layer_begin_at(buf[pass], r.x, r.y, r.w, r.h, pass ? 0x00FFFFFFu : 0) != 0)
            return -1;
        t->draw();
        gfx_layer_end();
    }
    TOPS_DRAWN++;
    gfx_layer_pack(a, b, r.w, r.h);
    return ops->upload(t->surf, a, r.w, 0, 0, r.w, r.h, 0);
}

/* One composed frame: draw_us drawing the surfaces, present_us the compose. Every REPORT_FRAMES frames the sums go to the
 * driver's report (it writes them down), at most REPORT_MAX times. */
static void measure(const GfxCompOps *ops, unsigned long long draw_us, unsigned long long present_us)
{
    unsigned full, part;
    wm_comp_counts(&full, &part);
    STATS.frames++;
    STATS.draw_us += draw_us;
    STATS.present_us += present_us;
    if (draw_us > STATS.draw_us_max) STATS.draw_us_max = draw_us;
    if (present_us > STATS.present_us_max) STATS.present_us_max = present_us;
    STATS.windows_full += full;
    STATS.windows_part += part;
    STATS.tops += TOPS_DRAWN;
    TOPS_DRAWN = 0;
    if (STATS.frames < REPORT_FRAMES || REPORTS >= REPORT_MAX)
        return;
    unsigned why[WM_WHY_COUNT];
    WmBusy busy[3];
    wm_comp_why(why);
    STATS.nwhy = WM_WHY_COUNT < 12 ? WM_WHY_COUNT : 12;
    for (int i = 0; i < 12; i++) /* names: the reasons, then "look" (per window only) */
    {
        STATS.why[i] = i < STATS.nwhy ? why[i] : 0;
        STATS.why_name[i] = wm_why_name(i);
    }
    STATS.nbusy = wm_comp_busiest(busy, 3);
    for (int i = 0; i < STATS.nbusy; i++)
    {
        for (int k = 0; k < 24; k++)
            STATS.busy[i].title[k] = busy[i].title[k];
        STATS.busy[i].redraws = busy[i].redraws;
        STATS.busy[i].pixels = busy[i].pixels;
        STATS.busy[i].us_max = busy[i].us_max;
        for (int r = 0; r < 12 && r <= WM_WHY_COUNT; r++)
        {
            STATS.busy[i].why_n[r] = busy[i].why_n[r];
            STATS.busy[i].why_kpx[r] = (unsigned)(busy[i].why_px[r] / 1000u);
        }
    }
    STATS.first = REPORTS == 0;
    STATS.last = REPORTS + 1 == REPORT_MAX;
    if (ops->report)
        ops->report(&STATS);
    REPORTS++;
    STATS = (GfxCompStats){0};
}

int desktop_comp_frame(const UiRect *areas, int n)
{
    const GfxCompOps *ops = gfx_comp();
    void *a, *b;
    unsigned long long bytes;
    int W = (int)gfx_width(), H = (int)gfx_height();
    if (!ops || !WALL_VALID || !gfx_double_buffered() || !wm_drag_memory(&a, &b, &bytes))
    {
        leave();
        return 0;
    }
    unsigned long long t0 = timer_us();
    int full = !ON;
    if (!ON)
    {
        ON = 1;
        wm_comp_set(1);
        gfx_backbuffer_detach(1); /* the windows' pictures are in their surfaces now: nothing to move in the back buffer */
        gfx_scroll_hook(wm_comp_scroll); /* rows a window scrolls move inside its surface (the GPU moves them) */
        gfx_cursor_external(pointer_changed); /* the pointer too: the CPU no longer reads and writes the screen for it */
    }
    if (!WALL_SURF && !(WALL_SURF = ops->surface_new(W, H)))
        goto fail;
    if (full || WALL_UP != WALL_GEN)
    {
        const unsigned int *wall = gfx_cache_pixels();
        if (!wall || ops->upload(WALL_SURF, wall, W, 0, 0, W, H, 1) != 0)
            goto fail;
        WALL_UP = WALL_GEN;
        full = 1;
    }
    if (wm_comp_update(ops, a, b, bytes) != 0)
        goto fail;
    for (int i = 0; i < 3; i++)
        if (update_top(&TOPS[i], ops, a, b, bytes, areas, n, full) != 0)
            goto fail;

    GfxCompItem items[1 + 60 + 3 + 1];
    int k = 0;
    items[k++] = (GfxCompItem){WALL_SURF, 0, 0, W, H, 0};
    k += wm_comp_items(items + k, 60);
    for (int i = 0; i < 3; i++)
        if (TOPS[i].shown)
            items[k++] = (GfxCompItem){TOPS[i].surf, TOPS[i].r.x, TOPS[i].r.y, TOPS[i].r.w, TOPS[i].r.h, 1};
    /* the pointer, on top of everything (its picture goes up again only when its shape changed) */
    static unsigned int ptr_px[PTR_MAX * PTR_MAX];
    int px, py, pw, ph;
    unsigned gen;
    if (gfx_cursor_layer(ptr_px, PTR_MAX * PTR_MAX, &px, &py, &pw, &ph, &gen))
    {
        if (!PTR_SURF && !(PTR_SURF = ops->surface_new(PTR_MAX, PTR_MAX)))
            goto fail;
        if (gen != PTR_UP)
        {
            if (ops->upload(PTR_SURF, ptr_px, pw, 0, 0, pw, ph, 0) != 0)
                goto fail;
            PTR_UP = gen;
        }
        items[k++] = (GfxCompItem){PTR_SURF, px, py, pw, ph, 1};
    }
    int rects[4 * (WM_DAMAGE_MAX + PTR_RECTS)], nr = 0;
    if (full)
    {
        rects[0] = 0; rects[1] = 0; rects[2] = W; rects[3] = H;
        nr = 1;
    }
    else
    {
        for (int i = 0; i < n && nr < WM_DAMAGE_MAX; i++, nr++)
        {
            rects[4 * nr] = areas[i].x;
            rects[4 * nr + 1] = areas[i].y;
            rects[4 * nr + 2] = areas[i].x + areas[i].w;
            rects[4 * nr + 3] = areas[i].y + areas[i].h;
        }
        for (int i = 0; i < NPRECT; i++, nr++) /* where the pointer was and is */
            for (int c = 0; c < 4; c++)
                rects[4 * nr + c] = PRECT[i][c];
    }
    NPRECT = 0;
    unsigned long long t1 = timer_us();
    if (gfx_comp_present(items, k, rects, nr) != 0)
        goto fail; /* the driver is dropped: the CPU from now on */
    measure(ops, t1 - t0, timer_us() - t1);
    return 1;
fail:
    gfx_comp_driver(0);
    leave();
    return 0;
}
