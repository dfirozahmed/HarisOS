/* HarisOS window manager. See include/wm.h. */
#include "wm.h"
#include "taskbar.h"

static Window POOL[WM_MAX_WINDOWS];
static Window *Z[WM_MAX_WINDOWS]; /* open windows, Z[0] = back, Z[ZN-1] = top (focused) */
static int ZN;
static int NEXT_ID = 1;

/* Damage: the screen area that must be redrawn (union of everything that changed). */
#define SHADOW_MARGIN 48 /* window shadows reach this far beyond the window */
static int HAS_DMG, DMG_X0, DMG_Y0, DMG_X1, DMG_Y1;

static void damage(int x0, int y0, int x1, int y1)
{
    if (!HAS_DMG)
    {
        DMG_X0 = x0;
        DMG_Y0 = y0;
        DMG_X1 = x1;
        DMG_Y1 = y1;
        HAS_DMG = 1;
        return;
    }
    if (x0 < DMG_X0)
        DMG_X0 = x0;
    if (y0 < DMG_Y0)
        DMG_Y0 = y0;
    if (x1 > DMG_X1)
        DMG_X1 = x1;
    if (y1 > DMG_Y1)
        DMG_Y1 = y1;
}

/* a window plus its shadow */
static void damage_window(const Window *w)
{
    damage(w->r.x - SHADOW_MARGIN, w->r.y - SHADOW_MARGIN,
           w->r.x + w->r.w + SHADOW_MARGIN, w->r.y + w->r.h + SHADOW_MARGIN);
}

void wm_invalidate(void)
{
    damage(0, 0, (int)gfx_width(), (int)gfx_height());
}

int wm_take_damage(UiRect *out)
{
    if (!HAS_DMG)
        return 0;
    int W = (int)gfx_width(), H = (int)gfx_height();
    int x0 = DMG_X0 < 0 ? 0 : DMG_X0, y0 = DMG_Y0 < 0 ? 0 : DMG_Y0;
    int x1 = DMG_X1 > W ? W : DMG_X1, y1 = DMG_Y1 > H ? H : DMG_Y1;
    HAS_DMG = 0;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    *out = ui_rect(x0, y0, x1 - x0, y1 - y0);
    return 1;
}

/* drag / close-button state */
static Window *DRAG;
static int DRAG_DX, DRAG_DY;
static Window *CLOSE_PRESSED, *MIN_PRESSED, *MAX_PRESSED;
static int DRAG_PX, DRAG_PY; /* where the drag started (to tell a click from a drag) */
static unsigned LAST_BUTTONS;

