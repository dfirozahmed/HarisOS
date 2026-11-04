/* Image Viewer app: shows a BMP, PNG or JPEG picture (decoded by kernel/graphics/picture.c).
 * Zoom: the - / + buttons, the keys - and +, the mouse wheel (about the pointer); Fit (key F or 0) shows all of the
 * picture, 100% (key 1) one picture pixel per screen pixel. A picture bigger than the window is moved by dragging it with
 * the mouse or with the Up / Down keys. Left / Right (or PageUp / PageDown, the Prev / Next buttons) go to the other
 * pictures in the same folder. The view maths (zoom steps, position, drawing a row) is in imgview.c.
 * The decoded picture is one block of the kernel heap (4 bytes a pixel) kept while the window is open.
 * An app uses only public headers (ui.h, wm.h, desktop.h, gfx.h) - never desktop_internal.h. */
#include "app.h"
#include "desktop.h"
#include "gfx.h"
#include "heap.h"
#include "imageviewer.h"
#include "imgview.h"
#include "ntfs.h"
#include "picture.h"
#include "taskbar.h"
#include "ui.h"
#include "vfs.h"
#include "wm.h"

#define IV_PAD 8
#define IV_TOOL_H 46
#define IV_STATUS_H 26
#define IV_LINE_MAX 4096 /* the widest view row drawn in one piece */
#define IV_LIST_MAX 256  /* pictures of one folder that Prev / Next walk through */
#define IV_PAN_STEP 48

static Window *IV_WIN;
static unsigned int *IV_PX; /* the decoded picture (IV_W x IV_H), NULL when none is shown */
static int IV_W, IV_H;
static int IV_NODE = -1; /* the file's node in the VFS */
static char IV_NAME[VFS_NAME_MAX];
static int IV_PCT = 100;
static int IV_FIT = 1;   /* 1 = zoomed to show all of the picture; follows the window's size */
static int IV_OX, IV_OY; /* where the picture's top-left lies in the view (set at each draw) */
static const char *IV_NOTE = ""; /* a message: why no picture is shown, or a short remark */
static int IV_DRAGGING, IV_DX, IV_DY; /* the picture is being moved: the pointer's last place */
static int IV_HX = -1, IV_HY = -1;    /* the pointer's last place over the window (screen), for zooming about it */
static unsigned int IV_LINE[IV_LINE_MAX];

/* ---- small text helpers ---- */
static void put_str(char *out, int cap, int *n, const char *s)
{
    while (*s && *n < cap - 1)
        out[(*n)++] = *s++;
    out[*n] = 0;
}

static void put_dec(char *out, int cap, int *n, unsigned v)
{
    char t[12];
    int k = 0;
    do
        t[k++] = (char)('0' + v % 10);
    while ((v /= 10) && k < 11);
    while (k && *n < cap - 1)
        out[(*n)++] = t[--k];
    out[*n] = 0;
}

static UiRect view_rect(UiRect body)
{
    UiRect b = body;
    ui_cut_top(&b, IV_TOOL_H);
    ui_cut_bottom(&b, IV_STATUS_H);
    return ui_inset(b, 4);
}

static const char *vfs_problem(void)
{
    if (vfs_last_error() == VFS_ERR_UNSUPPORTED)
        return "Files cannot be read here (RAM Disk / This PC)";
    return vfs_error_text(vfs_last_error());
}

/* ---- the folder's pictures ---- */

/* The pictures in the folder of the shown file, in the folder's order; the place of the shown one in *at. Returns how many. */
static int folder_pictures(int *list, int *at)
{
    *at = -1;
    const VfsNode *me = IV_NODE >= 0 ? vfs_node(IV_NODE) : 0;
    if (!me)
        return 0;
    int all[IV_LIST_MAX];
    int k = vfs_list(me->parent, all, IV_LIST_MAX);
    if (k > IV_LIST_MAX)
        k = IV_LIST_MAX;
    int n = 0;
    for (int i = 0; i < k; i++)
    {
        const VfsNode *vn = vfs_node(all[i]);
        if (vn && !vn->is_dir && picture_is_name(vn->name))
        {
            if (all[i] == IV_NODE)
                *at = n;
            list[n++] = all[i];
        }
    }
    return n;
}

