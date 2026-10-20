/* Window-manager pointer and keyboard interaction. */
#include "wm.h"
#include "taskbar.h"
#include "timer.h"
#include "wm_internal.h"

#define DOUBLE_CLICK_MS 450 /* two title-bar clicks within this time toggle maximize */
static Window *DRAG;
static int DRAG_DX, DRAG_DY;
static Window *CLOSE_PRESSED, *MIN_PRESSED, *MAX_PRESSED;
static Window *LC_WIN;
static Window *PRESS_WIN; /* window whose on_press started a drag (text selection): it gets on_drag */
static Window *HOVER_CONTENT; /* window whose on_hover last saw the pointer inside */
static unsigned long long LC_T;
static int LC_X, LC_Y;
static int DRAG_PX, DRAG_PY;
static unsigned LAST_BUTTONS;

/* title-bar button feedback: which button of which window the pointer is on, and which is held down */
static Window *HOVER_WIN;
static int HOVER_BTN = -1;
static Window *SIG_HW, *SIG_CP, *SIG_MP, *SIG_XP; /* what was drawn last, to notice changes */
static int SIG_HB = -1;

/* resizing by an edge or corner */
#define RESIZE_GRAB 6   /* pixels inside the window border that grab the edge */
#define RESIZE_CORNER 14 /* ... and how far along the edge a corner reaches */
static Window *RESIZE;
static int RS_EDGE;      /* WM_EDGE_* being dragged */
static int RS_PX, RS_PY; /* pointer position when the resize started */
static UiRect RS_START;  /* window rectangle when it started */

static int close_hit(const Window *w, int x, int y)
{
    int cx = w->r.x + 22, cy = w->r.y + ui_theme()->titlebar_h / 2;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= 8 * 8;
}

static int min_hit(const Window *w, int x, int y)
{
    if (w->dialog) return 0;
    int cx = w->r.x + 42, cy = w->r.y + ui_theme()->titlebar_h / 2;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= 8 * 8;
}

static int max_hit(const Window *w, int x, int y)
{
    if (w->dialog) return 0;
    int cx = w->r.x + 62, cy = w->r.y + ui_theme()->titlebar_h / 2;
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= 8 * 8;
}

static int in_titlebar(const Window *w, int x, int y)
{
    return ui_contains(ui_rect(w->r.x, w->r.y, w->r.w, ui_theme()->titlebar_h), x, y);
}

int wm_titlebtn_hover(const Window *w) { return w == HOVER_WIN ? HOVER_BTN : -1; }

int wm_titlebtn_pressed(const Window *w)
{
    return CLOSE_PRESSED == w ? 0 : MIN_PRESSED == w ? 1 : MAX_PRESSED == w ? 2 : -1;
}

static void damage_titlebar(const Window *w)
{
    if (w && wm_z_index_internal(w) >= 0)
        wm_damage_internal(w->r.x, w->r.y, w->r.x + w->r.w, w->r.y + ui_theme()->titlebar_h);
}

/* find the button under the pointer and redraw the title bars whose buttons changed look */
static void update_button_feedback(int x, int y)
{
    Window *hw = 0;
    int hb = -1;
    if (!DRAG && !RESIZE)
    {
        Window *t = wm_window_at_internal(x, y);
        if (t && !wm_blocker_internal(t))
        {
            hb = close_hit(t, x, y) ? 0 : min_hit(t, x, y) ? 1 : max_hit(t, x, y) ? 2 : -1;
            if (hb >= 0)
                hw = t;
        }
    }
    HOVER_WIN = hw;
    HOVER_BTN = hb;
    if (hw != SIG_HW || hb != SIG_HB || CLOSE_PRESSED != SIG_CP || MIN_PRESSED != SIG_MP || MAX_PRESSED != SIG_XP)
    {
        damage_titlebar(SIG_HW); /* before ... */
        damage_titlebar(SIG_CP);
        damage_titlebar(SIG_MP);
        damage_titlebar(SIG_XP);
        damage_titlebar(hw); /* ... and after */
        damage_titlebar(CLOSE_PRESSED);
        damage_titlebar(MIN_PRESSED);
        damage_titlebar(MAX_PRESSED);
        SIG_HW = hw;
        SIG_HB = hb;
        SIG_CP = CLOSE_PRESSED;
        SIG_MP = MIN_PRESSED;
        SIG_XP = MAX_PRESSED;
    }
}

