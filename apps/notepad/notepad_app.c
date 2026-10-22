/* Notepad app, step 1: a plain-text editor that lives in memory (no files yet).
 * Typing, Enter, Tab (4 spaces), Backspace / Delete, arrows, Home / End, PageUp / PageDown, mouse click
 * to place the caret, mouse wheel to scroll. The text is one flat buffer; a line ends at '\n'.
 * An app uses only public headers (ui.h, wm.h, desktop.h, gfx.h) - never desktop_internal.h. */
#include "app.h"
#include "desktop.h"
#include "gfx.h"
#include "taskbar.h"
#include "ui.h"
#include "wm.h"

#define NP_MAX 16384 /* bytes of text */
#define NP_PAD 8
#define NP_STATUS_H 22
#define NP_LINE_MAX 511 /* characters of one line that are measured / drawn */
#define NP_TAB 4

static Window *NP_WIN;
static char NP_BUF[NP_MAX];
static int NP_LEN;
static int NP_CUR;     /* caret: 0..NP_LEN */
static int NP_DESIRED; /* column the Up / Down keys try to keep */
static int NP_TOP;     /* first visible line */
static int NP_HSCROLL; /* pixels the text is scrolled to the left */
static int NP_ROWS = 1; /* visible rows at the last draw (for PageUp / PageDown) */
static int NP_FOLLOW;  /* 1 = scroll so that the caret is visible at the next draw */
static int NP_BLINK = 1;
static UiRect NP_CARET; /* where the caret was drawn (screen), w = 0 if it is scrolled out of view */

/* ---- text buffer ---- */

static int line_start(int p)
{
    while (p > 0 && NP_BUF[p - 1] != '\n')
        p--;
    return p;
}

static int line_end(int p)
{
    while (p < NP_LEN && NP_BUF[p] != '\n')
        p++;
    return p;
}

static int line_count(void)
{
    int n = 1;
    for (int i = 0; i < NP_LEN; i++)
        if (NP_BUF[i] == '\n')
            n++;
    return n;
}

/* buffer offset where line number `line` (0-based) starts; past the last line it gives the last one */
static int line_offset(int line)
{
    int p = 0;
    while (line > 0)
    {
        int e = line_end(p);
        if (e >= NP_LEN)
            break;
        p = e + 1;
        line--;
    }
    return p;
}

/* 0-based line number of buffer offset p */
static int line_of(int p)
{
    int n = 0;
    for (int i = 0; i < p; i++)
        if (NP_BUF[i] == '\n')
            n++;
    return n;
}

/* copy up to `count` characters from `from` into out (cut at NP_LINE_MAX, 0-terminated) */
static void copy_text(int from, int count, char *out)
{
    if (count > NP_LINE_MAX)
        count = NP_LINE_MAX;
    for (int i = 0; i < count; i++)
        out[i] = NP_BUF[from + i];
    out[count] = 0;
}

static int insert_char(char c)
{
    if (NP_LEN >= NP_MAX)
        return 0;
    for (int i = NP_LEN; i > NP_CUR; i--)
        NP_BUF[i] = NP_BUF[i - 1];
    NP_BUF[NP_CUR++] = c;
    NP_LEN++;
    return 1;
}

/* delete the character at offset p */
static void delete_at(int p)
{
    for (int i = p; i < NP_LEN - 1; i++)
        NP_BUF[i] = NP_BUF[i + 1];
    NP_LEN--;
}

/* ---- layout ---- */

static const Font *np_font(void)
{
    return ui_font(UI_TEXT_BODY);
}

static int np_line_h(void)
{
    return ui_text_height(UI_TEXT_BODY) + 4;
}

static UiRect text_area(UiRect body)
{
    return ui_rect(body.x + NP_PAD, body.y + NP_PAD, body.w - 2 * NP_PAD,
                   body.h - NP_PAD - NP_STATUS_H - 6 - NP_PAD / 2);
}

static UiRect status_rect(UiRect body)
{
    return ui_rect(body.x + NP_PAD, body.y + body.h - NP_STATUS_H - 4, body.w - 2 * NP_PAD, NP_STATUS_H);
}

/* x position of the caret inside a line, in pixels from the line's left edge */
static int caret_x(void)
{
    char tmp[NP_LINE_MAX + 1];
    int ls = line_start(NP_CUR);
    copy_text(ls, NP_CUR - ls, tmp);
    return gfx_text_w(np_font(), tmp);
}

/* scroll so that the caret is visible (when NP_FOLLOW is set); `in` is the text area inside its padding */
static void follow(UiRect in)
{
    if (!NP_FOLLOW)
        return;
    NP_ROWS = in.h / np_line_h();
    if (NP_ROWS < 1)
        NP_ROWS = 1;
    int cur_line = line_of(NP_CUR);
    if (cur_line < NP_TOP)
        NP_TOP = cur_line;
    if (cur_line >= NP_TOP + NP_ROWS)
        NP_TOP = cur_line - NP_ROWS + 1;
    int cx = caret_x();
    if (cx < NP_HSCROLL)
        NP_HSCROLL = cx;
    if (cx + 2 > NP_HSCROLL + in.w)
        NP_HSCROLL = cx + 2 - in.w;
    NP_FOLLOW = 0;
}