/* ---- loading ---- */
static void title_set(Window *w)
{
    char t[WM_TITLE_MAX];
    int n = 0;
    t[0] = 0;
    if (IV_NAME[0])
    {
        put_str(t, (int)sizeof t, &n, IV_NAME);
        put_str(t, (int)sizeof t, &n, " - ");
    }
    put_str(t, (int)sizeof t, &n, "Image Viewer");
    for (int i = 0; i <= n; i++)
        w->title[i] = t[i];
}

static void forget_picture(void)
{
    if (IV_PX)
        kfree(IV_PX);
    IV_PX = 0;
    IV_W = IV_H = 0;
    IV_DRAGGING = 0;
}

/* Show node `id`. NULL = done, else why not (the old picture is gone then). */
static const char *load_node(int id)
{
    forget_picture();
    const VfsNode *n = vfs_node(id);
    if (!n)
        return "File not found";
    if (n->is_dir)
        return "That is a folder, not a picture";
    IV_NODE = id;
    int i = 0;
    for (; n->name[i] && i < VFS_NAME_MAX - 1; i++)
        IV_NAME[i] = n->name[i];
    IV_NAME[i] = 0;
    if (IV_WIN)
        title_set(IV_WIN);
    unsigned size = n->size, done = 0;
    if (size < 4)
        return "Not a picture this viewer can read";
    if (size > 128u * 1024u * 1024u)
        return "File is too large";
    unsigned char *raw = kmalloc(size);
    if (!raw)
        return "Out of memory";
    while (done < size)
    {
        int r = vfs_read(id, done, raw + done, size - done);
        if (r < 0)
        {
            kfree(raw);
            return vfs_problem();
        }
        if (r == 0)
            break;
        done += (unsigned)r;
    }
    int w = 0, h = 0;
    unsigned int *px = done == size ? picture_decode(raw, size, &w, &h) : 0;
    kfree(raw);
    if (!px)
        return "Not a picture this viewer can read (BMP, PNG, baseline JPEG), or too large";
    IV_PX = px;
    IV_W = w;
    IV_H = h;
    IV_FIT = 1;
    IV_PCT = 100;
    IV_OX = IV_OY = 0;
    return 0;
}

static void redraw(void)
{
    if (IV_WIN)
        wm_add_damage(IV_WIN->r);
}

/* go to the previous (dir < 0) or next (dir > 0) picture of the folder, around the ends */
static void step_picture(int dir)
{
    int list[IV_LIST_MAX], at;
    int n = folder_pictures(list, &at);
    if (n < 2 || at < 0)
    {
        IV_NOTE = "No other picture in this folder";
        redraw();
        return;
    }
    int to = (at + (dir > 0 ? 1 : n - 1)) % n;
    const char *err = load_node(list[to]);
    IV_NOTE = err ? err : "";
    redraw();
}

/* ---- zoom and position ---- */
static void view_size(int *vw, int *vh)
{
    UiRect v = view_rect(wm_content(IV_WIN));
    *vw = v.w;
    *vh = v.h;
}

/* zoom to new_pct about the view point (cx, cy) */
static void set_zoom(int new_pct, int cx, int cy)
{
    if (!IV_PX)
        return;
    new_pct = imgview_clamp_pct(new_pct);
    imgview_zoom_at(&IV_OX, &IV_OY, IV_PCT, new_pct, cx, cy);
    IV_PCT = new_pct;
    IV_FIT = 0;
    redraw();
}

static void zoom_step(int dir, int use_pointer)
{
    if (!IV_WIN)
        return;
    int vw, vh;
    view_size(&vw, &vh);
    int cx = vw / 2, cy = vh / 2;
    if (use_pointer && IV_HX >= 0)
    {
        UiRect v = view_rect(wm_content(IV_WIN));
        int px = IV_HX - v.x, py = IV_HY - v.y;
        if (px >= 0 && py >= 0 && px < vw && py < vh)
        {
            cx = px;
            cy = py;
        }
    }
    set_zoom(imgview_step_pct(IV_PCT, dir), cx, cy);
}

static void zoom_fit(void)
{
    IV_FIT = 1;
    redraw();
}

static void zoom_actual(void)
{
    if (!IV_WIN)
        return;
    int vw, vh;
    view_size(&vw, &vh);
    set_zoom(100, vw / 2, vh / 2);
}

