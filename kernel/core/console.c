/* HarisOS console: see include/console.h. A character grid, a cursor and a line editor. No window code. */
#include "console.h"
#include "task.h"

typedef struct
{
    int used;
    unsigned gen; /* part of the handle: changes every time the slot is closed */
    char title[CONSOLE_TITLE_MAX];
    int finished;
    int dirty;
    unsigned lines; /* rows started so far: the row with the cursor is lines % CONSOLE_ROWS */
    int col;
    char line[CONSOLE_LINE_MAX]; /* the line being typed */
    int line_len;
    char queue[CONSOLE_INPUT_MAX]; /* finished lines, a ring */
    int q_head, q_count;
    int waiter; /* thread to wake, -1 = none */
} Console;

static Console C[CONSOLE_MAX];
static char CELL[CONSOLE_MAX][CONSOLE_ROWS][CONSOLE_COLS];

/* The handle is gen * 16 + slot; generations start at 1, so a valid handle is never 0 and -1 is never valid. */
static Console *get(int h)
{
    if (h < 0)
        return 0;
    unsigned slot = (unsigned)h & 15u, gen = (unsigned)h >> 4;
    if (slot >= CONSOLE_MAX || !C[slot].used || C[slot].gen != gen)
        return 0;
    return &C[slot];
}

static int slot_of(const Console *c) { return (int)(c - C); }

/* Writes go through volatile: no C library here, and a plain fill loop could be turned into a call to memset. */
static void clear_row(Console *c, unsigned row)
{
    volatile char *r = CELL[slot_of(c)][row % CONSOLE_ROWS];
    for (int i = 0; i < CONSOLE_COLS; i++)
        r[i] = ' ';
}

static void new_row(Console *c)
{
    c->lines++;
    c->col = 0;
    clear_row(c, c->lines);
}

static char *cell(Console *c, int col) { return &CELL[slot_of(c)][c->lines % CONSOLE_ROWS][col]; }

static void put_char(Console *c, char ch)
{
    if (ch == '\n')
        new_row(c);
    else if (ch == '\r')
        c->col = 0;
    else if (ch == '\b')
    {
        if (c->col > 0)
            c->col--;
    }
    else if (ch == '\t')
    {
        int stop = (c->col / 8 + 1) * 8;
        while (c->col < stop)
            put_char(c, ' ');
    }
    else
    {
        if (c->col >= CONSOLE_COLS) /* a long line wraps */
            new_row(c);
        *(volatile char *)cell(c, c->col++) = (ch >= 32 && ch < 127) ? ch : '?';
    }
    c->dirty = 1;
}

static void wake(Console *c)
{
    if (c->waiter >= 0)
    {
        task_wake(c->waiter);
        c->waiter = -1;
    }
}

int console_open(const char *title)
{
    for (int i = 0; i < CONSOLE_MAX; i++)
    {
        Console *c = &C[i];
        if (c->used)
            continue;
        if (c->gen == 0)
            c->gen = 1;
        c->used = 1;
        c->finished = 0;
        c->dirty = 1;
        c->lines = 0;
        c->col = 0;
        c->line_len = 0;
        c->q_head = c->q_count = 0;
        c->waiter = -1;
        int n = 0;
        for (; title && title[n] && n < CONSOLE_TITLE_MAX - 1; n++)
            c->title[n] = title[n];
        c->title[n] = 0;
        clear_row(c, 0);
        return (int)(c->gen << 4) | i;
    }
    return -1;
}

void console_close(int h)
{
    Console *c = get(h);
    if (!c)
        return;
    wake(c); /* a reader that is blocked now finds the handle invalid and gets end of input */
    c->used = 0;
    c->gen++;
    if (c->gen == 0 || c->gen > 0x7FFFFFF)
        c->gen = 1;
}

int console_valid(int h) { return get(h) != 0; }

int console_write(int h, const char *text, unsigned n)
{
    Console *c = get(h);
    if (!c)
        return -1;
    for (unsigned i = 0; i < n; i++)
        put_char(c, text[i]);
    return (int)n;
}

static void put_text(Console *c, const char *s)
{
    for (; *s; s++)
        put_char(c, *s);
}