/* ---- helpers ------------------------------------------------- */
static void copy_title(char *dst, const char *src)
{
    int i = 0;
    while (src && src[i] && i < WM_TITLE_MAX - 1)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

UiRect wm_content(const Window *w)
{
    int tb = ui_theme()->titlebar_h;
    return ui_rect(w->r.x + 1, w->r.y + tb + 1, w->r.w - 2, w->r.h - tb - 2);
}

/* put the buttons at their real screen position */
static void layout(Window *w)
{
    UiRect c = wm_content(w);
    int tb = ui_theme()->titlebar_h;
    int base_cw = w->base_w - 2, base_ch = w->base_h - tb - 2; /* content size the buttons were placed for */
    for (int i = 0; i < w->btn_count; i++)
    {
        int dx = (w->btn_anchor[i] & WM_ANCHOR_RIGHT) ? c.w - base_cw : 0;
        int dy = (w->btn_anchor[i] & WM_ANCHOR_BOTTOM) ? c.h - base_ch : 0;
        w->btn[i].r = ui_rect(c.x + w->btn_rel[i].x + dx, c.y + w->btn_rel[i].y + dy,
                              w->btn_rel[i].w, w->btn_rel[i].h);
    }
}

static int close_hit(const Window *w, int x, int y)
{
    int cx = w->r.x + 22, cy = w->r.y + ui_theme()->titlebar_h / 2;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= 8 * 8;
}

static int min_hit(const Window *w, int x, int y)
{
    int cx = w->r.x + 42, cy = w->r.y + ui_theme()->titlebar_h / 2;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= 8 * 8;
}

static int max_hit(const Window *w, int x, int y)
{
    int cx = w->r.x + 62, cy = w->r.y + ui_theme()->titlebar_h / 2;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= 8 * 8;
}

/* index in Z of the topmost visible (not minimized) window, or -1 */
static int top_index(void)
{
    for (int i = ZN - 1; i >= 0; i--)
        if (!Z[i]->minimized)
            return i;
    return -1;
}

static int in_titlebar(const Window *w, int x, int y)
{
    return ui_contains(ui_rect(w->r.x, w->r.y, w->r.w, ui_theme()->titlebar_h), x, y);
}

/* topmost window under the point, or NULL */
static Window *window_at(int x, int y)
{
    for (int i = ZN - 1; i >= 0; i--)
        if (!Z[i]->minimized && ui_contains(Z[i]->r, x, y))
            return Z[i];
    return 0;
}

static int z_index(const Window *w)
{
    for (int i = 0; i < ZN; i++)
        if (Z[i] == w)
            return i;
    return -1;
}

/* ---- create / close / raise ---------------------------------- */
Window *wm_create(const char *title, int x, int y, int w, int h, WmDrawFn draw, void *user)
{
    Window *win = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (!POOL[i].used)
        {
            win = &POOL[i];
            break;
        }
    if (!win)
        return 0;

    char *raw = (char *)win; /* clear the slot */
    for (unsigned i = 0; i < sizeof(*win); i++)
        raw[i] = 0;

    win->used = 1;
    win->id = NEXT_ID++;
    win->r = ui_rect(x, y, w, h);
    win->normal = win->r;
    win->base_w = w;
    win->base_h = h;
    copy_title(win->title, title);
    win->on_draw = draw;
    win->user = user;
    ui_buttons_init(&win->set);

    int old = top_index();
    if (old >= 0)
        damage_window(Z[old]); /* it stops being the focused window */
    Z[ZN++] = win;
    damage_window(win);
    return win;
}

void wm_close(Window *w)
{
    int zi = w ? z_index(w) : -1;
    if (zi < 0)
        return;
    if (w->on_close)
        w->on_close(w);
    for (int i = zi; i < ZN - 1; i++)
        Z[i] = Z[i + 1];
    ZN--;
    w->used = 0;
    if (DRAG == w)
        DRAG = 0;
    if (CLOSE_PRESSED == w)
        CLOSE_PRESSED = 0;
    if (MIN_PRESSED == w)
        MIN_PRESSED = 0;
    if (MAX_PRESSED == w)
        MAX_PRESSED = 0;
    damage_window(w);
    int t = top_index();
    if (t >= 0)
        damage_window(Z[t]); /* the next window becomes focused */
}

void wm_raise(Window *w)
{
    int zi = w ? z_index(w) : -1;
    if (zi < 0 || zi == ZN - 1)
        return;
    int old = top_index();
    if (old >= 0)
        damage_window(Z[old]); /* focus look changes */
    for (int i = zi; i < ZN - 1; i++)
        Z[i] = Z[i + 1];
    Z[ZN - 1] = w;
    damage_window(w);
}

void wm_minimize(Window *w)
{
    if (!w || z_index(w) < 0 || w->minimized)
        return;
    w->minimized = 1;
    if (DRAG == w)
        DRAG = 0;
    damage_window(w);
    int t = top_index();
    if (t >= 0)
        damage_window(Z[t]); /* the next window becomes focused */
}

void wm_activate(Window *w)
{
    if (!w || z_index(w) < 0)
        return;
    if (w->minimized)
    {
        int old = top_index();
        if (old >= 0)
            damage_window(Z[old]);
        w->minimized = 0;
        damage_window(w);
    }
    wm_raise(w);
}

int wm_is_open(const Window *w) { return w && z_index(w) >= 0; }
int wm_dragging(void) { return DRAG != 0; }
void wm_add_damage(UiRect r) { damage(r.x, r.y, r.x + r.w, r.y + r.h); }

int wm_list(Window **out, int max)
{
    int n = 0;
    for (int i = 0; i < ZN && n < max; i++)
        out[n++] = Z[i];
    for (int i = 1; i < n; i++) /* oldest first (insertion sort by id) */
    {
        Window *k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j]->id > k->id)
        {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = k;
    }
    return n;
}

