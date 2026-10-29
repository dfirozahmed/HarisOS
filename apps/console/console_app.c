/* Console window: where a program started from the File Explorer prints and reads.
 *
 * It is not an app in the start menu: console_window_open() is called when a program is launched (see
 * include/console.h). The window only draws the character grid of a console (kernel/userio/console.c) and passes typed
 * keys to its line editor. Everything the program does goes through the grid: system calls write into it from the
 * program's thread and mark it changed; this window notices on its next tick and redraws, so no window code ever runs
 * inside a system call. Closing the window closes the console: the program's reads then return end of input, which
 * ends well-behaved programs. */
#include "console.h"
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

static int handle_of(const Window *w) { return (int)(unsigned long long)w->user; }

/* How many text rows fit in the body, and the height of one. */
static int grid_rows(UiRect body, int *line_h)
{
    *line_h = font_line_height(&font_ui_14) + 2;
    int rows = (body.h - 2 * PAD) / *line_h;
    return rows < 1 ? 1 : rows;
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
    gfx_fill_rect(body.x, body.y, body.w, body.h, ui_color(ui_rgb(18, 20, 28)));
    UiRgb text_rgb = ui_rgb(220, 224, 232);
    GfxColor text_color = ui_color(text_rgb);
    const Font *f = &font_ui_14;
    int line_h;
    int rows = grid_rows(body, &line_h);

    /* The newest row is at the bottom: row `back` rows above the one with the cursor is drawn (rows - 1 - back) lines down. */
    char text[CONSOLE_COLS + 1];
    for (int i = 0; i < rows; i++)
    {
        int back = rows - 1 - i;
        if (console_row(con, back, text))
            gfx_text(f, body.x + PAD, body.y + PAD + i * line_h, text, text_color);
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
    else if (k->ch >= 32 && k->ch < 127)
        ch = k->ch;
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

int console_window_open(const char *title)
{
    int con = console_open(title);
    if (con < 0)
        return -1;
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
    win->icon = TASKBAR_ICON_TERMINAL;
    LAST_BLINK[con & 255] = (int)((timer_ms() / 500) & 1);
    return con;
}
