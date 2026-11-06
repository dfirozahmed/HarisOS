/* Console window: where a program started from the File Explorer prints and reads.
 *
 * It is not an app in the start menu: console_window_open() is called when a program is launched (see
 * include/console.h). The window only draws the character grid of a console (kernel/userio/console.c) and passes typed
 * keys to its line editor. Everything the program does goes through the grid: system calls write into it from the
 * program's thread and mark it changed; this window notices on its next tick and redraws, so no window code ever runs
 * inside a system call. Closing the window closes the console: the program's reads then return end of input, which
 * ends well-behaved programs. */
#include "console.h"
#include "console_sel.h"
#include "elf.h"
#include "font.h"
#include "gfx.h"
#include "keyboard.h"
#include "taskbar.h"
#include "timer.h"
#include "ui.h"
#include "wm.h"

#define PAD 10
#define WIN_W 640
#define WIN_H 380

static int OPENED;             /* windows opened so far: each new one starts a bit lower and further right */
static int LAST_BLINK[CONSOLE_MAX]; /* by console slot (the low 8 bits of a handle) */

/* The mouse selection of a window (see console_sel.h), by console slot like LAST_BLINK. A press starts it, dragging grows it,
 * a double click takes a word, a triple click a row. Ctrl+Insert / Ctrl+Shift+C or a right click copy it (Ctrl+C stays
 * "interrupt the program"); typing a character or a click elsewhere drops it. */
typedef struct
{
    ConSel sel;
    int clicks;                  /* 1, 2, 3: presses in a row at about the same place */
    unsigned long long click_ms; /* when the last press was */
    int px, py;                  /* where it was */
    int dragging;                /* the left button is held after a single press: moving it grows the selection */
} SelState;
static SelState SELS[CONSOLE_MAX];
#define DOUBLE_CLICK_MS 450

static int handle_of(const Window *w) { return (int)(unsigned long long)w->user; }
static SelState *sel_of(const Window *w) { return &SELS[handle_of(w) & 255]; }

/* How many text rows fit in the body, and the height of one. */
static int grid_rows(UiRect body, int *line_h)
{
    *line_h = font_line_height(&font_ui_14) + 2;
    int rows = (body.h - 2 * PAD) / *line_h;
    return rows < 1 ? 1 : rows;
}

/* Width in pixels of the first n bytes of a row (n is at the start of a character). */
static int prefix_w(const Font *f, const char *text, int n)
{
    char tmp[CONSOLE_COLS + 1];
    int i = 0;
    for (; i < n && text[i]; i++)
        tmp[i] = text[i];
    tmp[i] = 0;
    return gfx_text_w(f, tmp);
}

/* The point of the text under the screen position (x, y): a row number and a byte column. Above or below the text it is the
 * first / last visible row; left of it column 0, right of it the end of the row. */
static void point_at(const Window *w, int x, int y, ConPoint *p)
{
    int con = handle_of(w);
    UiRect body = wm_content((Window *)w);
    int line_h, rows = grid_rows(body, &line_h);
    int i = y - body.y - PAD < 0 ? 0 : (y - body.y - PAD) / line_h;
    if (i > rows - 1)
        i = rows - 1;
    unsigned lines = console_lines(con);
    int back = rows - 1 - i;
    if ((unsigned)back > lines)
        back = (int)lines; /* above the first row of a console that has few: the first row */
    p->row = lines - (unsigned)back;
    char text[CONSOLE_COLS + 1];
    if (!console_row(con, back, text))
        text[0] = 0;
    int rel = x - body.x - PAD;
    p->col = rel <= 0 ? 0 : gfx_text_fit(&font_ui_14, text, rel + 3); /* +3: the nearer half of a letter wins */
}

/* Put the selected text on the clipboard. 1 if there was any. */
static int copy_selection(Window *w)
{
    static char buf[CONSOLE_ROWS * (CONSOLE_COLS + 1) + 1];
    SelState *s = sel_of(w);
    if (!s->sel.on)
        return 0;
    int n = con_sel_text(handle_of(w), &s->sel, buf, (int)sizeof buf);
    return n > 0 && ui_clipboard_set(buf, n);
}

/* Mark the bottom text row (the one with the cursor) to be drawn again. */
static void mark_cursor_row(Window *w)
{
    UiRect body = wm_content(w);
    int line_h, rows = grid_rows(body, &line_h);
    wm_add_damage(ui_rect(body.x, body.y + PAD + (rows - 1) * line_h, body.w, line_h));
}