/* ---- drawing ---- */

static void put_dec(char *s, int *n, unsigned v)
{
    char tmp[10];
    int k = 0;
    do
    {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k)
        s[(*n)++] = tmp[--k];
    s[*n] = 0;
}

static void put_str(char *s, int *n, const char *t)
{
    while (*t)
        s[(*n)++] = *t++;
    s[*n] = 0;
}

static void np_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    const Font *f = np_font();
    int lh = np_line_h();
    UiRect ta = text_area(body);

    gfx_fill_round_rect(ta.x, ta.y, ta.w, ta.h, t->radius_s, ui_color(t->surface_alt));
    gfx_round_rect(ta.x, ta.y, ta.w, ta.h, t->radius_s, 1, ui_color(t->border));

    UiRect in = ui_inset(ta, NP_PAD);
    NP_ROWS = in.h / lh;
    if (NP_ROWS < 1)
        NP_ROWS = 1;

    int total = line_count();
    int cur_line = line_of(NP_CUR);
    follow(in);
    if (NP_TOP > total - 1)
        NP_TOP = total - 1;
    if (NP_TOP < 0)
        NP_TOP = 0;

    GfxClip saved = gfx_get_clip();
    gfx_clip_intersect(in.x, in.y, in.w, in.h); /* not ui_clip(): that would drop the window manager's clip */
    GfxClip cl = gfx_get_clip(); /* a partial redraw (caret blink) only needs the rows it touches */
    GfxColor ink = ui_color(t->text);
    int p = line_offset(NP_TOP);
    NP_CARET = ui_rect(0, 0, 0, 0);
    for (int row = 0; row < NP_ROWS + 1 && p <= NP_LEN; row++)
    {
        int e = line_end(p);
        int y = in.y + row * lh + 2;
        int is_cur = NP_TOP + row == cur_line;
        if (is_cur)
            NP_CARET = ui_rect(in.x - NP_HSCROLL + caret_x(), y, 2, lh - 4);
        if (y + lh >= cl.y0 && y <= cl.y1)
        {
            char tmp[NP_LINE_MAX + 1];
            copy_text(p, e - p, tmp);
            gfx_text(f, in.x - NP_HSCROLL, y, tmp, ink);
            if (is_cur && NP_BLINK)
                gfx_fill_rect(NP_CARET.x, NP_CARET.y, NP_CARET.w, NP_CARET.h, ui_color(t->accent));
        }
        p = e + 1;
    }
    gfx_restore_clip(saved);

    /* status bar: line, column and how full the buffer is */
    char st[64];
    int n = 0;
    put_str(st, &n, "Ln ");
    put_dec(st, &n, (unsigned)cur_line + 1);
    put_str(st, &n, ", Col ");
    put_dec(st, &n, (unsigned)(NP_CUR - line_start(NP_CUR)) + 1);
    put_str(st, &n, "     ");
    put_dec(st, &n, (unsigned)NP_LEN);
    put_str(st, &n, " / ");
    put_dec(st, &n, NP_MAX);
    ui_text_in_color(status_rect(body), st, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
}

/* ---- input ---- */

/* put the caret on line `line` at column `col` (clamped to the line) */
static void goto_line_col(int line, int col)
{
    int ls = line_offset(line);
    int len = line_end(ls) - ls;
    NP_CUR = ls + (col < len ? col : len);
}