/* the resize edge(s) under (x, y) for window w, 0 = none */
static int resize_edge(const Window *w, int x, int y)
{
    if (w->dialog || w->fixed_size || w->maximized || w->minimized)
        return 0;
    int e = 0, g = RESIZE_GRAB, c = RESIZE_CORNER;
    if (x < w->r.x + g)
        e |= WM_EDGE_LEFT;
    else if (x >= w->r.x + w->r.w - g)
        e |= WM_EDGE_RIGHT;
    if (y < w->r.y + g)
        e |= WM_EDGE_TOP;
    else if (y >= w->r.y + w->r.h - g)
        e |= WM_EDGE_BOTTOM;
    if (!e)
        return 0;
    if (!(e & (WM_EDGE_LEFT | WM_EDGE_RIGHT))) /* on the top / bottom edge, near a corner */
    {
        if (x < w->r.x + c)
            e |= WM_EDGE_LEFT;
        else if (x >= w->r.x + w->r.w - c)
            e |= WM_EDGE_RIGHT;
    }
    if (!(e & (WM_EDGE_TOP | WM_EDGE_BOTTOM))) /* on the left / right edge, near a corner */
    {
        if (y < w->r.y + c)
            e |= WM_EDGE_TOP;
        else if (y >= w->r.y + w->r.h - c)
            e |= WM_EDGE_BOTTOM;
    }
    return e;
}

int wm_resize_hint(int x, int y)
{
    if (RESIZE)
        return RS_EDGE;
    if (DRAG)
        return 0;
    Window *w = wm_window_at_internal(x, y);
    if (!w || wm_blocker_internal(w))
        return 0;
    return resize_edge(w, x, y);
}

/* move the dragged edges to follow the pointer, within the minimum size and the screen */
static void apply_resize(int x, int y)
{
    Window *w = RESIZE;
    int W = (int)gfx_width(), H = (int)gfx_height() - TASKBAR_HEIGHT;
    int l = RS_START.x, t = RS_START.y, r = RS_START.x + RS_START.w, b = RS_START.y + RS_START.h;
    int dx = x - RS_PX, dy = y - RS_PY;
    if (RS_EDGE & WM_EDGE_LEFT)
        l += dx;
    if (RS_EDGE & WM_EDGE_RIGHT)
        r += dx;
    if (RS_EDGE & WM_EDGE_TOP)
        t += dy;
    if (RS_EDGE & WM_EDGE_BOTTOM)
        b += dy;

    /* stay on the screen (a window that already stuck out may keep what it had) */
    int lmin = RS_START.x < 0 ? RS_START.x : 0, tmin = RS_START.y < 0 ? RS_START.y : 0;
    int rmax = RS_START.x + RS_START.w > W ? RS_START.x + RS_START.w : W;
    int bmax = RS_START.y + RS_START.h > H ? RS_START.y + RS_START.h : H;
    if (l < lmin && (RS_EDGE & WM_EDGE_LEFT))
        l = lmin;
    if (t < tmin && (RS_EDGE & WM_EDGE_TOP))
        t = tmin;
    if (r > rmax && (RS_EDGE & WM_EDGE_RIGHT))
        r = rmax;
    if (b > bmax && (RS_EDGE & WM_EDGE_BOTTOM))
        b = bmax;

    /* not smaller than the minimum: the edge that is NOT being dragged stays put */
    if (r - l < w->min_w)
    {
        if (RS_EDGE & WM_EDGE_LEFT)
            l = r - w->min_w;
        else
            r = l + w->min_w;
    }
    if (b - t < w->min_h)
    {
        if (RS_EDGE & WM_EDGE_TOP)
            t = b - w->min_h;
        else
            b = t + w->min_h;
    }

    if (l == w->r.x && t == w->r.y && r - l == w->r.w && b - t == w->r.h)
        return;
    wm_damage_window_internal(w); /* where it was */
    w->r = ui_rect(l, t, r - l, b - t);
    wm_layout_internal(w);
    wm_damage_window_internal(w); /* where it is now */
}