static void on_prev(UiButton *b, void *u) { (void)b; (void)u; step_picture(-1); }
static void on_next(UiButton *b, void *u) { (void)b; (void)u; step_picture(1); }
static void on_zoom_out(UiButton *b, void *u) { (void)b; (void)u; zoom_step(-1, 0); }
static void on_zoom_in(UiButton *b, void *u) { (void)b; (void)u; zoom_step(1, 0); }
static void on_fit(UiButton *b, void *u) { (void)b; (void)u; zoom_fit(); }
static void on_actual(UiButton *b, void *u) { (void)b; (void)u; zoom_actual(); }

/* ---- drawing ---- */
static unsigned int pack(UiRgb c) { return ((unsigned)c.r << 16) | ((unsigned)c.g << 8) | c.b; }

static void iv_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    UiRect v = view_rect(body);
    unsigned int bg = pack(t->surface_alt);

    if (IV_PX)
    {
        if (IV_FIT)
            IV_PCT = imgview_fit_pct(IV_W, IV_H, v.w, v.h);
        int sw = imgview_scaled(IV_W, IV_PCT), sh = imgview_scaled(IV_H, IV_PCT);
        if (IV_FIT)
            imgview_center(&IV_OX, &IV_OY, sw, sh, v.w, v.h);
        else
            imgview_clamp(&IV_OX, &IV_OY, sw, sh, v.w, v.h);

        GfxClip cl = gfx_get_clip();
        int vw = v.w < IV_LINE_MAX ? v.w : IV_LINE_MAX;
        for (int y = 0; y < v.h; y++)
        {
            int sy = v.y + y;
            if (sy < cl.y0 || sy >= cl.y1)
                continue;
            imgview_row(IV_PX, IV_W, IV_H, IV_PCT, IV_OX, IV_OY, vw, y, bg, IV_LINE);
            gfx_put_pixels(v.x, sy, IV_LINE, vw);
        }
    }
    else
    {
        gfx_fill_rect(v.x, v.y, v.w, v.h, ui_color(t->surface_alt));
        ui_text_in_color(v, IV_NOTE[0] ? IV_NOTE : "Open a picture from the File Explorer", UI_TEXT_BODY, UI_ALIGN_CENTER,
                         t->text_dim);
    }
    gfx_rect(v.x - 1, v.y - 1, v.w + 2, v.h + 2, ui_color(t->border));

    /* status bar: file, size, zoom and the place among the folder's pictures */
    char st[128];
    int n = 0, cap = (int)sizeof st;
    st[0] = 0;
    if (IV_PX)
    {
        put_str(st, cap, &n, IV_NAME);
        put_str(st, cap, &n, "     ");
        put_dec(st, cap, &n, (unsigned)IV_W);
        put_str(st, cap, &n, " x ");
        put_dec(st, cap, &n, (unsigned)IV_H);
        put_str(st, cap, &n, "     ");
        put_dec(st, cap, &n, (unsigned)IV_PCT);
        put_str(st, cap, &n, "%");
        int list[IV_LIST_MAX], at;
        int cnt = folder_pictures(list, &at);
        if (at >= 0)
        {
            put_str(st, cap, &n, "     ");
            put_dec(st, cap, &n, (unsigned)at + 1);
            put_str(st, cap, &n, " of ");
            put_dec(st, cap, &n, (unsigned)cnt);
        }
    }
    UiRect sr = ui_rect(body.x + IV_PAD, body.y + body.h - IV_STATUS_H, body.w - 2 * IV_PAD, IV_STATUS_H);
    ui_text_in_color(sr, st, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    if (IV_PX && IV_NOTE[0])
        ui_text_in_color(sr, IV_NOTE, UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
}

/* ---- input ---- */
static void iv_key(Window *w, const KeyEvent *k)
{
    (void)w;
    if (!k->pressed)
        return;
    int c = k->key;
    if (c == KEY_LEFT || c == KEY_PAGEUP)
        step_picture(-1);
    else if (c == KEY_RIGHT || c == KEY_PAGEDOWN)
        step_picture(1);
    else if (c == KEY_UP || c == KEY_DOWN)
    {
        IV_OY += c == KEY_UP ? IV_PAN_STEP : -IV_PAN_STEP;
        redraw();
    }
    else if (c == '=' || c == '+' || k->ch == '+')
        zoom_step(1, 0);
    else if (c == '-')
        zoom_step(-1, 0);
    else if (c == '0' || c == 'f')
        zoom_fit();
    else if (c == '1')
        zoom_actual();
}

static int iv_wheel(Window *w, int steps)
{
    (void)w;
    if (!IV_PX || steps == 0)
        return 0;
    zoom_step(steps > 0 ? -1 : 1, 1); /* wheel up = zoom in */
    return 1;
}

static int iv_hover(Window *w, int x, int y)
{
    (void)w;
    IV_HX = x;
    IV_HY = y;
    return 0;
}

static int iv_press(Window *w, int x, int y)
{
    (void)w;
    IV_DRAGGING = IV_PX && ui_contains(view_rect(wm_content(IV_WIN)), x, y);
    IV_DX = x;
    IV_DY = y;
    return 0;
}

static int iv_drag(Window *w, int x, int y, unsigned buttons)
{
    (void)w;
    if (!IV_DRAGGING || !(buttons & 1))
    {
        IV_DRAGGING = 0;
        return 0;
    }
    IV_OX += x - IV_DX;
    IV_OY += y - IV_DY;
    IV_DX = x;
    IV_DY = y;
    return 1;
}

static void iv_close(Window *w)
{
    (void)w;
    IV_WIN = 0;
    forget_picture();
    IV_NODE = -1;
    IV_NAME[0] = 0;
    IV_NOTE = "";
}

static void imageviewer_app_open(void)
{
    if (IV_WIN)
    {
        wm_activate(IV_WIN);
        return;
    }
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 760, h = 540;
    if (w > W - 20)
        w = W - 20;
    if (h > H - TASKBAR_HEIGHT - 8)
        h = H - TASKBAR_HEIGHT - 8;
    int x = (W - w) / 2, y = (H - TASKBAR_HEIGHT - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;

    Window *win = wm_create("Image Viewer", x, y, w, h, iv_draw, 0);
    if (!win)
    {
        desktop_set_status("Window limit reached - close one first.");
        wm_invalidate();
        return;
    }
    win->on_key = iv_key;
    win->on_wheel = iv_wheel;
    win->on_hover = iv_hover;
    win->on_press = iv_press;
    win->on_drag = iv_drag;
    win->on_close = iv_close;
    win->icon = TASKBAR_ICON_BROWSER;
    wm_set_min_size(win, 440, 280);
    IV_WIN = win;
    IV_HX = IV_HY = -1;
    desktop_set_status(0);

    int bx = IV_PAD;
    wm_add_button(win, "Prev", UI_BTN_SECONDARY, ui_rect(bx, 8, 76, 30), on_prev, 0);
    wm_set_button_icon(win, 0, UI_ICON_ARROW_LEFT);
    bx += 84;
    wm_add_button(win, "Next", UI_BTN_SECONDARY, ui_rect(bx, 8, 76, 30), on_next, 0);
    wm_set_button_icon(win, 1, UI_ICON_ARROW_RIGHT);
    bx += 96;
    wm_add_button(win, "-", UI_BTN_SECONDARY, ui_rect(bx, 8, 40, 30), on_zoom_out, 0);
    bx += 48;
    wm_add_button(win, "+", UI_BTN_SECONDARY, ui_rect(bx, 8, 40, 30), on_zoom_in, 0);
    bx += 48;
    wm_add_button(win, "Fit", UI_BTN_SECONDARY, ui_rect(bx, 8, 56, 30), on_fit, 0);
    bx += 64;
    wm_add_button(win, "100%", UI_BTN_SECONDARY, ui_rect(bx, 8, 64, 30), on_actual, 0);
    title_set(win);
}

const char *imageviewer_open_file(const char *path)
{
    if (!path || !path[0])
        return "No file given";
    imageviewer_app_open(); /* the window, in front (a new one if none is open) */
    if (!IV_WIN)
        return "Could not open Image Viewer";
    int id = vfs_lookup(path);
    const char *err = id < 0 ? "File not found" : load_node(id);
    IV_NOTE = err ? err : "";
    redraw();
    return err;
}

const AppInfo APP_IMAGEVIEWER = {
    .name = "Image Viewer",
    .desc = "View BMP, PNG and JPEG pictures",
    .icon = UI_ICON_BROWSER,
    .open = imageviewer_app_open,
};
