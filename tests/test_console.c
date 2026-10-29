/* Host test for console.c: the character grid, the cursor, wrapping, scrolling, the line editor and handles. */
#include "console.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* the kernel heap: host memory, counted, with a switch that makes it fail */
static int HEAP_BLOCKS, HEAP_FAIL;
void *kmalloc(unsigned long long size)
{
    if (HEAP_FAIL)
        return 0;
    void *p = calloc(1, size ? (size_t)size : 1);
    if (p)
        HEAP_BLOCKS++;
    return p;
}
void kfree(void *p)
{
    if (p)
    {
        free(p);
        HEAP_BLOCKS--;
    }
}
void *krealloc(void *p, unsigned long long size)
{
    if (HEAP_FAIL)
        return 0;
    void *n = realloc(p, size ? (size_t)size : 1);
    if (n && !p)
        HEAP_BLOCKS++;
    return n;
}

/* ---- the scheduler calls the console makes ---- */
static int WOKEN[16], WOKEN_N;
void task_wake(int id)
{
    if (WOKEN_N < 16)
        WOKEN[WOKEN_N++] = id;
}

static int HOOKED[8], HOOKED_N;
static void on_interrupt(int con) { if (HOOKED_N < 8) HOOKED[HOOKED_N++] = con; }

static void put(int con, const char *s) { console_write(con, s, (unsigned)strlen(s)); }

static int row(int con, int back, char *out)
{
    out[0] = 'X';
    return console_row(con, back, out);
}

static void type(int con, const char *s)
{
    for (; *s; s++)
        console_key(con, *s == '\r' ? '\n' : *s);
}

