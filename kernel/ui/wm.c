/* HarisOS window manager. See include/wm.h. */
#include "wm.h"
#include "taskbar.h"
#include "timer.h"
#include "wm_internal.h"

static Window POOL[WM_MAX_WINDOWS];
Window *WM_Z[WM_MAX_WINDOWS]; /* open windows, WM_Z[0] = back, WM_Z[WM_ZN-1] = top (focused) */
int WM_ZN;
static int NEXT_ID = 1;

/* drag / close-button state */
static Window *DRAG;
static int DRAG_DX, DRAG_DY;
static Window *CLOSE_PRESSED, *MIN_PRESSED, *MAX_PRESSED;
#define DOUBLE_CLICK_MS 450     /* two clicks on a title bar within this time = double-click */
static Window *LC_WIN;          /* window of the last title-bar click */
static unsigned long long LC_T; /* ... and when it happened */
static int LC_X, LC_Y;          /* ... and where */
static int DRAG_PX, DRAG_PY;    /* where the drag started (to tell a click from a drag) */
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
void wm_layout_internal(Window *w)
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
    if (w->dialog)
        return 0;
    int cx = w->r.x + 42, cy = w->r.y + ui_theme()->titlebar_h / 2;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= 8 * 8;
}

static int max_hit(const Window *w, int x, int y)
{
    if (w->dialog)
        return 0;
    int cx = w->r.x + 62, cy = w->r.y + ui_theme()->titlebar_h / 2;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= 8 * 8;
}

/* index in WM_Z of the topmost visible (not minimized) window, or -1 */
int wm_top_index_internal(void)
{
    for (int i = WM_ZN - 1; i >= 0; i--)
        if (!WM_Z[i]->minimized)
            return i;
    return -1;
}

static int in_titlebar(const Window *w, int x, int y)
{
    return ui_contains(ui_rect(w->r.x, w->r.y, w->r.w, ui_theme()->titlebar_h), x, y);
}

/* the open dialog that blocks `w` (its owner), or NULL */
static Window *blocker(const Window *w)
{
    for (int i = WM_ZN - 1; i >= 0; i--)
        if (WM_Z[i]->dialog && WM_Z[i]->owner == w)
            return WM_Z[i];
    return 0;
}

/* topmost window under the point, or NULL */
static Window *window_at(int x, int y)
{
    for (int i = WM_ZN - 1; i >= 0; i--)
        if (!WM_Z[i]->minimized && ui_contains(WM_Z[i]->r, x, y))
            return WM_Z[i];
    return 0;
}

static int z_index(const Window *w)
{
    for (int i = 0; i < WM_ZN; i++)
        if (WM_Z[i] == w)
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

    int old = wm_top_index_internal();
    if (old >= 0)
        wm_damage_window_internal(WM_Z[old]); /* it stops being the focused window */
    WM_Z[WM_ZN++] = win;
    wm_damage_window_internal(win);
    return win;
}

Window *wm_create_dialog(Window *owner, const char *title, int x, int y, int w, int h, WmDrawFn draw, void *user)
{
    Window *win = wm_create(title, x, y, w, h, draw, user);
    if (win)
    {
        win->dialog = 1;
        win->owner = owner;
    }
    return win;
}

void wm_close(Window *w)
{
    int zi = w ? z_index(w) : -1;
    if (zi < 0)
        return;
    for (Window *d = blocker(w); d; d = blocker(w)) /* its dialogs close with it */
        wm_close(d);
    zi = z_index(w);
    if (w->on_close)
        w->on_close(w);
    for (int i = zi; i < WM_ZN - 1; i++)
        WM_Z[i] = WM_Z[i + 1];
    WM_ZN--;
    w->used = 0;
    if (DRAG == w)
        DRAG = 0;
    if (CLOSE_PRESSED == w)
        CLOSE_PRESSED = 0;
    if (MIN_PRESSED == w)
        MIN_PRESSED = 0;
    if (MAX_PRESSED == w)
        MAX_PRESSED = 0;
    if (LC_WIN == w)
        LC_WIN = 0;
    wm_damage_window_internal(w);
    int t = wm_top_index_internal();
    if (t >= 0)
        wm_damage_window_internal(WM_Z[t]); /* the next window becomes focused */
}

static void raise_z(Window *w)
{
    int zi = w ? z_index(w) : -1;
    if (zi < 0 || zi == WM_ZN - 1)
        return;
    int old = wm_top_index_internal();
    if (old >= 0)
        wm_damage_window_internal(WM_Z[old]); /* focus look changes */
    for (int i = zi; i < WM_ZN - 1; i++)
        WM_Z[i] = WM_Z[i + 1];
    WM_Z[WM_ZN - 1] = w;
    wm_damage_window_internal(w);
}

/* raising a window with an open dialog brings the dialog up with it */
void wm_raise(Window *w)
{
    raise_z(w);
    Window *d = w ? blocker(w) : 0;
    if (d)
        raise_z(d);
}