static void clamp_to_screen(Window *w)
{
    if (w->maximized) return;
    int W = (int)gfx_width(), H = (int)gfx_height();
    int tb = ui_theme()->titlebar_h;
    int keep = 120; /* keep at least this much title bar visible */
    if (w->r.x > W - keep) w->r.x = W - keep;
    if (w->r.x < keep - w->r.w) w->r.x = keep - w->r.w;
    if (w->r.y < 0) w->r.y = 0;
    if (w->r.y > H - TASKBAR_HEIGHT - tb) w->r.y = H - TASKBAR_HEIGHT - tb;
}

int wm_dragging(void) { return DRAG != 0 || RESIZE != 0; }

void wm_input_forget_window(Window *w)
{
    if (DRAG == w) DRAG = 0;
    if (CLOSE_PRESSED == w) CLOSE_PRESSED = 0;
    if (MIN_PRESSED == w) MIN_PRESSED = 0;
    if (MAX_PRESSED == w) MAX_PRESSED = 0;
    if (LC_WIN == w) LC_WIN = 0;
    if (PRESS_WIN == w) PRESS_WIN = 0;
    if (HOVER_CONTENT == w) HOVER_CONTENT = 0;
    if (RESIZE == w) RESIZE = 0;
    if (HOVER_WIN == w) HOVER_WIN = 0, HOVER_BTN = -1;
    if (SIG_HW == w) SIG_HW = 0;
    if (SIG_CP == w) SIG_CP = 0;
    if (SIG_MP == w) SIG_MP = 0;
    if (SIG_XP == w) SIG_XP = 0;
}

