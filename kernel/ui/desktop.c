/* HarisOS desktop. Everything here is built from HarisUI (include/ui.h), so
 * restyling the whole desktop is a theme change: ui_set_theme(&ui_theme_light). */
#include "desktop.h"
#include "taskbar.h"
#include "ui.h"
#include "wm.h"

static const char *STATUS_LINE;
static unsigned long long MEM_FREE_MB, MEM_TOTAL_MB;
static int DOUBLE_BUFFERED;
static int MOUSE_PRESENT;

static Window *WELCOME; /* the welcome window (NULL once the user closes it) */
static int DARK = 1;

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
static void hello_draw(Window *w, UiRect body)
{
    const UiTheme *t = ui_theme();
    body = ui_inset(body, t->pad);
    char buf[48], *o = buf;
    o = put_str(o, "Window #");
    o = put_num(o, (unsigned long long)w->id);
    *o = 0;
    ui_text(body.x, body.y, buf, UI_TEXT_HEADING);
    ui_text(body.x, body.y + 34, "Drag the title bar to move me.", UI_TEXT_DIM);
    ui_text(body.x, body.y + 54, "Click a window to bring it to the front.", UI_TEXT_DIM);
    o = buf;
    o = put_num(o, (unsigned long long)wm_count());
    o = put_str(o, " window(s) open");
    *o = 0;
    ui_text(body.x, body.y + 80, buf, UI_TEXT_SMALL);
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
    int w = 340, h = 200;
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
    if (!win)
    {
        STATUS_LINE = "Window limit reached - close one first.";
        wm_invalidate();
        return;
    }
    STATUS_LINE = 0;
    int tb = ui_theme()->titlebar_h;
    int cw = w - 2, ch = h - tb - 2;
    wm_add_button(win, "Close", UI_BTN_DANGER, ui_rect(cw - 20 - 100, ch - 20 - 34, 100, 34),
                  on_hello_close, win);
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
    wm_invalidate();
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

    /* buttons at the bottom right of the content area */
    int cw = ww - 2, ch = wh - t->titlebar_h - 2;
    int by = ch - t->pad - 38;
    UiRect start = ui_rect(cw - t->pad - 130, by, 130, 38);
    UiRect sett = ui_rect(start.x - t->gap - 100, by, 100, 38);
    wm_add_button(WELCOME, "Settings", UI_BTN_SECONDARY, sett, on_settings, 0);
    wm_add_button(WELCOME, "Get started", UI_BTN_PRIMARY, start, on_get_started, 0);
}

static int FIRST = 1;

void desktop_draw(void)
{
    if (!gfx_ready())
        return;
    if (FIRST)
    {
        FIRST = 0;
        welcome_create();
    }
    ui_background();
    wm_draw();
    gfx_reset_clip();
    taskbar_draw();
}

int desktop_pointer(int x, int y, unsigned buttons)
{
    /* focus, dragging, close buttons and button events are handled by the window manager */
    if (!wm_pointer(x, y, buttons))
        return 0;
    gfx_cursor_show(0); /* keeps the pointer correct even without a back buffer */
    desktop_draw();
    gfx_present();
    gfx_cursor_show(1);
    return 1;
}