int main(void)
{
    char r[CONSOLE_COLS + 8];

    /* a new console: one empty row */
    int a = console_open("PROG A");
    CHECK(a > 0 && console_valid(a) && !console_valid(-1) && !console_valid(0));
    CHECK(strcmp(console_title(a), "PROG A") == 0 && console_title(-1)[0] == 0);
    CHECK(row(a, 0, r) == 1 && r[0] == 0);
    CHECK(row(a, 1, r) == 0); /* no history yet */
    CHECK(row(a, -1, r) == 0);
    CHECK(console_dirty(a) == 1 && console_dirty(a) == 0); /* dirty once after the change, then clean */
    CHECK(console_cursor_col(a) == 0);

    /* text, newline, carriage return, backspace, tab, odd characters */
    CHECK(console_write(a, "hello", 5) == 5);
    CHECK(console_dirty(a) == 1);
    CHECK(row(a, 0, r) && strcmp(r, "hello") == 0 && console_cursor_col(a) == 5);
    put(a, " world\nsecond line\n");
    CHECK(row(a, 0, r) && r[0] == 0);                         /* the cursor is on a new empty row */
    CHECK(row(a, 1, r) && strcmp(r, "second line") == 0);
    CHECK(row(a, 2, r) && strcmp(r, "hello world") == 0);
    put(a, "abc\rXY");
    CHECK(row(a, 0, r) && strcmp(r, "XYc") == 0);             /* \r goes back to column 0 and overwrites */
    put(a, "\b\bZ");
    CHECK(row(a, 0, r) && strcmp(r, "ZYc") == 0);                /* two back from column 2, then Z over the X */
    put(a, "\n\bx");                                           /* backspace at column 0 stays there */
    CHECK(row(a, 0, r) && strcmp(r, "x") == 0);
    put(a, "\n12\t3");
    CHECK(row(a, 0, r) && strcmp(r, "12      3") == 0);      /* tab to the next multiple of 8 */
    put(a, "\n");
    console_write(a, "a\x01\xFFz", 4);
    CHECK(row(a, 0, r) && strcmp(r, "a??z") == 0);            /* control and high bytes show as '?' */
    console_write(a, "no\0nul", 6);
    CHECK(row(a, 0, r) && strlen(r) >= 4); /* an embedded NUL is just a character: it counts as '?' */
    CHECK(console_write(a, "", 0) == 0);

    /* a line longer than the grid wraps */
    put(a, "\n");
    for (int i = 0; i < CONSOLE_COLS + 5; i++)
        console_write(a, "k", 1);
    CHECK(row(a, 0, r) && strcmp(r, "kkkkk") == 0);
    CHECK(row(a, 1, r) && strlen(r) == CONSOLE_COLS);

    /* scrolling: far more rows than the ring keeps; the newest ones are intact, the oldest are gone */
    int b = console_open("PROG B");
    CHECK(b > 0 && b != a);
    for (int i = 0; i < CONSOLE_ROWS * 2 + 7; i++)
    {
        char line[32];
        snprintf(line, sizeof(line), "line %d\n", i);
        put(b, line);
    }
    CHECK(row(b, 0, r) && r[0] == 0);
    CHECK(row(b, 1, r) && strcmp(r, "line 406") == 0);
    CHECK(row(b, 2, r) && strcmp(r, "line 405") == 0);
    CHECK(row(b, CONSOLE_ROWS - 1, r) && strcmp(r, "line 208") == 0); /* the oldest row that is kept */
    CHECK(row(b, CONSOLE_ROWS, r) == 0);                              /* ... and nothing older */
    console_close(b);
    CHECK(!console_valid(b));

    /* the line editor: typed text is echoed, Enter makes the line readable, Backspace edits */
    int c = console_open("EDIT");
    char in[64];
    CHECK(console_read(c, in, 64) == -1);                     /* nothing typed: would block */
    put(c, "name? ");
    type(c, "Bob");
    CHECK(row(c, 0, r) && strcmp(r, "name? Bob") == 0);       /* the echo follows the prompt */
    CHECK(console_read(c, in, 64) == -1);                     /* not finished yet: still nothing to read */
    console_key(c, '\b');
    console_key(c, 'x');
    CHECK(row(c, 0, r) && strcmp(r, "name? Box") == 0);
    CHECK(console_key(c, '\n') == 1);
    CHECK(row(c, 0, r) && r[0] == 0 && row(c, 1, r) && strcmp(r, "name? Box") == 0);
    CHECK(console_read(c, in, 64) == 4 && memcmp(in, "Box\n", 4) == 0);
    CHECK(console_read(c, in, 64) == -1);                     /* all taken */
    CHECK(console_read(c, in, 0) == 0);
    CHECK(console_key(c, '\b') == 0);                         /* nothing to erase on an empty line */
    CHECK(console_key(c, 0x01) == 0 && console_key(c, 0x7F) == 0 && console_key(c, 300) == 0);

    /* partial reads, several lines queued, empty line */
    type(c, "one\rtwo\r\r");
    CHECK(console_read(c, in, 2) == 2 && memcmp(in, "on", 2) == 0);
    CHECK(console_read(c, in, 64) == 2 && memcmp(in, "e\n", 2) == 0);      /* a read stops at the end of a line */
    CHECK(console_read(c, in, 64) == 4 && memcmp(in, "two\n", 4) == 0);
    CHECK(console_read(c, in, 64) == 1 && in[0] == '\n');     /* an empty line is just the newline */
    CHECK(console_read(c, in, 64) == -1);

    /* a line that is too long stops accepting characters (and the screen shows only what was accepted) */
    for (int i = 0; i < CONSOLE_LINE_MAX + 50; i++)
        console_key(c, 'q');
    console_key(c, '\n');
    int n = console_read(c, in, 64);
    int total = n;
    while (n > 0 && (n = console_read(c, in, 64)) > 0)
        total += n;
    CHECK(total == CONSOLE_LINE_MAX - 1 + 1); /* the accepted characters and the newline */

    /* waking the reader: the waiter is woken by Enter, once; typing without Enter does not wake it */
    WOKEN_N = 0;
    console_wait(c, 5);
    type(c, "hi");
    CHECK(WOKEN_N == 0);
    console_key(c, '\n');
    CHECK(WOKEN_N == 1 && WOKEN[0] == 5);
    console_key(c, 'x');
    console_key(c, '\n');
    CHECK(WOKEN_N == 1); /* nobody waits any more */
    while (console_read(c, in, 64) > 0) /* take both lines */
        ;

    /* the program ends: the message, end of input after the queue is empty, no more typing */
    put(c, "partial");
    WOKEN_N = 0;
    console_wait(c, 6);
    console_finish(c, 42);
    CHECK(console_finished(c) && WOKEN_N == 1 && WOKEN[0] == 6);
    CHECK(row(c, 1, r) && strcmp(r, "[process exited, code 42]") == 0);
    CHECK(row(c, 2, r) && strcmp(r, "partial") == 0); /* the message starts on a new row */
    CHECK(console_read(c, in, 64) == 0);              /* end of input */
    CHECK(console_key(c, 'a') == 0);
    CHECK(console_cursor_col(c) == -1);
    console_finish(c, 1); /* twice: nothing changes */
    CHECK(row(c, 1, r) && strcmp(r, "[process exited, code 42]") == 0);
    console_close(c);

    int d = console_open("D");
    console_finish(d, -7);
    CHECK(row(d, 1, r) && strcmp(r, "[process exited, code -7]") == 0);
    console_close(d);

    /* a typed line that was finished before the program ended can still be read */
    int e = console_open("E");
    type(e, "last\r");
    console_finish(e, 0);
    CHECK(console_read(e, in, 64) == 5 && memcmp(in, "last\n", 5) == 0);
    CHECK(console_read(e, in, 64) == 0);
    console_close(e);

    /* handles: a closed console is invalid, a reused slot gets a new handle, the old one stays invalid */
    int f = console_open("F");
    put(f, "old text\n");
    int f_slot = f & 255;
    WOKEN_N = 0;
    console_wait(f, 9);
    console_close(f);
    CHECK(WOKEN_N == 1 && WOKEN[0] == 9);                    /* a blocked reader is woken when the window goes */
    CHECK(!console_valid(f) && console_read(f, in, 8) == 0 && console_write(f, "x", 1) == -1);
    CHECK(console_key(f, 'a') == 0 && console_dirty(f) == 0 && console_finished(f) == 1);
    CHECK(console_row(f, 0, r) == 0 && console_cursor_col(f) == -1);
    console_close(f); /* closing twice is harmless */
    int g = -1;
    for (int tries = 0; tries < CONSOLE_MAX + 1 && (g < 0 || (g & 255) != f_slot); tries++)
    {
        if (g >= 0)
            console_close(g);
        g = console_open("G");
    }
    CHECK(g > 0 && (g & 255) == f_slot && g != f);            /* the slot came back with another generation */
    CHECK(!console_valid(f) && console_valid(g));
    CHECK(row(g, 0, r) && r[0] == 0 && row(g, 1, r) == 0);   /* and it is empty: nothing of the old text */
    console_close(g);
    /* Ctrl+C: "^C" on the screen, the line being typed is dropped (not sent to the reader), the hook is told which console */
    {
        int ic = console_open("INT");
        CHECK(console_interrupt(ic) == 1);                       /* no hook yet: still shows it */
        type(ic, "half a line");
        CHECK(console_interrupt(ic) == 1);
        CHECK(row(ic, 0, r) && r[0] == 0);                        /* back on a fresh row */
        CHECK(row(ic, 1, r) && strcmp(r, "half a line^C") == 0);
        char got[16];
        CHECK(console_read(ic, got, sizeof(got)) == -1);          /* the dropped line was not queued */
        type(ic, "ok\r");
        CHECK(console_read(ic, got, sizeof(got)) == 3 && memcmp(got, "ok\n", 3) == 0);
        console_set_interrupt_hook(on_interrupt);
        HOOKED_N = 0;
        CHECK(console_interrupt(ic) == 1 && HOOKED_N == 1 && HOOKED[0] == ic);
        CHECK(console_interrupt(-1) == 0 && console_interrupt(ic + 100) == 0 && HOOKED_N == 1);
        console_finish(ic, 0);
        CHECK(console_interrupt(ic) == 0 && HOOKED_N == 1);       /* a finished console takes no more */
        console_set_interrupt_hook(0);
        console_close(ic);
        CHECK(console_interrupt(ic) == 0);                        /* a stale handle */
    }


    /* all slots in use */
    int h[CONSOLE_MAX + 1], opened = 0;
    for (int i = 0; i <= CONSOLE_MAX; i++)
        if ((h[i] = console_open("X")) >= 0)
            opened++;
    CHECK(opened >= 1 && console_open("one too many") == -1);
    for (int i = 0; i <= CONSOLE_MAX; i++)
        console_close(h[i]);
    console_close(a);

    /* the heap: every console went back (only the table of slots stays); no memory means no console, and nothing is left behind */
    CHECK(HEAP_BLOCKS == 1);
    HEAP_FAIL = 1;
    CHECK(console_open("no memory") == -1 && HEAP_BLOCKS == 1);
    HEAP_FAIL = 0;
    int fresh = console_open("fresh");
    CHECK(fresh >= 0 && HEAP_BLOCKS == 2);
    console_close(fresh);
    CHECK(HEAP_BLOCKS == 1);

    /* a long title is cut, not overrun */
    int t = console_open("0123456789012345678901234567890123456789");
    CHECK(strlen(console_title(t)) == CONSOLE_TITLE_MAX - 1);
    console_close(t);

    /* how much changed: 1 = only the cursor's row (the window draws that row), 2 = rows moved (the whole window) */
    int k = console_open("levels");
    CHECK(console_changed(k) == 2 && console_changed(k) == 0); /* new: everything; then nothing */
    CHECK(console_write(k, "abc", 3) == 3 && console_changed(k) == 1);
    console_key(k, 'x');
    CHECK(console_changed(k) == 1);
    console_key(k, '\b');
    CHECK(console_changed(k) == 1);
    console_key(k, '\b'); /* nothing typed any more: nothing changes */
    CHECK(console_changed(k) == 0);
    console_key(k, 'y');
    console_key(k, '\n'); /* Enter: a new row, everything moves up */
    CHECK(console_changed(k) == 2);
    console_write(k, "a\nb", 3); /* a new row in between: still 2 */
    CHECK(console_changed(k) == 2 && console_changed(k) == 0);
    console_write(k, "z", 1);
    CHECK(console_dirty(k) == 1 && console_changed(k) == 0); /* console_dirty clears it too */
    console_close(k);

    if (fails == 0)
        printf("console tests passed\n");
    return fails ? 1 : 0;
}
