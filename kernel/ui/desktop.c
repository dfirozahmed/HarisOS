/* HarisOS desktop. Everything here is built from HarisUI (include/ui.h), so
 * restyling the whole desktop is a theme change: ui_set_theme(&ui_theme_light). */
#include "desktop.h"
#include "taskbar.h"
#include "ui.h"

static const char *STATUS_LINE;
static unsigned long long MEM_FREE_MB, MEM_TOTAL_MB;
static int DOUBLE_BUFFERED;

void desktop_set_status(const char *line) { STATUS_LINE = line; }

void desktop_set_memory(unsigned long long free_mb, unsigned long long total_mb)
{
    MEM_FREE_MB = free_mb;
    MEM_TOTAL_MB = total_mb;
}

void desktop_set_double_buffered(int on) { DOUBLE_BUFFERED = on; }

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

void desktop_draw(void)
{
    if (!gfx_ready())
        return;
    const UiTheme *t = ui_theme();
    int W = (int)gfx_width(), H = (int)gfx_height();
    int area_h = H - TASKBAR_HEIGHT;

    ui_background();

    /* ---- welcome window ---- */
    int ww = W * 56 / 100, wh = 336;
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
    if (DOUBLE_BUFFERED)
        ui_badge(in.x, in.y + in.h - 22, "Double buffered", t->success);
    else
        ui_badge(in.x, in.y + in.h - 22, "Direct drawing", t->warning);

    if (STATUS_LINE)
    {
        ui_cut_top(&body, t->gap);
        ui_text_in(ui_cut_top(&body, 22), STATUS_LINE, UI_TEXT_SMALL, UI_ALIGN_LEFT);
    }

    /* buttons at the bottom right */
    UiRect btns = ui_cut_bottom(&body, 38);
    ui_button(ui_cut_right(&btns, 130), "Get started", UI_BTN_PRIMARY, UI_STATE_NORMAL);
    ui_cut_right(&btns, t->gap);
    ui_button(ui_cut_right(&btns, 100), "Settings", UI_BTN_SECONDARY, UI_STATE_NORMAL);

    gfx_reset_clip();
    taskbar_draw();
}
