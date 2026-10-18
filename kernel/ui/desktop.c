/* HarisOS desktop. Everything here is built from HarisUI (include/ui.h), so
 * restyling the whole desktop is a theme change: ui_set_theme(&ui_theme_light). */
#include "desktop.h"
#include "taskbar.h"
#include "ui.h"
#include "timer.h"
#include "wm.h"

static const char *STATUS_LINE;
static unsigned long long MEM_FREE_MB, MEM_TOTAL_MB;
static int DOUBLE_BUFFERED;
static int MOUSE_PRESENT;

static Window *WELCOME; /* the welcome window (NULL once the user closes it) */
static int DARK = 1;
static int WALL_VALID; /* 1 = the wallpaper is in gfx's cache and matches the current theme */

void desktop_set_status(const char *line) { STATUS_LINE = line; }

void desktop_set_memory(unsigned long long free_mb, unsigned long long total_mb)
{
    MEM_FREE_MB = free_mb;
    MEM_TOTAL_MB = total_mb;
}

void desktop_set_double_buffered(int on) { DOUBLE_BUFFERED = on; }
void desktop_set_mouse(int present) { MOUSE_PRESENT = present; }

static char *put_str(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;
    return o;
}

static char *put_num(char *o, unsigned long long v)
{
    char tmp[24];
    int n = 0;
    do
    {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *o++ = tmp[--n];
    return o;
}

/* "<label>" small caption above a value */
static void caption(UiRect r, const char *s)
{
    ui_text(r.x, r.y, s, UI_TEXT_SMALL);
}

/* ---- the "Hello" windows opened by "Get started" ---- */
static const char *const HELLO_LABEL[WM_MAX_EDITS] = {"Name", "Email", "Message"};
#define HELLO_FIELD_Y 78 /* first field, relative to the body */
#define HELLO_FIELD_STEP 58

static void hello_draw(Window *w, UiRect body)
{
    const UiTheme *t = ui_theme();
    body = ui_inset(body, t->pad);
    char buf[48], *o = buf;
    o = put_str(o, "Window #");
    o = put_num(o, (unsigned long long)w->id);
    *o = 0;
    ui_text(body.x, body.y, buf, UI_TEXT_HEADING);
    ui_text(body.x, body.y + 34, "Click a box or press Tab to switch.", UI_TEXT_DIM);
    /* the window's own text inputs; only the focused one of the top window has the keyboard */
    for (int i = 0; i < w->edit_count; i++)
    {
        int y = body.y + HELLO_FIELD_Y + i * HELLO_FIELD_STEP;
        ui_text(body.x, y, HELLO_LABEL[i], UI_TEXT_DIM);
        ui_edit_draw(ui_rect(body.x, y + 20, body.w, 34), &w->edits[i], "Type here...",
                     w == wm_top() && i == w->focus);
    }
}

static void hello_set_focus(Window *w, int i)
{
    w->focus = (i % w->edit_count + w->edit_count) % w->edit_count;
    ui_caret_restart();
}

/* clicking a box gives it the keyboard */
static int hello_press(Window *w, int x, int y)
{
    for (int i = 0; i < w->edit_count; i++)
    {
        UiRect f = w->edits[i].field;
        if (f.w > 0 && x >= f.x && x < f.x + f.w && y >= f.y && y < f.y + f.h)
        {
            if (i == w->focus)
                return 0;
            hello_set_focus(w, i);
            return 1;
        }
    }
    return 0;
}

/* keyboard: typing goes to the focused box; Tab / Shift+Tab / Up / Down move between boxes;
 * Escape clears the focused box */
static void hello_key(Window *w, const KeyEvent *k)
{
    UiEdit *e = &w->edits[w->focus];
    if (k->pressed && k->key == KEY_TAB)
        hello_set_focus(w, w->focus + ((k->mods & KMOD_SHIFT) ? -1 : 1));
    else if (k->pressed && k->key == KEY_DOWN)
        hello_set_focus(w, w->focus + 1);
    else if (k->pressed && k->key == KEY_UP)
        hello_set_focus(w, w->focus - 1);
    else if (k->pressed && k->key == KEY_ESC)
        ui_edit_clear(e);
    else
        ui_edit_key(e, k);
}

/* ---- message box: a modal dialog showing what was typed in a Hello window ---- */
#define MSG_LINE (UI_EDIT_MAX + 12)
typedef struct
{
    int used;
    char line[WM_MAX_EDITS][MSG_LINE];
} MsgData;
static MsgData MSGS[WM_MAX_WINDOWS];

static void msg_close(Window *w)
{
    if (w->user)
        ((MsgData *)w->user)->used = 0;
}

static void msg_draw(Window *w, UiRect body)
{
    const UiTheme *t = ui_theme();
    MsgData *m = (MsgData *)w->user;
    body = ui_inset(body, t->pad);
    ui_text(body.x, body.y, "You typed", UI_TEXT_HEADING);
    for (int i = 0; m && i < WM_MAX_EDITS; i++)
        ui_text_in(ui_rect(body.x, body.y + 40 + i * 30, body.w, 26), m->line[i], UI_TEXT_BODY, UI_ALIGN_LEFT);
}

static void msg_key(Window *w, const KeyEvent *k)
{
    if (k->pressed && (k->key == KEY_ENTER || k->key == KEY_ESC))
        wm_close(w);
}

static void on_msg_ok(UiButton *b, void *user)
{
    (void)b;
    wm_close((Window *)user);
}

static void show_message(const Window *src)
{
    MsgData *m = 0;
    for (int i = 0; i < WM_MAX_WINDOWS && !m; i++)
        if (!MSGS[i].used)
            m = &MSGS[i];
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 340, h = 210;
    int x = src->r.x + (src->r.w - w) / 2, y = src->r.y + (src->r.h - h) / 2; /* centered over its window */
    if (x + w > W)
        x = W - w;
    if (y + h > H - TASKBAR_HEIGHT)
        y = H - TASKBAR_HEIGHT - h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    Window *win = m ? wm_create_dialog("Message", x, y, w, h, msg_draw, m) : 0;
    if (!win)
    {
        STATUS_LINE = "Window limit reached - close one first.";
        wm_invalidate();
        return;
    }
    m->used = 1;
    for (int i = 0; i < WM_MAX_EDITS; i++)
    {
        char *o = put_str(m->line[i], HELLO_LABEL[i]);
        o = put_str(o, ": ");
        const UiEdit *e = &src->edits[i];
        if (e->len == 0)
            o = put_str(o, "(empty)");
        else
        {
            for (int j = 0; j < e->len; j++)
                *o++ = e->text[j];
        }
        *o = 0;
    }
    win->on_close = msg_close;
    win->on_key = msg_key;
    STATUS_LINE = 0;
    int tb = ui_theme()->titlebar_h;
    int cw = w - 2, ch = h - tb - 2;
    wm_add_button_ex(win, "OK", UI_BTN_PRIMARY, ui_rect(cw - 20 - 100, ch - 20 - 34, 100, 34),
                     WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_msg_ok, win);
}

static void on_hello_show(UiButton *b, void *user)
{
    (void)b;
    show_message((Window *)user);
}

static void on_hello_close(UiButton *b, void *user)
{
    (void)b;
    wm_close((Window *)user);
}

static void open_hello_window(void)
{
    int W = (int)gfx_width(), H = (int)gfx_height();
    int k = wm_count();
    int w = 340, h = 360;
    int x = 60 + 32 * (k % 8), y = 40 + 32 * (k % 8);
    if (x + w > W)
        x = W - w;
    if (y + h > H - TASKBAR_HEIGHT)
        y = H - TASKBAR_HEIGHT - h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;

    Window *win = wm_create("Hello", x, y, w, h, hello_draw, 0);
    if (win)
    {
        win->on_key = hello_key;
        win->on_press = hello_press;
        win->edit_count = 3;
        win->caret = 1; /* its text field has a blinking caret */
    }
    if (!win)
    {
        STATUS_LINE = "Window limit reached - close one first.";
        wm_invalidate();
        return;
    }
    win->icon = TASKBAR_ICON_TERMINAL;
    STATUS_LINE = 0;
    int tb = ui_theme()->titlebar_h;
    int cw = w - 2, ch = h - tb - 2;
    wm_add_button_ex(win, "Close", UI_BTN_DANGER, ui_rect(cw - 20 - 100, ch - 20 - 34, 100, 34),
                     WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_hello_close, win);
    wm_add_button_ex(win, "Show", UI_BTN_PRIMARY, ui_rect(cw - 20 - 100 - 10 - 100, ch - 20 - 34, 100, 34),
                     WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_hello_show, win);
}

/* ---- the welcome window ---- */
static void on_get_started(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    open_hello_window();
}

/* "Settings": switch between the dark and light theme */
static void on_settings(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    DARK = !DARK;
    ui_set_theme(DARK ? &ui_theme_dark : &ui_theme_light);
    WALL_VALID = 0; /* the wallpaper colors changed: draw it again */
    wm_invalidate();
}

/* keyboard: Enter does the same as "Get started" */
static void welcome_key(Window *w, const KeyEvent *k)
{
    (void)w;
    if (k->pressed && k->key == KEY_ENTER)
        open_hello_window();
}

static void on_welcome_close(Window *w)
{
    (void)w;
    WELCOME = 0;
}

static void welcome_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    int W = (int)gfx_width(), H = (int)gfx_height();
    body = ui_inset(body, t->pad);

    /* header: icon + heading + subtitle */
    UiRect head = ui_cut_top(&body, 56);
    ui_icon(UI_ICON_LOGO, head.x + 24, head.y + 28, 48, t->text);
    ui_text(head.x + 64, head.y + 4, "HarisOS", UI_TEXT_HEADING);
    ui_text(head.x + 64, head.y + 32, "Your smooth UEFI desktop is ready.", UI_TEXT_DIM);
    ui_cut_top(&body, t->gap + 4);
    ui_separator(body.x, body.y, body.w);
    ui_cut_top(&body, t->gap + 8);

    /* two info cards side by side */
    int card_h = 96;
    UiRect row = ui_cut_top(&body, card_h);
    int cw = (row.w - t->gap) / 2;
    UiRect c1 = ui_cut_left(&row, cw);
    ui_cut_left(&row, t->gap);
    UiRect c2 = row;

    /* memory card */
    ui_card(c1);
    UiRect in = ui_inset(c1, 14);
    caption(in, "MEMORY");
    char buf[64], *o = buf;
    int percent = 0;
    if (MEM_TOTAL_MB)
    {
        unsigned long long used = MEM_TOTAL_MB - MEM_FREE_MB;
        percent = (int)(used * 100 / MEM_TOTAL_MB);
        o = put_num(o, MEM_FREE_MB);
        o = put_str(o, " MB free of ");
        o = put_num(o, MEM_TOTAL_MB);
        o = put_str(o, " MB");
    }
    else
        o = put_str(o, "Memory manager failed");
    *o = 0;
    ui_text_in(ui_rect(in.x, in.y + 18, in.w, 24), buf, UI_TEXT_LABEL, UI_ALIGN_LEFT);
    ui_progress(ui_rect(in.x, in.y + in.h - 14, in.w, 8), percent);

    /* display card */
    ui_card(c2);
    in = ui_inset(c2, 14);
    caption(in, "DISPLAY");
    o = buf;
    o = put_num(o, (unsigned long long)W);
    o = put_str(o, " x ");
    o = put_num(o, (unsigned long long)H);
    *o = 0;
    ui_text_in(ui_rect(in.x, in.y + 18, in.w, 24), buf, UI_TEXT_LABEL, UI_ALIGN_LEFT);
    int bx = in.x;
    if (DOUBLE_BUFFERED)
        bx += ui_badge(bx, in.y + in.h - 22, "Double buffered", t->success) + 6;
    else
        bx += ui_badge(bx, in.y + in.h - 22, "Direct drawing", t->warning) + 6;
    if (MOUSE_PRESENT)
        ui_badge(bx, in.y + in.h - 22, "Mouse", t->accent_hi);
    else
        ui_badge(bx, in.y + in.h - 22, "No mouse", t->text_dim);

    if (STATUS_LINE)
    {
        ui_cut_top(&body, t->gap);
        ui_text_in(ui_cut_top(&body, 22), STATUS_LINE, UI_TEXT_SMALL, UI_ALIGN_LEFT);
    }
}