void wm_minimize(Window *w)
{
    if (!w || z_index(w) < 0 || w->minimized || w->dialog || blocker(w))
        return;
    w->minimized = 1;
    if (DRAG == w)
        DRAG = 0;
    wm_damage_window_internal(w);
    int t = wm_top_index_internal();
    if (t >= 0)
        wm_damage_window_internal(WM_Z[t]); /* the next window becomes focused */
}

void wm_activate(Window *w)
{
    if (!w || z_index(w) < 0)
        return;
    if (w->minimized)
    {
        int old = wm_top_index_internal();
        if (old >= 0)
            wm_damage_window_internal(WM_Z[old]);
        w->minimized = 0;
        wm_damage_window_internal(w);
    }
    wm_raise(w);
}

int wm_is_open(const Window *w) { return w && z_index(w) >= 0; }
int wm_dragging(void) { return DRAG != 0; }

int wm_list(Window **out, int max)
{
    int n = 0;
    for (int i = 0; i < WM_ZN && n < max; i++)
        if (!WM_Z[i]->dialog) /* dialogs are not listed in the taskbar */
            out[n++] = WM_Z[i];
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
    if (!w || z_index(w) < 0 || w->maximized == on || w->dialog || blocker(w))
        return;
    wm_damage_window_internal(w); /* where it was */
    if (on)
    {
        w->normal = w->r;
        w->r = ui_rect(0, 0, (int)gfx_width(), (int)gfx_height() - TASKBAR_HEIGHT);
    }
    else
        w->r = w->normal;
    w->maximized = on;
    wm_layout_internal(w);
    wm_damage_window_internal(w); /* where it is now */
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
    wm_layout_internal(w);
    return 0;
}

int wm_count(void) { return WM_ZN; }
Window *wm_top(void)
{
    int t = wm_top_index_internal();
    return t >= 0 ? WM_Z[t] : 0;
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
            if (blocker(w))
                w = 0; /* the owner is blocked: the click only brings its dialog to the front */
        }
        if (w)
        {
            if (close_hit(w, x, y))
                CLOSE_PRESSED = w;
            else if (min_hit(w, x, y))
                MIN_PRESSED = w;
            else if (max_hit(w, x, y))
                MAX_PRESSED = w;
            else if (!in_titlebar(w, x, y))
            {
                if (w->on_press && w->on_press(w, x, y))
                    wm_damage_window_internal(w);
            }
            if (!CLOSE_PRESSED && !MIN_PRESSED && !MAX_PRESSED && in_titlebar(w, x, y))
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
            wm_damage_window_internal(DRAG); /* where it was */
            DRAG->r.x = nx;
            DRAG->r.y = ny;
            clamp_to_screen(DRAG);
            wm_layout_internal(DRAG);
            wm_damage_window_internal(DRAG); /* where it is now */
        }
    }

    /* 3) release: finish a drag, or a click on the close button */
    Window *to_close = 0, *to_min = 0, *to_max = 0, *to_toggle = 0;
    if (!down && was_down)
    {
        Window *pressed = DRAG; /* title bar pressed and released without moving = a click */
        DRAG = 0;
        if (pressed)
        {
            int mx = x - DRAG_PX, my = y - DRAG_PY;
            if (mx < 0)
                mx = -mx;
            if (my < 0)
                my = -my;
            if (mx <= 4 && my <= 4 && timer_ready())
            {
                unsigned long long now = timer_ms();
                int gx = x - LC_X, gy = y - LC_Y;
                if (gx < 0)
                    gx = -gx;
                if (gy < 0)
                    gy = -gy;
                if (LC_WIN == pressed && now - LC_T <= DOUBLE_CLICK_MS && gx <= 8 && gy <= 8)
                {
                    to_toggle = pressed; /* double-click: maximize / restore */
                    LC_WIN = 0;
                }
                else
                {
                    LC_WIN = pressed;
                    LC_T = now;
                    LC_X = x;
                    LC_Y = y;
                }
            }
            else
                LC_WIN = 0;
        }
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
    if (target && blocker(target))
        target = 0;
    if (target && (CLOSE_PRESSED || to_close || MIN_PRESSED || to_min || MAX_PRESSED || to_max || to_toggle))
        target = 0;
    for (int i = 0; i < WM_ZN; i++)
    {
        Window *w = WM_Z[i];
        int hit = (w == target);
        if (ui_buttons_pointer(&w->set, hit ? x : -1, hit ? y : -1, buttons))
            wm_damage_window_internal(w);
        if (WM_ZN == 0 || z_index(w) < 0) /* a click handler closed it */
            break;
    }

    if (to_close)
        wm_close(to_close);
    else if (to_min)
        wm_minimize(to_min);
    else if (to_max)
        wm_set_maximized(to_max, !to_max->maximized);
    else if (to_toggle)
        wm_set_maximized(to_toggle, !to_toggle->maximized);

    return WM_HAS_DMG;
}

int wm_key(const KeyEvent *k)
{
    Window *w = wm_top();
    if (!w || !w->on_key)
        return 0;
    w->on_key(w, k);
    wm_damage_window_internal(w); /* the handler probably changed what the window shows */
    return 1;
}