void console_finish(int h, int code)
{
    Console *c = get(h);
    if (!c || c->finished)
        return;
    if (c->col > 0)
        put_char(c, '\n');
    put_text(c, "[process exited, code ");
    char digits[12];
    int n = 0;
    unsigned v = code < 0 ? 0u - (unsigned)code : (unsigned)code;
    do
        digits[n++] = (char)('0' + v % 10);
    while ((v /= 10) != 0);
    if (code < 0)
        put_char(c, '-');
    while (n)
        put_char(c, digits[--n]);
    put_text(c, "]");
    put_char(c, '\n');
    c->finished = 1;
    wake(c);
}

int console_finished(int h)
{
    Console *c = get(h);
    return c ? c->finished : 1;
}

static void enqueue(Console *c, char b)
{
    if (c->q_count < CONSOLE_INPUT_MAX) /* a full queue drops what does not fit: nobody is reading */
    {
        c->queue[(c->q_head + c->q_count) % CONSOLE_INPUT_MAX] = b;
        c->q_count++;
    }
}

int console_key(int h, int ch)
{
    Console *c = get(h);
    if (!c || c->finished)
        return 0;
    if (ch == '\b')
    {
        if (c->line_len == 0)
            return 0;
        c->line_len--;
        if (c->col > 0) /* erase the echoed character */
        {
            c->col--;
            *(volatile char *)cell(c, c->col) = ' ';
        }
        c->dirty = 1;
        return 1;
    }
    if (ch == '\n')
    {
        for (int i = 0; i < c->line_len; i++)
            enqueue(c, c->line[i]);
        enqueue(c, '\n');
        c->line_len = 0;
        put_char(c, '\n');
        wake(c);
        return 1;
    }
    if (ch >= 32 && ch < 127 && c->line_len < CONSOLE_LINE_MAX - 1)
    {
        c->line[c->line_len++] = (char)ch;
        put_char(c, (char)ch);
        return 1;
    }
    return 0;
}

static void (*INTERRUPT_HOOK)(int);

void console_set_interrupt_hook(void (*hook)(int con)) { INTERRUPT_HOOK = hook; }

int console_interrupt(int h)
{
    Console *c = get(h);
    if (!c || c->finished)
        return 0;
    c->line_len = 0; /* what was typed on this line is dropped, as a terminal does */
    put_char(c, '^');
    put_char(c, 'C');
    put_char(c, '\n');
    if (INTERRUPT_HOOK)
        INTERRUPT_HOOK(h);
    return 1;
}

int console_read(int h, void *buf, int len)
{
    Console *c = get(h);
    if (!c)
        return 0;
    if (len <= 0)
        return 0;
    if (c->q_count == 0)
        return c->finished ? 0 : -1;
    /* like a terminal in line mode: a read never goes past the end of a line, so a program that asks for 4096 bytes gets
     * the one line the user typed, not the next one too */
    char *out = (char *)buf;
    int n = 0;
    while (n < len && c->q_count > 0)
    {
        char b = c->queue[c->q_head];
        out[n++] = b;
        c->q_head = (c->q_head + 1) % CONSOLE_INPUT_MAX;
        c->q_count--;
        if (b == '\n')
            break;
    }
    return n;
}

void console_wait(int h, int task_id)
{
    Console *c = get(h);
    if (c)
        c->waiter = task_id;
}

int console_row(int h, int back, char *out)
{
    Console *c = get(h);
    if (!c || back < 0)
        return 0;
    unsigned available = c->lines + 1 < CONSOLE_ROWS ? c->lines + 1 : CONSOLE_ROWS;
    if ((unsigned)back >= available)
        return 0;
    const char *row = CELL[slot_of(c)][(c->lines - (unsigned)back) % CONSOLE_ROWS];
    int n = CONSOLE_COLS;
    while (n > 0 && row[n - 1] == ' ')
        n--;
    volatile char *o = out; /* volatile: a plain copy loop could be turned into a call to memcpy (none in this build) */
    for (int i = 0; i < n; i++)
        o[i] = row[i];
    o[n] = 0;
    return 1;
}

int console_cursor_col(int h)
{
    Console *c = get(h);
    return !c || c->finished ? -1 : (c->col < CONSOLE_COLS ? c->col : CONSOLE_COLS - 1);
}

int console_dirty(int h)
{
    Console *c = get(h);
    if (!c)
        return 0;
    int d = c->dirty;
    c->dirty = 0;
    return d;
}

const char *console_title(int h)
{
    Console *c = get(h);
    return c ? c->title : "";
}