static void draw(Window *w, UiRect body)
{
    int con = handle_of(w);
    ui_fill_body(body, !w->maximized && !w->dialog, ui_rgb(18, 20, 28));
    UiRgb text_rgb = ui_rgb(220, 224, 232);
    GfxColor text_color = ui_color(text_rgb);
    const Font *f = &font_ui_14;
    int line_h;
    int rows = grid_rows(body, &line_h);

    /* The newest row is at the bottom: row `back` rows above the one with the cursor is drawn (rows - 1 - back) lines down. */
    char text[CONSOLE_COLS + 1];
    const ConSel *sel = &SELS[con & 255].sel;
    unsigned lines = console_lines(con);
    for (int i = 0; i < rows; i++)
    {
        int back = rows - 1 - i;
        if (console_row(con, back, text))
        {
            int y = body.y + PAD + i * line_h, from, to, more;
            if (sel->on && (unsigned)back <= lines && con_sel_row_span(sel, lines - (unsigned)back, text, &from, &to, &more))
            {
                int x0 = body.x + PAD + prefix_w(f, text, from), x1 = body.x + PAD + prefix_w(f, text, to) + (more ? 6 : 0);
                if (x1 > x0) /* the selected part (and the line break, when the selection goes on to the next row) */
                {
                    const UiTheme *t = ui_theme();
                    gfx_fill_rect_alpha(x0, y - 1, x1 - x0, line_h, t->accent.r, t->accent.g, t->accent.b, 110);
                }
            }
            gfx_text(f, body.x + PAD, y, text, text_color);
        }
    }

    /* the cursor: a thin bar after the text of the bottom row, blinking, until the program has ended */
    int col = console_cursor_col(con);
    if (col >= 0 && ((timer_ms() / 500) & 1) == 0 && console_row(con, 0, text))
    {
        char upto[CONSOLE_COLS + 1];
        int n = 0;
        for (; n < col && text[n]; n++)
            upto[n] = text[n];
        for (; n < col; n++) /* the cursor can be past the last character (after spaces): they count too */
            upto[n] = ' ';
        upto[n] = 0;
        int x = body.x + PAD + gfx_text_w(f, upto);
        gfx_fill_rect(x + 1, body.y + PAD + (rows - 1) * line_h, 2, line_h - 2, text_color);
    }
}

static void on_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    /* Ctrl+Insert and Ctrl+Shift+C copy the selected text. (Ctrl+C alone must stay "end the program", as in a terminal.) */
    if ((k->mods & KMOD_CTRL) &&
        (k->key == KEY_INSERT || ((k->mods & KMOD_SHIFT) && (k->key == 'c' || k->key == 'C'))))
    {
        copy_selection(w);
        return;
    }
    /* Ctrl+C ends the program that runs here. (With Ctrl held ch is 0: the letter is in key.) */
    if ((k->mods & KMOD_CTRL) && (k->key == 'c' || k->key == 'C'))
    {
        console_interrupt(handle_of(w));
        return;
    }
    int ch = 0;
    if (k->key == KEY_ENTER || k->ch == '\n')
        ch = '\n';
    else if (k->key == KEY_BACKSPACE)
        ch = '\b';
    else if ((k->ch >= 32 && k->ch < 127) || (k->ch >= 0x80 && k->ch <= 0x10FFFF)) /* ASCII or a Unicode code point */
        ch = k->ch;
    if (ch && sel_of(w)->sel.on) /* typing drops the selection: the highlighted rows must be drawn again */
    {
        sel_of(w)->sel.on = 0;
        wm_add_damage(wm_content(w));
    }
    if (ch)
        console_key(handle_of(w), ch);
    /* What the key changed (and anything the program printed meanwhile): on the cursor's row only (a typed character, a
     * backspace) that row is drawn again; when rows moved (Enter) the window manager draws the whole window. */
    switch (console_changed(handle_of(w)))
    {
    case 0:
        wm_key_partial(); /* nothing to draw */
        break;
    case 1:
        mark_cursor_row(w);
        wm_key_partial();
        break;
    default:
        break;
    }
}

static int on_tick(Window *w)
{
    int con = handle_of(w);
    int printed = console_changed(con); /* the program printed: 1 = on the cursor's row only, 2 = rows moved up */
    int blink = (int)((timer_ms() / 500) & 1);
    int *last = &LAST_BLINK[con & 255];
    int cursor = 0;
    if (blink != *last)
    {
        *last = blink; /* the cursor turns on or off */
        cursor = console_cursor_col(con) >= 0;
    }
    if (printed == 2)
        return 1;
    if (!printed && !cursor)
        return 0;
    /* Only the cursor's row changed (the cursor blinked, or text on that row): only that row is drawn again. */
    mark_cursor_row(w);
    return 2;
}