/* ---- mouse --------------------------------------------------- */
int wm_pointer(int x, int y, unsigned buttons)
{
    int down = (buttons & 1) != 0, was_down = (LAST_BUTTONS & 1) != 0;
    int r_edge = (buttons & 2) && !(LAST_BUTTONS & 2); /* right button went down just now */
    LAST_BUTTONS = buttons;

    /* 1) press: focus, start dragging, or arm the close button */
    if (down && !was_down)
    {
        Window *w = wm_window_at_internal(x, y);
        if (w)
        {
            wm_raise(w);
            if (wm_blocker_internal(w))
                w = 0; /* the owner is blocked: the click only brings its dialog to the front */
        }
        if (w)
        {
            int edge = resize_edge(w, x, y);
            if (edge)
            {
                RESIZE = w; /* grab an edge or corner */
                RS_EDGE = edge;
                RS_PX = x;
                RS_PY = y;
                RS_START = w->r;
            }
            else if (close_hit(w, x, y))
                CLOSE_PRESSED = w;
            else if (min_hit(w, x, y))
                MIN_PRESSED = w;
            else if (max_hit(w, x, y))
                MAX_PRESSED = w;
            else if (!in_titlebar(w, x, y))
            {
                if (w->on_press && w->on_press(w, x, y))
                    wm_damage_window_internal(w);
                if (w->on_drag)
                    PRESS_WIN = w;
            }
            if (!RESIZE && !CLOSE_PRESSED && !MIN_PRESSED && !MAX_PRESSED && in_titlebar(w, x, y))
            {
                DRAG = w;
                DRAG_PX = x;
                DRAG_PY = y;
                DRAG_DX = x - w->r.x;
                DRAG_DY = y - w->r.y;
            }
        }
    }

    /* 1a) right press: raise the window and let it show its own menu */
    if (r_edge && !DRAG && !RESIZE)
    {
        Window *w = wm_window_at_internal(x, y);
        if (w)
        {
            wm_raise(w);
            if (!wm_blocker_internal(w) && !in_titlebar(w, x, y) && w->on_rpress && w->on_rpress(w, x, y))
                wm_damage_window_internal(w);
        }
    }

    /* 1b) the button stays down after a press inside a window: text selection by dragging.
     *     Also called once on release, so the window can end the drag. */
    if (PRESS_WIN && wm_z_index_internal(PRESS_WIN) < 0)
        PRESS_WIN = 0;
    if (PRESS_WIN && !(down && !was_down))
    {
        Window *pw = PRESS_WIN;
        if (!down)
            PRESS_WIN = 0;
        if (pw->on_drag && pw->on_drag(pw, x, y, buttons))
            wm_damage_window_internal(pw);
    }

    /* 1c) resize: follow the pointer while the button is held */
    if (RESIZE && wm_z_index_internal(RESIZE) < 0)
        RESIZE = 0;
    if (RESIZE && down)
        apply_resize(x, y);

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
        RESIZE = 0;
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
            if (wm_z_index_internal(MAX_PRESSED) >= 0 && max_hit(MAX_PRESSED, x, y))
                to_max = MAX_PRESSED;
            MAX_PRESSED = 0;
        }
        if (MIN_PRESSED)
        {
            if (wm_z_index_internal(MIN_PRESSED) >= 0 && min_hit(MIN_PRESSED, x, y))
                to_min = MIN_PRESSED;
            MIN_PRESSED = 0;
        }
        if (CLOSE_PRESSED)
        {
            if (wm_z_index_internal(CLOSE_PRESSED) >= 0 && close_hit(CLOSE_PRESSED, x, y))
                to_close = CLOSE_PRESSED;
            CLOSE_PRESSED = 0;
        }
    }

    /* 4) buttons: only the window under the pointer gets real coordinates,
     *    the others get "far away" so their hover/press state is cleared. */
    Window *target = (DRAG || RESIZE) ? 0 : wm_window_at_internal(x, y);
    if (target && wm_blocker_internal(target))
        target = 0;
    if (target && (CLOSE_PRESSED || to_close || MIN_PRESSED || to_min || MAX_PRESSED || to_max || to_toggle))
        target = 0;
    for (int i = 0; i < WM_ZN; i++)
    {
        Window *w = WM_Z[i];
        int hit = (w == target);
        if (ui_buttons_pointer(&w->set, hit ? x : -1, hit ? y : -1, buttons))
            wm_damage_window_internal(w);
        if (WM_ZN == 0 || wm_z_index_internal(w) < 0) /* a click handler closed it */
            break;
    }

    /* 4b) content hover: the window under the pointer, or nobody while dragging / blocked */
    if (HOVER_CONTENT && HOVER_CONTENT != target && wm_z_index_internal(HOVER_CONTENT) >= 0)
    {
        Window *old = HOVER_CONTENT;
        HOVER_CONTENT = 0;
        if (old->on_hover && old->on_hover(old, -1, -1))
            wm_damage_window_internal(old);
    }
    if (target && wm_z_index_internal(target) >= 0)
    {
        HOVER_CONTENT = target;
        if (target->on_hover && target->on_hover(target, x, y))
            wm_damage_window_internal(target);
    }
    else
        HOVER_CONTENT = 0;

    if (to_close)
        wm_close(to_close);
    else if (to_min)
        wm_minimize(to_min);
    else if (to_max)
        wm_set_maximized(to_max, !to_max->maximized);
    else if (to_toggle)
        wm_set_maximized(to_toggle, !to_toggle->maximized);

    update_button_feedback(x, y);
    return WM_HAS_DMG;
}

int wm_wheel(int x, int y, int steps)
{
    if (steps == 0 || DRAG || RESIZE)
        return 0;
    Window *w = wm_window_at_internal(x, y);
    if (!w || wm_blocker_internal(w) || !w->on_wheel)
        return 0;
    if (w->on_wheel(w, steps))
        wm_damage_window_internal(w);
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