int wm_add_button(Window *w, const char *label, UiButtonKind kind, UiRect rel,
                  UiButtonFn on_click, void *user)
{
    return wm_add_button_ex(w, label, kind, rel, 0, on_click, user);
}

void wm_set_maximized(Window *w, int on)
{
    on = on != 0;
    if (!w || z_index(w) < 0 || w->maximized == on)
        return;
    damage_window(w); /* where it was */
    if (on)
    {
        w->normal = w->r;
        w->r = ui_rect(0, 0, (int)gfx_width(), (int)gfx_height() - TASKBAR_HEIGHT);
    }
    else
        w->r = w->normal;
    w->maximized = on;
    layout(w);
    damage_window(w); /* where it is now */
}

int wm_add_button_ex(Window *w, const char *label, UiButtonKind kind, UiRect rel, int anchor,
                     UiButtonFn on_click, void *user)
{
    if (!w || w->btn_count >= WM_MAX_BUTTONS)
        return -1;
    int i = w->btn_count++;
    UiButton *b = &w->btn[i];
    b->r = ui_rect(0, 0, 0, 0);
    b->label = label;
    b->kind = kind;
    b->enabled = 1;
    b->on_click = on_click;
    b->on_press = 0;
    b->on_hover = 0;
    b->on_leave = 0;
    b->user = user;
    w->btn_rel[i] = rel;
    w->btn_anchor[i] = anchor;
    ui_buttons_add(&w->set, b);
    layout(w);
    return 0;
}

int wm_count(void) { return ZN; }
Window *wm_top(void)
{
    int t = top_index();
    return t >= 0 ? Z[t] : 0;
}

/* ---- drawing ------------------------------------------------- */
void wm_draw(void)
{
    int top = top_index();
    for (int i = 0; i < ZN; i++)
    {
        Window *w = Z[i];
        if (w->minimized)
            continue;
        GfxClip saved = gfx_get_clip();
        /* partial redraw: skip windows (and shadows) that are outside the clip */
        if (w->r.x - SHADOW_MARGIN >= saved.x1 || w->r.x + w->r.w + SHADOW_MARGIN <= saved.x0 ||
            w->r.y - SHADOW_MARGIN >= saved.y1 || w->r.y + w->r.h + SHADOW_MARGIN <= saved.y0)
            continue;
        UiRect body = ui_window(w->r, w->title, i == top);
        layout(w);
        gfx_clip_intersect(body.x, body.y, body.w, body.h); /* nothing can spill out of the window */
        if (w->on_draw)
            w->on_draw(w, body);
        ui_buttons_draw(&w->set);
        gfx_restore_clip(saved);
    }
}

/* ---- mouse --------------------------------------------------- */
static void clamp_to_screen(Window *w)
{
    if (w->maximized)
        return;
    int W = (int)gfx_width(), H = (int)gfx_height();
    int tb = ui_theme()->titlebar_h;
    int keep = 120; /* at least this much of the title bar stays on screen */
    if (w->r.x > W - keep)
        w->r.x = W - keep;
    if (w->r.x < keep - w->r.w)
        w->r.x = keep - w->r.w;
    if (w->r.y < 0)
        w->r.y = 0;
    if (w->r.y > H - TASKBAR_HEIGHT - tb)
        w->r.y = H - TASKBAR_HEIGHT - tb;
}