static void welcome_create(void)
{
    const UiTheme *t = ui_theme();
    int W = (int)gfx_width(), H = (int)gfx_height();
    int area_h = H - TASKBAR_HEIGHT;
    int ww = W * 56 / 100, wh = 372;
    if (ww > 720)
        ww = 720;
    if (ww < 420)
        ww = W < 420 ? W - 20 : 420;
    if (wh > area_h - 24)
        wh = area_h - 24;
    UiRect win = ui_center(ui_rect(0, 0, W, area_h), ww, wh);

    WELCOME = wm_create("Welcome to HarisOS", win.x, win.y, win.w, win.h, welcome_draw, 0);
    if (!WELCOME)
        return;
    WELCOME->on_close = on_welcome_close;
    WELCOME->on_key = welcome_key;
    WELCOME->icon = TASKBAR_ICON_LOGO;

    /* buttons at the bottom right of the content area */
    int cw = ww - 2, ch = wh - t->titlebar_h - 2;
    int by = ch - t->pad - 38;
    UiRect start = ui_rect(cw - t->pad - 130, by, 130, 38);
    UiRect sett = ui_rect(start.x - t->gap - 100, by, 100, 38);
    int corner = WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM; /* stay in the corner when maximized */
    wm_add_button_ex(WELCOME, "Settings", UI_BTN_SECONDARY, sett, corner, on_settings, 0);
    wm_add_button_ex(WELCOME, "Get started", UI_BTN_PRIMARY, start, corner, on_get_started, 0);
}

