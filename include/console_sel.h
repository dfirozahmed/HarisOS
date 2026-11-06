/* ============================================================
 * HarisOS console: selecting text with the mouse (the logic; apps/console draws and reads the mouse)
 *
 * A point in the text is a row number (the scale of console_lines(): a row keeps its number while more rows follow, so a
 * selection stays on its text as the screen scrolls) and a byte column in that row. A selection is two points: a, where
 * the press began, and b, where the pointer is now (b can be before a). Columns are bytes of UTF-8 text and are always
 * moved back to the start of a character, so a Bengali letter is never cut.
 * ============================================================ */
#ifndef HARIS_CONSOLE_SEL_H
#define HARIS_CONSOLE_SEL_H

typedef struct
{
    unsigned row;
    int col;
} ConPoint;

typedef struct
{
    int on; /* 1 = something is selected (a and b differ) */
    ConPoint a, b;
} ConSel;

/* Order of two points: negative, 0, positive. */
int con_point_cmp(ConPoint x, ConPoint y);

/* The part of row `row` (whose text is `text`) that the selection covers: [*from, *to) in bytes. Returns 1 if the row is in the
 * selection at all (an empty row between the ends counts); *continues = 1 if the selection goes on into the next row (the
 * window draws the end of the line selected too). */
int con_sel_row_span(const ConSel *s, unsigned row, const char *text, int *from, int *to, int *continues);

/* The word around column col of text: the run of characters that are not spaces (or, on a space, the run of spaces). */
void con_sel_word(const char *text, int col, int *from, int *to);

/* The selected text of console `con`, rows joined by '\n' (no trailing spaces on a row). Writes at most cap - 1 bytes and a
 * terminating 0, never cutting a character; returns the length. */
int con_sel_text(int con, const ConSel *s, char *out, int cap);

/* Paste: type text[0..len) into console `con` as if it was typed (it is echoed and collected in the line being edited). A line
 * break (\n, or \r\n as one) is Enter, so a pasted line runs; a Tab becomes 4 spaces; other control characters and bytes
 * that are not well-formed UTF-8 are skipped. At most CON_PASTE_MAX bytes are taken. Returns how many characters the
 * console accepted (a full line takes no more). */
#define CON_PASTE_MAX (256 * 1024)
int con_paste(int con, const char *text, int len);

#endif
