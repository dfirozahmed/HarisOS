/* HarisOS desktop. Everything here is built from HarisUI (include/ui.h), so
 * restyling the whole desktop is a theme change: ui_set_theme(&ui_theme_light). */
#include "desktop.h"
#include "taskbar.h"
#include "ui.h"

static const char *STATUS_LINE;
static unsigned long long MEM_FREE_MB, MEM_TOTAL_MB;
static int DOUBLE_BUFFERED;
static int MOUSE_PRESENT;

/* ---- buttons: each one has an onClick handler (see ui.h) ---------------- */
static void on_get_started(UiButton *b, void *user);
static void on_settings(UiButton *b, void *user);

static UiButton BTN_START = UI_BUTTON("Get started", UI_BTN_PRIMARY, on_get_started, 0);
static UiButton BTN_SETTINGS = UI_BUTTON("Settings", UI_BTN_SECONDARY, on_settings, 0);
static UiButtonSet BUTTONS;
static int BUTTONS_READY;
static UiRect BTN_ROW; /* band behind the buttons, cleared before they are redrawn */
static UiRgb BTN_BG;
static int DARK = 1;
static int FULL_REDRAW; /* set by a handler when the whole desktop must be redrawn */
static int CLICKS;
static char CLICK_MSG[48];

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

static void on_get_started(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    char *o = CLICK_MSG;
    CLICKS++;
    o = put_str(o, "\"Get started\" clicked ");
    o = put_num(o, (unsigned long long)CLICKS);
    o = put_str(o, CLICKS == 1 ? " time" : " times");
    *o = 0;
    STATUS_LINE = CLICK_MSG;
    FULL_REDRAW = 1; /* the status line changes the window layout */
}

/* "Settings": switch between the dark and light theme */
static void on_settings(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    DARK = !DARK;
    ui_set_theme(DARK ? &ui_theme_dark : &ui_theme_light);
    FULL_REDRAW = 1;
}

void desktop_draw(void)
{
    if (!gfx_ready())
        return;
    const UiTheme *t = ui_theme();
    int W = (int)gfx_width(), H = (int)gfx_height();
    int area_h = H - TASKBAR_HEIGHT;

    ui_background();

    /* ---- welcome window ---- */
    int ww = W * 56 / 100, wh = STATUS_LINE ? 372 : 336;
    if (ww > 720)
        ww = 720;
    if (ww < 420)
        ww = W < 420 ? W - 20 : 420;
    if (wh > area_h - 24)
        wh = area_h - 24;
    UiRect win = ui_center(ui_rect(0, 0, W, area_h), ww, wh);
    UiRect body = ui_window(win, "Welcome to HarisOS", 1);
    ui_clip(body); /* nothing can spill out of the window */
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

    /* buttons at the bottom right */
    UiRect btns = ui_cut_bottom(&body, 38);
    BTN_BG = t->surface;
    BTN_START.r = ui_cut_right(&btns, 130);
    ui_cut_right(&btns, t->gap);
    BTN_SETTINGS.r = ui_cut_right(&btns, 100);
    if (!BUTTONS_READY)
    {
        ui_buttons_init(&BUTTONS);
        ui_buttons_add(&BUTTONS, &BTN_SETTINGS);
        ui_buttons_add(&BUTTONS, &BTN_START);
        BUTTONS_READY = 1;
    }
    /* the area behind the buttons (incl. their shadow); stays inside the window's straight edges */
    BTN_ROW = ui_rect(BTN_SETTINGS.r.x - 10, BTN_SETTINGS.r.y - 10,
                      BTN_START.r.x + BTN_START.r.w - BTN_SETTINGS.r.x + 20, BTN_START.r.h + 24);
    ui_buttons_draw(&BUTTONS);

    gfx_reset_clip();
    taskbar_draw();
}

int desktop_pointer(int x, int y, unsigned buttons)
{
    if (!BUTTONS_READY)
        return 0;

    /* hover/press/click events are dispatched here; handlers run inside */
    int changed = ui_buttons_pointer(&BUTTONS, x, y, buttons);

    if (FULL_REDRAW)
    {
        FULL_REDRAW = 0;
        gfx_cursor_show(0); /* keeps the pointer correct even without a back buffer */
        desktop_draw();
        gfx_present();
        gfx_cursor_show(1);
        return 1;
    }
    if (changed)
    {
        gfx_cursor_show(0);
        gfx_fill_rect(BTN_ROW.x, BTN_ROW.y, BTN_ROW.w, BTN_ROW.h, ui_color(BTN_BG));
        ui_buttons_draw(&BUTTONS);
        gfx_present();
        gfx_cursor_show(1);
        return 1;
    }
    return 0;
}