/* ---- taskbar: one button per open window ---------------------------- */
static Window *TB_WIN[TASKBAR_MAX_APPS];
static int TB_N;
static unsigned TB_SIG[TASKBAR_MAX_APPS]; /* what the taskbar shows now, to spot changes */

/* Rebuild the taskbar's app list from the open windows.
 * Returns 1 if what the taskbar shows has changed (it must be redrawn). */
static int refresh_taskbar(void)
{
    Window *list[TASKBAR_MAX_APPS];
    TaskbarApp apps[TASKBAR_MAX_APPS];
    unsigned sig[TASKBAR_MAX_APPS];
    int n = wm_list(list, TASKBAR_MAX_APPS);
    Window *top = wm_top();
    for (int i = 0; i < n; i++)
    {
        apps[i].icon = (TaskbarIcon)list[i]->icon;
        apps[i].running = 1;
        apps[i].active = (list[i] == top);
        apps[i].minimized = list[i]->minimized;
        sig[i] = (unsigned)list[i]->id * 8u + (unsigned)apps[i].icon + (apps[i].active ? 64u : 0u) +
                 (apps[i].minimized ? 128u : 0u);
    }
    int changed = (n != TB_N);
    for (int i = 0; i < n && !changed; i++)
        if (sig[i] != TB_SIG[i])
            changed = 1;
    for (int i = 0; i < n; i++)
    {
        TB_WIN[i] = list[i];
        TB_SIG[i] = sig[i];
    }
    TB_N = n;
    taskbar_set_apps(apps, n);
    return changed;
}

