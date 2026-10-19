/* HarisOS window manager. See include/wm.h. */
#include "wm.h"
#include "taskbar.h"
#include "timer.h"
#include "wm_internal.h"

static Window POOL[WM_MAX_WINDOWS];
Window *WM_Z[WM_MAX_WINDOWS]; /* open windows, WM_Z[0] = back, WM_Z[WM_ZN-1] = top (focused) */
int WM_ZN;
static int NEXT_ID = 1;


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

/* index in WM_Z of the topmost visible (not minimized) window, or -1 */
int wm_top_index_internal(void)
{
    for (int i = WM_ZN - 1; i >= 0; i--)
        if (!WM_Z[i]->minimized)
            return i;
    return -1;
}

/* the open dialog that blocks `w` (its owner), or NULL */
Window *wm_blocker_internal(const Window *w)
{
    for (int i = WM_ZN - 1; i >= 0; i--)
        if (WM_Z[i]->dialog && WM_Z[i]->owner == w)
            return WM_Z[i];
    return 0;
}

/* topmost window under the point, or NULL */
Window *wm_window_at_internal(int x, int y)
{
    for (int i = WM_ZN - 1; i >= 0; i--)
        if (!WM_Z[i]->minimized && ui_contains(WM_Z[i]->r, x, y))
            return WM_Z[i];
    return 0;
}

int wm_z_index_internal(const Window *w)
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
    win->min_w = w * 3 / 4 < 180 ? (w < 180 ? w : 180) : w * 3 / 4; /* about 3/4 of the size, but not tiny */
    win->min_h = h * 3 / 4 < 120 ? (h < 120 ? h : 120) : h * 3 / 4;
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
    int zi = w ? wm_z_index_internal(w) : -1;
    if (zi < 0)
        return;
    for (Window *d = wm_blocker_internal(w); d; d = wm_blocker_internal(w)) /* its dialogs close with it */
        wm_close(d);
    zi = wm_z_index_internal(w);
    if (w->on_close)
        w->on_close(w);
    for (int i = zi; i < WM_ZN - 1; i++)
        WM_Z[i] = WM_Z[i + 1];
    WM_ZN--;
    w->used = 0;
    wm_input_forget_window(w);
    wm_damage_window_internal(w);
    int t = wm_top_index_internal();
    if (t >= 0)
        wm_damage_window_internal(WM_Z[t]); /* the next window becomes focused */
}

static void raise_z(Window *w)
{
    int zi = w ? wm_z_index_internal(w) : -1;
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
    Window *d = w ? wm_blocker_internal(w) : 0;
    if (d)
        raise_z(d);
}

void wm_minimize(Window *w)
{
    if (!w || wm_z_index_internal(w) < 0 || w->minimized || w->dialog || wm_blocker_internal(w))
        return;
    w->minimized = 1;
    wm_input_forget_window(w);
    wm_damage_window_internal(w);
    int t = wm_top_index_internal();
    if (t >= 0)
        wm_damage_window_internal(WM_Z[t]); /* the next window becomes focused */
}

void wm_activate(Window *w)
{
    if (!w || wm_z_index_internal(w) < 0)
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

int wm_is_open(const Window *w) { return w && wm_z_index_internal(w) >= 0; }

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

void wm_set_resizable(Window *w, int on)
{
    if (w)
        w->fixed_size = !on;
}

void wm_set_min_size(Window *w, int min_w, int min_h)
{
    if (!w)
        return;
    w->min_w = min_w > 60 ? min_w : 60;
    w->min_h = min_h > ui_theme()->titlebar_h + 40 ? min_h : ui_theme()->titlebar_h + 40;
}

void wm_set_maximized(Window *w, int on)
{
    on = on != 0;
    if (!w || wm_z_index_internal(w) < 0 || w->maximized == on || w->dialog || wm_blocker_internal(w))
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