int wm_pointer(int x, int y, unsigned buttons)
{
    int down = (buttons & 1) != 0, was_down = (LAST_BUTTONS & 1) != 0;
    LAST_BUTTONS = buttons;

    /* 1) press: focus, start dragging, or arm the close button */
    if (down && !was_down)
    {
        Window *w = window_at(x, y);
        if (w)
        {
            wm_raise(w);
            if (close_hit(w, x, y))
                CLOSE_PRESSED = w;
            else if (min_hit(w, x, y))
                MIN_PRESSED = w;
            else if (max_hit(w, x, y))
                MAX_PRESSED = w;
            else if (in_titlebar(w, x, y))
            {
                DRAG = w;
                DRAG_PX = x;
                DRAG_PY = y;
                DRAG_DX = x - w->r.x;
                DRAG_DY = y - w->r.y;
            }
        }
    }

    /* 2) move: drag the window. A maximized window first goes back to its normal
     *    size once the pointer has really moved, keeping the pointer at the same relative spot. */
    if (DRAG && down && DRAG->maximized)
    {
        int mx = x - DRAG_PX, my = y - DRAG_PY;
        if (mx < 0)
            mx = -mx;
        if (my < 0)
            my = -my;
        if (mx > 4 || my > 4)
        {
            Window *d = DRAG;
            DRAG_DX = DRAG_DX * d->normal.w / d->r.w;
            wm_set_maximized(d, 0);
        }
    }
    if (DRAG && down && !DRAG->maximized)
    {
        int nx = x - DRAG_DX, ny = y - DRAG_DY;
        if (nx != DRAG->r.x || ny != DRAG->r.y)
        {
            damage_window(DRAG); /* where it was */
            DRAG->r.x = nx;
            DRAG->r.y = ny;
            clamp_to_screen(DRAG);
            layout(DRAG);
            damage_window(DRAG); /* where it is now */
        }
    }

    /* 3) release: finish a drag, or a click on the close button */
    Window *to_close = 0, *to_min = 0, *to_max = 0;
    if (!down && was_down)
    {
        DRAG = 0;
        if (MAX_PRESSED)
        {
            if (z_index(MAX_PRESSED) >= 0 && max_hit(MAX_PRESSED, x, y))
                to_max = MAX_PRESSED;
            MAX_PRESSED = 0;
        }
        if (MIN_PRESSED)
        {
            if (z_index(MIN_PRESSED) >= 0 && min_hit(MIN_PRESSED, x, y))
                to_min = MIN_PRESSED;
            MIN_PRESSED = 0;
        }
        if (CLOSE_PRESSED)
        {
            if (z_index(CLOSE_PRESSED) >= 0 && close_hit(CLOSE_PRESSED, x, y))
                to_close = CLOSE_PRESSED;
            CLOSE_PRESSED = 0;
        }
    }

    /* 4) buttons: only the window under the pointer gets real coordinates,
     *    the others get "far away" so their hover/press state is cleared. */
    Window *target = DRAG ? 0 : window_at(x, y);
    if (target && (CLOSE_PRESSED || to_close || MIN_PRESSED || to_min || MAX_PRESSED || to_max))
        target = 0;
    for (int i = 0; i < ZN; i++)
    {
        Window *w = Z[i];
        int hit = (w == target);
        if (ui_buttons_pointer(&w->set, hit ? x : -1, hit ? y : -1, buttons))
            damage_window(w);
        if (ZN == 0 || z_index(w) < 0) /* a click handler closed it */
            break;
    }

    if (to_close)
        wm_close(to_close);
    else if (to_min)
        wm_minimize(to_min);
    else if (to_max)
        wm_set_maximized(to_max, !to_max->maximized);

    return HAS_DMG;
}