/* Start button: open the welcome window again (or bring it to the front). */
static void on_taskbar_start(void)
{
    if (!WELCOME)
        welcome_create();
    else
        wm_activate(WELCOME);
}

/* App button: restore a minimized window, minimize the focused one, or focus another. */
static void on_taskbar_app(int i)
{
    if (i < 0 || i >= TB_N || !wm_is_open(TB_WIN[i]))
        return;
    Window *w = TB_WIN[i];
    if (w->minimized)
        wm_activate(w);
    else if (w == wm_top())
        wm_minimize(w);
    else
        wm_raise(w);
}

static int FIRST = 1;

/* Full redraw: wallpaper, windows, taskbar. The finished wallpaper is saved in
 * gfx's cache (if there is one) so later redraws can be partial. */
void desktop_draw(void)
{
    if (!gfx_ready())
        return;
    if (FIRST)
    {
        FIRST = 0;
        taskbar_set_callbacks(on_taskbar_start, on_taskbar_app);
        welcome_create();
    }
    gfx_reset_clip();
    ui_background();
    gfx_cache_save(); /* does nothing without a cache */
    WALL_VALID = gfx_cache_valid();
    wm_draw();
    gfx_reset_clip();
    refresh_taskbar();
    taskbar_draw();
}

/* Partial redraw of one rectangle: put the saved wallpaper back, then draw the
 * windows and taskbar on top, all clipped to that rectangle. */
