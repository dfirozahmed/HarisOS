/* HarisOS console selection: see include/console_sel.h. */
#include "console_sel.h"
#include "console.h"
#include "utf8.h"

int con_point_cmp(ConPoint x, ConPoint y)
{
    if (x.row != y.row)
        return x.row < y.row ? -1 : 1;
    return x.col < y.col ? -1 : x.col > y.col ? 1 : 0;
}

static int text_len(const char *t)
{
    int n = 0;
    while (t[n])
        n++;
    return n;
}

/* a column inside 0..len, moved back to the start of the character it is in */
static int snap(const char *t, int len, int i)
{
    if (i < 0)
        i = 0;
    if (i > len)
        i = len;
    while (i > 0 && i < len && utf8_is_cont((unsigned char)t[i]))
        i--;
    return i;
}

static void order(const ConSel *s, ConPoint *lo, ConPoint *hi)
{
    if (con_point_cmp(s->a, s->b) <= 0)
    {
        *lo = s->a;
        *hi = s->b;
    }
    else
    {
        *lo = s->b;
        *hi = s->a;
    }
}

int con_sel_row_span(const ConSel *s, unsigned row, const char *text, int *from, int *to, int *continues)
{
    ConPoint lo, hi;
    if (!s->on)
        return 0;
    order(s, &lo, &hi);
    if (row < lo.row || row > hi.row)
        return 0;
    int len = text_len(text);
    *from = row == lo.row ? snap(text, len, lo.col) : 0;
    *to = row == hi.row ? snap(text, len, hi.col) : len;
    *continues = row < hi.row;
    return 1;
}

void con_sel_word(const char *text, int col, int *from, int *to)
{
    int len = text_len(text);
    col = snap(text, len, col);
    if (col >= len)
    {
        *from = *to = len;
        return;
    }
    int blank = text[col] == ' ';
    int f = col, e = col;
    while (f > 0 && (text[f - 1] == ' ') == blank)
        f--;
    while (e < len && (text[e] == ' ') == blank)
        e++;
    *from = f; /* a space is one byte, so f and e are at characters' starts */
    *to = e;
}

int con_sel_text(int con, const ConSel *s, char *out, int cap)
{
    ConPoint lo, hi;
    int n = 0;
    if (cap <= 0)
        return 0;
    out[0] = 0;
    if (!s->on)
        return 0;
    order(s, &lo, &hi);
    unsigned lines = console_lines(con);
    for (unsigned row = lo.row; row <= hi.row; row++)
    {
        char text[CONSOLE_COLS + 1];
        text[0] = 0;
        if (row <= lines)
            console_row(con, (int)(lines - row), text); /* 0 for a row that has scrolled out: it stays empty */
        int from, to, cont;
        if (con_sel_row_span(s, row, text, &from, &to, &cont))
        {
            if (n + (to - from) + 1 > cap) /* does not fit: stop at the row's start (whole rows only for the rest) */
            {
                int room = cap - 1 - n;
                to = from + (room > 0 ? room : 0);
                to = snap(text, text_len(text), to);
                if (to < from)
                    to = from;
            }
            for (int i = from; i < to; i++)
                out[n++] = text[i];
            if (row < hi.row && n + 1 < cap)
                out[n++] = '\n';
        }
    }
    out[n] = 0;
    return n;
}
