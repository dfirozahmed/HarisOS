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
static unsigned long long LC_T;
static int LC_X, LC_Y;
static int DRAG_PX, DRAG_PY;
static unsigned LAST_BUTTONS;

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

int wm_dragging(void) { return DRAG != 0; }

void wm_input_forget_window(Window *w)
{
    if (DRAG == w) DRAG = 0;
    if (CLOSE_PRESSED == w) CLOSE_PRESSED = 0;
    if (MIN_PRESSED == w) MIN_PRESSED = 0;
    if (MAX_PRESSED == w) MAX_PRESSED = 0;
    if (LC_WIN == w) LC_WIN = 0;
}

/* ---- mouse --------------------------------------------------- */
int wm_pointer(int x, int y, unsigned buttons)
{
    int down = (buttons & 1) != 0, was_down = (LAST_BUTTONS & 1) != 0;
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
    Window *target = DRAG ? 0 : wm_window_at_internal(x, y);
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