static void desktop_draw_region(UiRect r)
{
    gfx_set_clip(r.x, r.y, r.w, r.h);
    gfx_cache_restore(r.x, r.y, r.w, r.h);
    wm_draw();
    refresh_taskbar();
    taskbar_draw();
    gfx_reset_clip();
}

/* After any input: refresh the taskbar, then redraw whatever changed. Returns 1 if something was drawn. */
static int finish_input(int bar_changed)
{
    int bar_dirty = refresh_taskbar(); /* a window may have opened/closed/changed focus */
    bar_dirty |= bar_changed;
    if (bar_dirty)
    {
        int bx, by, bw, bh;
        taskbar_bounds(&bx, &by, &bw, &bh);
        wm_add_damage(ui_rect(bx, by, bw, bh));
    }

    UiRect area;
    if (!wm_take_damage(&area))
        return 0;

    /* With a back buffer the scene is drawn in RAM and gfx_present() handles the
     * pointer, so it stays visible the whole time (no flicker). Without one we
     * draw straight to the screen and must hide the pointer meanwhile. */
    int direct = !gfx_double_buffered();
    if (direct)
        gfx_cursor_show(0);
    if (WALL_VALID && gfx_cache_valid())
        desktop_draw_region(area); /* fast: only what changed */
    else
        desktop_draw(); /* first frame, new theme, or no cache */
    gfx_present();
    if (direct)
        gfx_cursor_show(1);
    return 1;
}

int desktop_pointer(int x, int y, unsigned buttons)
{
    int H = (int)gfx_height();
    int on_bar = y >= H - TASKBAR_HEIGHT;

    /* The taskbar is on top of the windows: while the pointer is over it (and no
     * window is being dragged) the windows must not see the mouse. */
    int to_windows = !(on_bar && !wm_dragging());
    wm_pointer(to_windows ? x : -1, to_windows ? y : -1, buttons);

    /* The taskbar only sees the mouse when no window is being dragged. */
    int bar_changed = refresh_taskbar(); /* its buttons must match the windows before a click is handled */
    bar_changed |= taskbar_pointer(wm_dragging() ? -1 : x, wm_dragging() ? -1 : y, buttons);
    return finish_input(bar_changed);
}

/* Alt+Tab: focus the next window (a minimized one is restored). shift = go backwards. */
static void cycle_windows(int backwards)
{
    Window *list[TASKBAR_MAX_APPS];
    int n = wm_list(list, TASKBAR_MAX_APPS);
    if (n == 0)
        return;
    Window *top = wm_top();
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (list[i] == top)
            cur = i;
    int next = cur < 0 ? 0 : (backwards ? (cur + n - 1) % n : (cur + 1) % n);
    wm_activate(list[next]);
}

int desktop_key(const KeyEvent *k)
{
    ui_caret_restart(); /* typing: the caret is shown and its blink starts over */
    if (k->pressed)
    {
        if ((k->mods & KMOD_ALT) && k->key == KEY_F4)
            wm_close(wm_top()); /* Alt+F4: close the focused window */
        else if ((k->mods & KMOD_ALT) && k->key == KEY_TAB)
            cycle_windows((k->mods & KMOD_SHIFT) != 0);
        else if (k->key == KEY_F11)
        {
            Window *w = wm_top(); /* F11: maximize / restore */
            if (w)
                wm_set_maximized(w, !w->maximized);
        }
        else
            wm_key(k);
    }
    else
        wm_key(k);
    return finish_input(0);
}

/* Called on every main-loop pass: makes the text caret blink. Only the small text
 * field is redrawn, and only when the caret really flips (about twice a second). */
int desktop_tick(void)
{
    int flipped = ui_caret_update();
    Window *top = wm_top();
    if (!flipped || !top || !top->caret || top->edit_count == 0)
        return 0;
    UiRect f = top->edits[top->focus].field;
    if (f.w == 0)
        return 0;
    wm_add_damage(f);
    return finish_input(0);
}
