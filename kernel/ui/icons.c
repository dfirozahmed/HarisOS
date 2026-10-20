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

/* arrows: a shaft and a chevron head, drawn in the tint color */
static void icon_arrow_left(const Frame *f, GfxColor c)
{
    line(f, 38, 24, 10, 24, 6, c);
    line(f, 23, 11, 10, 24, 6, c);
    line(f, 23, 37, 10, 24, 6, c);
}

static void icon_arrow_right(const Frame *f, GfxColor c)
{
    line(f, 10, 24, 38, 24, 6, c);
    line(f, 25, 11, 38, 24, 6, c);
    line(f, 25, 37, 38, 24, 6, c);
}

static void icon_arrow_up(const Frame *f, GfxColor c)
{
    line(f, 24, 38, 24, 10, 6, c);
    line(f, 11, 23, 24, 10, 6, c);
    line(f, 37, 23, 24, 10, 6, c);
}

static void icon_folder_plus(const Frame *f)
{
    icon_files(f);
    GfxColor green = gfx_rgb(52, 190, 120), white = gfx_rgb(255, 255, 255);
    gfx_fill_circle_q(q(f, 36), qy(f, 34), len_q(f, 11), green, 255);
    line(f, 36, 28, 36, 40, 3, white);
    line(f, 30, 34, 42, 34, 3, white);
}

/* scissors: two crossing blades and two finger rings */
static void icon_cut(const Frame *f, GfxColor c)
{
    line(f, 15, 5, 31, 31, 5, c);
    line(f, 33, 5, 17, 31, 5, c);
    gfx_ring_q(q(f, 14), qy(f, 37), len_q(f, 7), len_q(f, 3), c, 255);
    gfx_ring_q(q(f, 34), qy(f, 37), len_q(f, 7), len_q(f, 3), c, 255);
}

/* two sheets: the one behind shows only its top-left edge */
static void icon_copy(const Frame *f, GfxColor c)
{
    line(f, 11, 34, 11, 8, 4, c);
    line(f, 11, 8, 31, 8, 4, c);
    line(f, 31, 8, 31, 14, 4, c);
    line(f, 11, 34, 17, 34, 4, c);
    gfx_round_rect(px(f, 17), py(f, 14), pl(f, 22), pl(f, 28), pl(f, 5), f->s >= 30 ? 3 : 1, c);
}

/* clipboard: board, clip on top, two lines of text */
static void icon_paste(const Frame *f, GfxColor c)
{
    gfx_round_rect(px(f, 8), py(f, 8), pl(f, 32), pl(f, 36), pl(f, 6), f->s >= 30 ? 3 : 1, c);
    rrect(f, 16, 4, 16, 9, 4, c);
    line(f, 16, 24, 32, 24, 4, c);
    line(f, 16, 32, 28, 32, 4, c);
}

/* a text box with an I-beam caret: edit the name */
static void icon_rename(const Frame *f, GfxColor c)
{
    gfx_round_rect(px(f, 4), py(f, 14), pl(f, 40), pl(f, 20), pl(f, 6), f->s >= 30 ? 3 : 1, c);
    line(f, 24, 8, 24, 40, 4, c);
    line(f, 19, 8, 29, 8, 4, c);
    line(f, 19, 40, 29, 40, 4, c);
}

/* a page with a green plus badge: new file */
static void icon_file_plus(const Frame *f, GfxColor c)
{
    gfx_round_rect(px(f, 9), py(f, 4), pl(f, 26), pl(f, 38), pl(f, 5), f->s >= 30 ? 3 : 1, c);
    line(f, 15, 16, 29, 16, 3, c);
    line(f, 15, 24, 29, 24, 3, c);
    GfxColor green = gfx_rgb(52, 190, 120), white = gfx_rgb(255, 255, 255);
    gfx_fill_circle_q(q(f, 36), qy(f, 36), len_q(f, 11), green, 255);
    line(f, 36, 30, 36, 42, 3, white);
    line(f, 30, 36, 42, 36, 3, white);
}

