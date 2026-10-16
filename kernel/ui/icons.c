/* HarisUI vector icons. See ui_icon() in include/ui.h.
 *
 * Each icon is drawn on a 48x48 design grid and scaled to the requested size,
 * using only the smooth gfx shapes, so icons stay sharp at any size. */
#include "ui.h"

/* sin/cos of k*45 degrees, scaled by 256 (used for the gear teeth) */
static const short DIR8[8][2] = {{256, 0}, {181, 181}, {0, 256}, {-181, 181},
                                 {-256, 0}, {-181, -181}, {0, -256}, {181, -181}};

typedef struct
{
    int ox, oy; /* top-left of the icon in pixels */
    int s;      /* size in pixels */
} Frame;

/* grid units -> Q4 pixels / whole pixels */
static int q(const Frame *f, int u) { return f->ox * 16 + u * f->s * 16 / 48; }
static int qy(const Frame *f, int u) { return f->oy * 16 + u * f->s * 16 / 48; }
static int len_q(const Frame *f, int u) { return u * f->s * 16 / 48; }
static int px(const Frame *f, int u) { return f->ox + (u * f->s + 24) / 48; }
static int py(const Frame *f, int u) { return f->oy + (u * f->s + 24) / 48; }
static int pl(const Frame *f, int u) { return (u * f->s + 24) / 48; }

static void rrect(const Frame *f, int x, int y, int w, int h, int r, GfxColor c)
{
    int rw = pl(f, w), rh = pl(f, h);
    gfx_fill_round_rect(px(f, x), py(f, y), rw > 0 ? rw : 1, rh > 0 ? rh : 1, pl(f, r), c);
}

static void line(const Frame *f, int x0, int y0, int x1, int y1, int w, GfxColor c)
{
    int wq = len_q(f, w);
    gfx_line_q(q(f, x0), qy(f, y0), q(f, x1), qy(f, y1), wq < 16 ? 16 : wq, c, 255);
}

static void icon_logo(const Frame *f)
{
    GfxColor white = gfx_rgb(255, 255, 255);
    gfx_fill_round_rect_gradient(f->ox, f->oy, f->s, f->s, f->s * 3 / 10, gfx_rgb(104, 168, 255), gfx_rgb(48, 112, 240));
    rrect(f, 12, 11, 6, 26, 2, white); /* left stem */
    rrect(f, 30, 11, 6, 26, 2, white); /* right stem */
    rrect(f, 12, 21, 24, 6, 1, white); /* crossbar */
}

static void icon_files(const Frame *f)
{
    rrect(f, 3, 8, 20, 14, 5, gfx_rgb(224, 148, 36));                  /* tab */
    rrect(f, 3, 13, 42, 30, 6, gfx_rgb(240, 166, 48));                 /* back */
    gfx_fill_round_rect_gradient(px(f, 3), py(f, 20), pl(f, 42), pl(f, 23), pl(f, 6),
                                 gfx_rgb(255, 214, 110), gfx_rgb(250, 182, 62)); /* front */
}

static void icon_terminal(const Frame *f)
{
    GfxColor green = gfx_rgb(110, 231, 160);
    rrect(f, 3, 6, 42, 36, 7, gfx_rgb(86, 100, 128));  /* frame */
    rrect(f, 5, 8, 38, 32, 6, gfx_rgb(16, 22, 34));    /* screen */
    line(f, 13, 18, 21, 24, 4, green);
    line(f, 21, 24, 13, 30, 4, green);
    line(f, 25, 31, 35, 31, 4, green);
}

static void icon_settings(const Frame *f, GfxColor c)
{
    int cx = q(f, 24), cy = qy(f, 24);
    for (int i = 0; i < 8; i++)
    {
        int a = len_q(f, 14), b = len_q(f, 20);
        gfx_line_q(cx + DIR8[i][0] * a / 256, cy + DIR8[i][1] * a / 256,
                   cx + DIR8[i][0] * b / 256, cy + DIR8[i][1] * b / 256, len_q(f, 7), c, 255);
    }
    gfx_ring_q(cx, cy, len_q(f, 12), len_q(f, 9), c, 255); /* the body: a thick ring leaves a real hole */
}

