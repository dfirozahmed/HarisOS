/* ============================================================
 * HarisOS console: the text window a program writes to and reads from
 *
 *   int con = console_window_open("hello.elf");   a new Console window on the desktop (apps/console), or -1
 *   console_write(con, "text\n", 5);              what the program printed (SYS_WRITE on fd 1 and 2)
 *   console_read(con, buf, n);                    what the user typed, a line at a time (SYS_READ on fd 0)
 *   console_finish(con, code);                    the program ended: "[process exited, code N]"
 *
 * A console is a small terminal: a grid of CONSOLE_COLS x CONSOLE_ROWS characters (the rows above the window are scroll
 * back), a cursor, and a line editor for input. Typed characters are echoed on the screen and collected in a line;
 * Enter makes the line readable. A read with no finished line waits (the program's thread is blocked, see task.h) and
 * is woken by Enter. Control characters are not interpreted except \n, \r, \b and \t.
 *
 * A console is named by a handle: the slot and a generation number, so a handle left over from a closed console (a
 * program that outlives its window) is simply invalid, and never points at another program's console. Everything here
 * accepts an invalid handle and does nothing / returns "end of input".
 *
 * console.c has no window code: it is a buffer and a line editor, tested on the host. The window is apps/console.
 * ============================================================ */
#ifndef HARIS_CONSOLE_H
#define HARIS_CONSOLE_H

#define CONSOLE_MAX 256 /* consoles that can exist at once (a handle holds the slot in 8 bits): the table is grown as needed up to this */
#define CONSOLE_COLS 100
#define CONSOLE_ROWS 200       /* rows kept, including the ones scrolled out of the window */
#define CONSOLE_TITLE_MAX 32
#define CONSOLE_LINE_MAX 200   /* characters in a line being typed */
#define CONSOLE_INPUT_MAX 1024 /* bytes of finished lines waiting to be read */

/* ---- the buffer (console.c) ---------------------------------- */
int console_open(const char *title); /* a new empty console: its handle, or -1 if all CONSOLE_MAX are in use */
void console_close(int con);         /* its window is gone; a blocked reader is woken and sees end of input */
int console_valid(int con);

/* Output: returns the number of bytes taken (n), or -1 for an invalid handle. */
int console_write(int con, const char *text, unsigned n);
void console_finish(int con, int exit_code);
int console_finished(int con);

/* Ctrl+C: "^C" is shown, the line being typed is dropped, and the hook (below) is called with the console: the process layer
 * ends the programs that write to it. 1 if the screen changed. */
int console_interrupt(int con);
void console_set_interrupt_hook(void (*hook)(int con)); /* NULL = none (the host tests) */

/* Input: one typed character (ch is a printable character, '\n' for Enter or '\b' for Backspace). 1 if the screen changed. */
int console_key(int con, int ch);
/* Up to len bytes of the lines that Enter has finished. Returns the count (> 0), 0 = end of input (the console is gone or
 * its program has ended), -1 = nothing yet (call console_wait, then block, then try again). */
int console_read(int con, void *buf, int len);
void console_wait(int con, int task_id); /* wake task_id (task_wake) when input arrives or the console ends */

/* For drawing. console_row copies the text of the row `back` rows above the bottom (0 = the row with the cursor) into
 * out[CONSOLE_COLS + 1], without trailing spaces, and returns 1; 0 if there is no such row yet. */
int console_row(int con, int back, char *out);
int console_cursor_col(int con); /* column of the cursor on the bottom row, -1 if the console is finished or invalid */
int console_dirty(int con);      /* 1 once after every change of the screen: the window redraws then */
const char *console_title(int con);

/* ---- the window (apps/console) ------------------------------- */
int console_window_open(const char *title); /* console + window; the handle, or -1 (no free console or window) */

#endif