static void np_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
    {
        wm_key_partial(); /* a release changes nothing: no redraw at all */
        return;
    }
    int vertical = 0;
    int old_lines = line_count(), old_line = line_of(NP_CUR);
    int old_top = NP_TOP, old_hscroll = NP_HSCROLL;

    switch (k->key)
    {
    case KEY_LEFT:
        if (NP_CUR > 0)
            NP_CUR--;
        break;
    case KEY_RIGHT:
        if (NP_CUR < NP_LEN)
            NP_CUR++;
        break;
    case KEY_UP:
    case KEY_DOWN:
    case KEY_PAGEUP:
    case KEY_PAGEDOWN:
    {
        int step = (k->key == KEY_UP || k->key == KEY_DOWN) ? 1 : NP_ROWS;
        int dir = (k->key == KEY_UP || k->key == KEY_PAGEUP) ? -1 : 1;
        int line = line_of(NP_CUR) + dir * step;
        if (line < 0)
            line = 0;
        if (line > line_count() - 1)
            line = line_count() - 1;
        goto_line_col(line, NP_DESIRED);
        vertical = 1;
        break;
    }
    case KEY_HOME:
        NP_CUR = line_start(NP_CUR);
        break;
    case KEY_END:
        NP_CUR = line_end(NP_CUR);
        break;
    case KEY_BACKSPACE:
        if (NP_CUR > 0)
            delete_at(--NP_CUR);
        break;
    case KEY_DELETE:
        if (NP_CUR < NP_LEN)
            delete_at(NP_CUR);
        break;
    case KEY_ENTER:
        insert_char('\n');
        break;
    case KEY_TAB:
        for (int i = 0; i < NP_TAB; i++)
            insert_char(' ');
        break;
    default:
        if (k->ch >= 32 && k->ch < 127 && !(k->mods & (KMOD_CTRL | KMOD_ALT)))
            insert_char((char)k->ch);
        break;
    }

    if (!vertical)
        NP_DESIRED = NP_CUR - line_start(NP_CUR);
    NP_FOLLOW = 1;
    ui_caret_restart();
    NP_BLINK = 1;

    /* Tell the window manager what really changed, so it does not redraw the whole window per key: the status
     * bar always, and either the rows between the old and the new caret line, or (if the text scrolled or the
     * number of lines changed, so the rows below moved) the whole text area. */
    UiRect body = wm_content(w);
    UiRect in = ui_inset(text_area(body), NP_PAD);
    follow(in);
    int new_line = line_of(NP_CUR);
    UiRect rows = in;
    if (NP_TOP == old_top && NP_HSCROLL == old_hscroll && line_count() == old_lines)
    {
        int a = old_line < new_line ? old_line : new_line;
        int b = old_line < new_line ? new_line : old_line;
        if (a < NP_TOP)
            a = NP_TOP;
        if (b > NP_TOP + NP_ROWS - 1)
            b = NP_TOP + NP_ROWS - 1;
        rows = ui_rect(in.x, in.y + (a - NP_TOP) * np_line_h(), in.w, (b - a + 1) * np_line_h());
    }
    if (rows.h > 0)
        wm_add_damage(rows);
    wm_add_damage(status_rect(body));
    wm_key_partial();
}

static int np_press(Window *w, int x, int y)
{
    UiRect body = wm_content(w);
    UiRect in = ui_inset(text_area(body), NP_PAD);
    if (!ui_contains(in, x, y))
        return 0;

    int line = NP_TOP + (y - in.y) / np_line_h();
    if (line > line_count() - 1)
        line = line_count() - 1;
    int ls = line_offset(line);
    char tmp[NP_LINE_MAX + 1];
    copy_text(ls, line_end(ls) - ls, tmp);
    int col = gfx_text_fit(np_font(), tmp, x - in.x + NP_HSCROLL + 3); /* +3: nearer half of a letter wins */
    NP_CUR = ls + col;
    NP_DESIRED = col;
    NP_FOLLOW = 1;
    ui_caret_restart();
    NP_BLINK = 1;
    return 1;
}

static int np_wheel(Window *w, int steps)
{
    (void)w;
    int top = NP_TOP + steps * 3;
    if (top > line_count() - 1)
        top = line_count() - 1;
    if (top < 0)
        top = 0;
    if (top == NP_TOP)
        return 0;
    NP_TOP = top;
    return 1;
}

/* the caret blinks: redraw the window when it appears or disappears */
static int np_tick(Window *w)
{
    (void)w;
    int vis = ui_caret_visible();
    if (vis == NP_BLINK)
        return 0;
    NP_BLINK = vis;
    if (NP_CARET.w == 0)
        return 0;
    wm_add_damage(ui_rect(NP_CARET.x - 1, NP_CARET.y - 1, NP_CARET.w + 2, NP_CARET.h + 2)); /* just the caret, not the window */
    return 2;
}

static void np_close(Window *w)
{
    (void)w;
    NP_WIN = 0;
}

static void notepad_app_open(void)
{
    if (NP_WIN)
    {
        wm_activate(NP_WIN);
        return;
    }
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 640, h = 440;
    if (w > W - 20)
        w = W - 20;
    if (h > H - TASKBAR_HEIGHT - 8)
        h = H - TASKBAR_HEIGHT - 8;
    int x = (W - w) / 2, y = (H - TASKBAR_HEIGHT - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;

    Window *win = wm_create("Notepad", x, y, w, h, np_draw, 0);
    if (!win)
    {
        desktop_set_status("Window limit reached - close one first.");
        wm_invalidate();
        return;
    }
    win->on_key = np_key;
    win->on_press = np_press;
    win->on_wheel = np_wheel;
    win->on_tick = np_tick;
    win->on_close = np_close;
    win->icon = TASKBAR_ICON_NOTEPAD;
    wm_set_min_size(win, 320, 220);
    NP_WIN = win;
    desktop_set_status(0);
    NP_FOLLOW = 1;
}

const AppInfo APP_NOTEPAD = {
    .name = "Notepad",
    .desc = "Write plain text",
    .icon = UI_ICON_NOTEPAD,
    .open = notepad_app_open,
};