/* bin: handle, lid, tapered body with two slots */
static void icon_trash(const Frame *f, GfxColor c)
{
    line(f, 8, 12, 40, 12, 5, c);
    line(f, 18, 12, 18, 6, 4, c);
    line(f, 18, 6, 30, 6, 4, c);
    line(f, 30, 6, 30, 12, 4, c);
    line(f, 12, 18, 14, 41, 4, c);
    line(f, 14, 41, 34, 41, 4, c);
    line(f, 34, 41, 36, 18, 4, c);
    line(f, 20, 22, 20, 36, 3, c);
    line(f, 28, 22, 28, 36, 3, c);
}

/* three quarters of a circle (8 steps of 45 degrees, top-right gap) with an arrow head at the top */
static void icon_refresh(const Frame *f, GfxColor c)
{
    const int R = 14;
    for (int i = 6; i > 0; i--) /* from the top (6), counter-clockwise on screen, to the right (0) */
    {
        int a = i % 8, b = (i - 1) % 8;
        line(f, 24 + DIR8[a][0] * R / 256, 24 + DIR8[a][1] * R / 256, 24 + DIR8[b][0] * R / 256,
             24 + DIR8[b][1] * R / 256, 5, c);
    }
    int head[6] = {q(f, 22), qy(f, 2), q(f, 36), qy(f, 10), q(f, 22), qy(f, 18)};
    gfx_fill_poly_q(head, 3, c, 255);
}

/* three bars getting shorter and a down arrow: sort */
static void icon_sort(const Frame *f, GfxColor c)
{
    line(f, 6, 12, 26, 12, 5, c);
    line(f, 6, 24, 20, 24, 5, c);
    line(f, 6, 36, 14, 36, 5, c);
    line(f, 37, 8, 37, 38, 5, c);
    line(f, 29, 30, 37, 39, 5, c);
    line(f, 45, 30, 37, 39, 5, c);
}

void ui_icon(UiIcon icon, int cx, int cy, int size, UiRgb tint)
{
    /* A zero/negative size has no drawable area. Reject it before deriving
     * coordinates so small or invalid layout values cannot create odd shapes. */
    if (size <= 0 || !gfx_ready())
        return;

    Frame f = {cx - size / 2, cy - size / 2, size};

    /* Keep anti-aliased edges inside this icon's allocation. Without a local
     * clip, a thick line or ring can paint into a neighboring taskbar icon. */
    GfxClip previous_clip = gfx_get_clip();
    gfx_clip_intersect(f.ox, f.oy, size, size);

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
    case UI_ICON_ARROW_LEFT:
        icon_arrow_left(&f, t);
        break;
    case UI_ICON_ARROW_RIGHT:
        icon_arrow_right(&f, t);
        break;
    case UI_ICON_ARROW_UP:
        icon_arrow_up(&f, t);
        break;
    case UI_ICON_FOLDER_PLUS:
        icon_folder_plus(&f);
        break;
    case UI_ICON_CUT:
        icon_cut(&f, t);
        break;
    case UI_ICON_COPY:
        icon_copy(&f, t);
        break;
    case UI_ICON_PASTE:
        icon_paste(&f, t);
        break;
    case UI_ICON_RENAME:
        icon_rename(&f, t);
        break;
    case UI_ICON_FILE_PLUS:
        icon_file_plus(&f, t);
        break;
    case UI_ICON_TRASH:
        icon_trash(&f, t);
        break;
    case UI_ICON_REFRESH:
        icon_refresh(&f, t);
        break;
    case UI_ICON_SORT:
        icon_sort(&f, t);
        break;
    default:
        break;
    }

    /* ui_icon() is nestable: restore the caller's clip exactly as received. */
    gfx_restore_clip(previous_clip);
}
