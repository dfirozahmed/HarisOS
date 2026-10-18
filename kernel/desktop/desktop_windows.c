/* HarisOS desktop windows: welcome screen and sample Hello/message dialogs. */
#include "desktop.h"
#include "taskbar.h"
#include "ui.h"
#include "wm.h"
#include "rtc.h"
#include "desktop_internal.h"

static char *put_str(char *o, const char *s) { return desktop_put_str(o, s); }
static void caption(UiRect r, const char *s)
{
    ui_text(r.x, r.y, s, UI_TEXT_SMALL);
}

static char *put_num(char *o, unsigned long long v) { return desktop_put_num(o, v); }

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
    ui_edit_select_all(&w->edits[w->focus]); /* arriving with Tab / arrows selects the box's text, like a normal form */
    ui_caret_restart();
}

/* clicking a box gives it the keyboard and puts the caret where you clicked
 * (double-click selects a word, triple-click everything) */
static int hello_press(Window *w, int x, int y)
{
    for (int i = 0; i < w->edit_count; i++)
    {
        UiRect f = w->edits[i].field;
        if (f.w > 0 && x >= f.x && x < f.x + f.w && y >= f.y && y < f.y + f.h)
        {
            if (i != w->focus)
                hello_set_focus(w, i);
            ui_edit_mouse(&w->edits[i], x, y, 1);
            ui_caret_restart();
            return 1;
        }
    }
    return 0;
}

/* holding the button after the press selects text; the release ends it */
static int hello_drag(Window *w, int x, int y, unsigned buttons)
{
    if (!w->edits[w->focus].dragging)
        return 0; /* the press was not on a box: nothing to select */
    int changed = ui_edit_mouse(&w->edits[w->focus], x, y, buttons);
    if (changed)
        ui_caret_restart();
    return changed;
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

static void show_message(Window *src)
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
    Window *win = m ? wm_create_dialog((Window *)src, "Message", x, y, w, h, msg_draw, m) : 0;
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

void open_hello_window(void)
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
        win->on_drag = hello_drag;
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

    /* Keep the welcome content readable on narrow or short GOP modes. The
     * window manager clips to the client area, but a full two-card layout
     * would otherwise force its labels and buttons to overlap. */
    if (body.w < 340 || body.h < 250)
    {
        ui_text(body.x, body.y, "HarisOS is ready", UI_TEXT_HEADING);
        if (body.h >= 76)
        {
            char compact[48], *o = compact;
            o = put_str(o, "Display: ");
            o = put_num(o, (unsigned long long)W);
            o = put_str(o, " x ");
            o = put_num(o, (unsigned long long)H);
            *o = 0;
            ui_text(body.x, body.y + 26, compact, UI_TEXT_BODY);
        }
        if (STATUS_LINE && body.h >= 104)
            ui_text_in(ui_rect(body.x, body.y + 52, body.w, 22), STATUS_LINE,
                       UI_TEXT_SMALL, UI_ALIGN_LEFT);
        return;
    }

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

void welcome_create(void)
{
    const UiTheme *t = ui_theme();
    int W = (int)gfx_width(), H = (int)gfx_height();
    if (W <= 0 || H <= TASKBAR_HEIGHT + 64)
        return;

    int area_h = H - TASKBAR_HEIGHT;
    int max_w = W - 20;
    if (max_w < 1)
        max_w = W;
    int max_h = area_h - 8;
    if (max_h < 1)
        max_h = area_h;

    int ww = W * 56 / 100, wh = 372;
    if (ww > 720)
        ww = 720;
    if (ww < 420)
        ww = W < 420 ? max_w : 420;
    if (ww > max_w)
        ww = max_w;
    if (wh > max_h)
        wh = max_h;
    if (ww < 1 || wh < 64)
        return;

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