static void icon_browser(const Frame *f)
{
    GfxColor ring = gfx_rgb(214, 232, 255);
    int cx = q(f, 24), cy = qy(f, 24), r = len_q(f, 20);
    gfx_fill_circle_q(cx, cy, r, gfx_rgb(52, 130, 240), 255);
    gfx_ring_q(cx, cy, r - len_q(f, 1), len_q(f, 2), ring, 255);
    line(f, 4, 24, 44, 24, 2, ring);   /* equator */
    line(f, 24, 4, 24, 44, 2, ring);   /* meridian */
    line(f, 8, 14, 40, 14, 2, ring);   /* latitudes */
    line(f, 8, 34, 40, 34, 2, ring);
    gfx_ring_q(cx, cy, len_q(f, 10), len_q(f, 2), ring, 255);
}

static void icon_search(const Frame *f, GfxColor c)
{
    gfx_ring_q(q(f, 21), qy(f, 21), len_q(f, 13), len_q(f, 5), c, 255);
    line(f, 31, 31, 42, 42, 6, c);
}

static void icon_wifi(const Frame *f, GfxColor c)
{
    for (int i = 0; i < 4; i++) /* four signal bars, growing */
    {
        int h = 9 + i * 8;
        rrect(f, 5 + i * 10, 42 - h, 7, h, 3, c);
    }
}

static void icon_volume(const Frame *f, GfxColor c)
{
    rrect(f, 4, 18, 10, 12, 2, c); /* speaker box */
    int cone[8] = {q(f, 12), qy(f, 18), q(f, 25), qy(f, 7), q(f, 25), qy(f, 41), q(f, 12), qy(f, 30)};
    gfx_fill_poly_q(cone, 4, c, 255);
    GfxClip saved = gfx_get_clip();
    gfx_clip_intersect(px(f, 30), f->oy, f->s, f->s); /* sound waves: right part of two rings */
    gfx_ring_q(q(f, 24), qy(f, 24), len_q(f, 10), len_q(f, 3), c, 255);
    gfx_ring_q(q(f, 24), qy(f, 24), len_q(f, 17), len_q(f, 3), c, 255);
    gfx_restore_clip(saved);
}

static void icon_battery(const Frame *f, GfxColor c)
{
    gfx_round_rect(px(f, 2), py(f, 12), pl(f, 38), pl(f, 24), pl(f, 6), f->s >= 30 ? 2 : 1, c);
    rrect(f, 41, 20, 5, 8, 2, c);                     /* contact */
    rrect(f, 7, 17, 28, 14, 3, gfx_rgb(84, 214, 140)); /* charge */
}

static void icon_close(const Frame *f, GfxColor c)
{
    line(f, 12, 12, 36, 36, 5, c);
    line(f, 36, 12, 12, 36, 5, c);
}

static void icon_check(const Frame *f, GfxColor c)
{
    line(f, 9, 25, 20, 36, 6, c);
    line(f, 20, 36, 40, 12, 6, c);
}

void ui_icon(UiIcon icon, int cx, int cy, int size, UiRgb tint)
{
    Frame f = {cx - size / 2, cy - size / 2, size};
    GfxColor t = ui_color(tint);
    switch (icon)
    {
    case UI_ICON_LOGO:
        icon_logo(&f);
        break;
    case UI_ICON_FILES:
        icon_files(&f);
        break;
    case UI_ICON_TERMINAL:
        icon_terminal(&f);
        break;
    case UI_ICON_SETTINGS:
        icon_settings(&f, t);
        break;
    case UI_ICON_BROWSER:
        icon_browser(&f);
        break;
    case UI_ICON_SEARCH:
        icon_search(&f, t);
        break;
    case UI_ICON_WIFI:
        icon_wifi(&f, t);
        break;
    case UI_ICON_VOLUME:
        icon_volume(&f, t);
        break;
    case UI_ICON_BATTERY:
        icon_battery(&f, t);
        break;
    case UI_ICON_CLOSE:
        icon_close(&f, t);
        break;
    case UI_ICON_CHECK:
        icon_check(&f, t);
        break;
    }
}