static void on_close(Window *w) { console_close(handle_of(w)); }

/* ---- the mouse: select text ---- */
static int on_press(Window *w, int x, int y)
{
    SelState *s = sel_of(w);
    ConPoint p;
    point_at(w, x, y, &p);
    unsigned long long now = timer_ready() ? timer_ms() : 0;
    int dx = x - s->px, dy = y - s->py;
    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    if (timer_ready() && s->clicks > 0 && now - s->click_ms <= DOUBLE_CLICK_MS && dx <= 4 && dy <= 4)
        s->clicks = s->clicks >= 3 ? 1 : s->clicks + 1;
    else
        s->clicks = 1;
    s->click_ms = now;
    s->px = x;
    s->py = y;
    int had = s->sel.on;
    s->dragging = 0;
    if (s->clicks == 1) /* a single press: the start of a drag (nothing is selected until the pointer moves) */
    {
        s->sel.a = s->sel.b = p;
        s->sel.on = 0;
        s->dragging = 1;
        return had ? 1 : 0;
    }
    /* double click: the word; triple click: the whole row */
    char text[CONSOLE_COLS + 1];
    unsigned lines = console_lines(handle_of(w));
    if (p.row > lines || !console_row(handle_of(w), (int)(lines - p.row), text))
        text[0] = 0;
    int from = 0, to = 0;
    if (s->clicks == 2)
        con_sel_word(text, p.col, &from, &to);
    else
        while (text[to])
            to++;
    s->sel.a.row = s->sel.b.row = p.row;
    s->sel.a.col = from;
    s->sel.b.col = to;
    s->sel.on = to > from;
    return 1;
}

static int on_drag(Window *w, int x, int y, unsigned buttons)
{
    SelState *s = sel_of(w);
    if (!(buttons & 1))
    {
        s->dragging = 0;
        return 0;
    }
    if (!s->dragging)
        return 0;
    ConPoint p;
    point_at(w, x, y, &p);
    if (con_point_cmp(p, s->sel.b) == 0)
        return 0;
    s->sel.b = p;
    s->sel.on = con_point_cmp(s->sel.a, s->sel.b) != 0;
    return 1;
}

/* A right click copies what is selected (and drops the selection), as the Windows terminal does. */
static int on_rpress(Window *w, int x, int y)
{
    (void)x;
    (void)y;
    SelState *s = sel_of(w);
    if (!s->sel.on)
        return 0;
    copy_selection(w);
    s->sel.on = 0;
    return 1;
}

/* The window of an existing console. 0 on success; on failure the console is closed. */
static int window_for(int con, const char *title)
{
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = WIN_W < W - 20 ? WIN_W : W - 20, h = WIN_H < H - TASKBAR_HEIGHT - 20 ? WIN_H : H - TASKBAR_HEIGHT - 20;
    int step = (OPENED++ % 5) * 28;
    int x = (W - w) / 2 - 60 + step, y = (H - TASKBAR_HEIGHT - h) / 2 - 40 + step;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    char name[WM_TITLE_MAX];
    const char *prefix = "Console - ";
    int n = 0;
    for (; prefix[n]; n++)
        name[n] = prefix[n];
    for (int i = 0; title[i] && n < WM_TITLE_MAX - 1; i++)
        name[n++] = title[i];
    name[n] = 0;

    Window *win = wm_create(name, x, y, w, h, draw, (void *)(unsigned long long)con);
    if (!win)
    {
        console_close(con);
        return -1;
    }
    win->on_key = on_key;
    win->on_tick = on_tick;
    win->on_close = on_close;
    win->on_press = on_press;
    win->on_drag = on_drag;
    win->on_rpress = on_rpress;
    win->icon = TASKBAR_ICON_TERMINAL;
    LAST_BLINK[con & 255] = (int)((timer_ms() / 500) & 1);
    SELS[con & 255].sel.on = 0; /* a slot is used again by a new console: no selection left over */
    SELS[con & 255].clicks = 0;
    SELS[con & 255].dragging = 0;
    return 0;
}

int console_window_open(const char *title)
{
    int con = console_open(title);
    if (con < 0)
        return -1;
    return window_for(con, title) ? -1 : con;
}

int console_window_open_for(const char *title, int subsystem)
{
    if (subsystem == ELF_SUBSYS_CONSOLE)
        return console_window_open(title);
    return console_open_hidden(title, subsystem == ELF_SUBSYS_GUI);
}

int console_windows_poll(void)
{
    int opened = 0, con;
    while ((con = console_take_pending()) >= 0)
        opened |= window_for(con, console_title(con)) == 0;
    return opened;
}
