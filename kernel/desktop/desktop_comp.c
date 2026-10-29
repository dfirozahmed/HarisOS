/* The desktop through the GPU compositor (gfx.h "compositor"): the wallpaper, each window and the taskbar are
 * surfaces, and for the parts of the screen that changed the GPU puts them together. The CPU draws a window only
 * when what it shows changed (wm_comp.c), the taskbar only when it changed, the wallpaper only once per theme.
 * While a menu is open (menus are not surfaces yet), or when anything fails, the desktop is drawn by the CPU as
 * before; going back to the CPU draws the whole scene once, because the back buffer did not follow. */
#include "desktop.h"
#include "taskbar.h"
#include "startmenu.h"
#include "appmenu.h"
#include "ui.h"
#include "wm.h"
#include "desktop_internal.h"

extern int WALL_VALID;
extern unsigned WALL_GEN; /* desktop.c: counts full redraws (the wallpaper may have changed) */

static int ON;           /* the last frame was composited */
static int WALL_SURF, TB_SURF;
static unsigned WALL_UP; /* WALL_GEN of the uploaded wallpaper */

/* Back to the CPU: the back buffer does not hold the windows' latest pictures, so draw everything once. */
static void leave(void)
{
    if (!ON)
        return;
    ON = 0;
    const GfxCompOps *ops = gfx_comp();
    wm_comp_drop(ops);
    if (ops)
    {
        if (WALL_SURF) ops->surface_free(WALL_SURF);
        if (TB_SURF) ops->surface_free(TB_SURF);
    }
    WALL_SURF = TB_SURF = 0;
    desktop_draw();
    gfx_present();
}

static int overlaps(UiRect a, int x0, int y0, int x1, int y1)
{
    return a.x < x1 && a.x + a.w > x0 && a.y < y1 && a.y + a.h > y0;
}

/* The taskbar as a packed layer (drawn over black and over white: it is partly see-through). */
static int taskbar_layer(const GfxCompOps *ops, void *a, void *b, int w, int h)
{
    void *buf[2] = {a, b};
    for (int pass = 0; pass < 2; pass++)
    {
        if (gfx_layer_begin(buf[pass], w, h, pass ? 0x00FFFFFFu : 0) != 0)
            return -1;
        taskbar_draw(); /* inside the layer the "screen" is w x h, so it lands at its top */
        gfx_layer_end();
    }
    gfx_layer_pack(a, b, w, h);
    return ops->upload(TB_SURF, a, w, 0, 0, w, h, 0);
}

int desktop_comp_frame(const UiRect *areas, int n)
{
    const GfxCompOps *ops = gfx_comp();
    void *a, *b;
    unsigned long long bytes;
    int W = (int)gfx_width(), H = (int)gfx_height(), th = TASKBAR_HEIGHT;
    if (!ops || !WALL_VALID || !gfx_double_buffered() || startmenu_is_open() || appmenu_is_open() ||
        !wm_drag_memory(&a, &b, &bytes) || (unsigned long long)W * (unsigned long long)th * 4ULL > bytes)
    {
        leave();
        return 0;
    }
    int full = !ON;
    if (!ON)
    {
        ON = 1;
        wm_comp_set(1);
    }
    if (!WALL_SURF && !(WALL_SURF = ops->surface_new(W, H)))
        goto fail;
    if (!TB_SURF && !(TB_SURF = ops->surface_new(W, th)))
        goto fail;
    if (full || WALL_UP != WALL_GEN)
    {
        const unsigned int *wall = gfx_cache_pixels();
        if (!wall || ops->upload(WALL_SURF, wall, W, 0, 0, W, H, 1) != 0)
            goto fail;
        WALL_UP = WALL_GEN;
        full = 1;
    }
    int bar = full;
    for (int i = 0; i < n && !bar; i++)
        bar = overlaps(areas[i], 0, H - th, W, H);
    if (bar && taskbar_layer(ops, a, b, W, th) != 0)
        goto fail;
    if (wm_comp_update(ops, a, b, bytes) != 0)
        goto fail;

    GfxCompItem items[2 + 62];
    int k = 0;
    items[k++] = (GfxCompItem){WALL_SURF, 0, 0, W, H, 0};
    k += wm_comp_items(items + k, 62);
    items[k++] = (GfxCompItem){TB_SURF, 0, H - th, W, th, 1};
    int rects[4 * WM_DAMAGE_MAX], nr = 0;
    if (full)
    {
        rects[0] = 0; rects[1] = 0; rects[2] = W; rects[3] = H;
        nr = 1;
    }
    else
        for (int i = 0; i < n && nr < WM_DAMAGE_MAX; i++, nr++)
        {
            rects[4 * nr] = areas[i].x;
            rects[4 * nr + 1] = areas[i].y;
            rects[4 * nr + 2] = areas[i].x + areas[i].w;
            rects[4 * nr + 3] = areas[i].y + areas[i].h;
        }
    if (gfx_comp_present(items, k, rects, nr) != 0)
        goto fail; /* the driver is dropped: the CPU from now on */
    return 1;
fail:
    gfx_comp_driver(0);
    leave();
    return 0;
}
