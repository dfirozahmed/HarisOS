/* Host test for console_sel.c: the points and spans of a mouse selection, word selection, and the text that is copied. */
#include "console.h"
#include "console_sel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* the kernel heap and scheduler calls console.c makes, on the host */
void *kmalloc(unsigned long long size) { return calloc(1, size ? (size_t)size : 1); }
void kfree(void *p) { free(p); }
void *krealloc(void *p, unsigned long long size) { return realloc(p, size ? (size_t)size : 1); }
void task_wake(int id) { (void)id; }

static void put(int con, const char *s) { console_write(con, s, (unsigned)strlen(s)); }
static ConPoint pt(unsigned row, int col) { ConPoint p = {row, col}; return p; }
static ConSel sel(ConPoint a, ConPoint b)
{
    ConSel s = {1, a, b};
    return s;
}

int main(void)
{
    /* ---- ordering ---- */
    CHECK(con_point_cmp(pt(1, 5), pt(2, 0)) < 0 && con_point_cmp(pt(2, 0), pt(1, 9)) > 0);
    CHECK(con_point_cmp(pt(3, 4), pt(3, 4)) == 0 && con_point_cmp(pt(3, 4), pt(3, 5)) < 0);

    /* ---- the span of a row ---- */
    {
        ConSel s = sel(pt(5, 3), pt(7, 2));
        int from, to, cont;
        CHECK(!con_sel_row_span(&s, 4, "abcdefgh", &from, &to, &cont) && !con_sel_row_span(&s, 8, "abcdefgh", &from, &to, &cont));
        CHECK(con_sel_row_span(&s, 5, "abcdefgh", &from, &to, &cont) && from == 3 && to == 8 && cont == 1); /* first row: from the press to the end */
        CHECK(con_sel_row_span(&s, 6, "abcdefgh", &from, &to, &cont) && from == 0 && to == 8 && cont == 1); /* a middle row whole */
        CHECK(con_sel_row_span(&s, 6, "", &from, &to, &cont) && from == 0 && to == 0 && cont == 1);          /* an empty row is in it */
        CHECK(con_sel_row_span(&s, 7, "abcdefgh", &from, &to, &cont) && from == 0 && to == 2 && cont == 0); /* last row: up to the end point */
        ConSel back = sel(pt(7, 2), pt(5, 3)); /* dragged upwards: the same selection */
        CHECK(con_sel_row_span(&back, 5, "abcdefgh", &from, &to, &cont) && from == 3 && to == 8);
        CHECK(con_sel_row_span(&back, 7, "abcdefgh", &from, &to, &cont) && to == 2);
        ConSel one = sel(pt(2, 6), pt(2, 1)); /* inside one row, right to left */
        CHECK(con_sel_row_span(&one, 2, "abcdefgh", &from, &to, &cont) && from == 1 && to == 6 && cont == 0);
        ConSel beyond = sel(pt(2, 3), pt(2, 90)); /* a column past the text is the end of the text */
        CHECK(con_sel_row_span(&beyond, 2, "abc", &from, &to, &cont) && from == 3 && to == 3);
        ConSel none = {0, pt(2, 0), pt(2, 5)};
        CHECK(!con_sel_row_span(&none, 2, "abcdefgh", &from, &to, &cont));
        /* a column in the middle of a 3-byte Bengali letter moves back to the letter's start */
        const char *bn = "\xe0\xa6\x95\xe0\xa6\x96\xe0\xa6\x97"; /* ক খ গ */
        ConSel mid = sel(pt(1, 1), pt(1, 7));
        CHECK(con_sel_row_span(&mid, 1, bn, &from, &to, &cont) && from == 0 && to == 6);
    }

    /* ---- the word around a column ---- */
    {
        int f, t;
        con_sel_word("hello big world", 7, &f, &t);
        CHECK(f == 6 && t == 9); /* "big" */
        con_sel_word("hello big world", 0, &f, &t);
        CHECK(f == 0 && t == 5);
        con_sel_word("hello big world", 14, &f, &t);
        CHECK(f == 10 && t == 15);
        con_sel_word("hello   big", 6, &f, &t);
        CHECK(f == 5 && t == 8); /* on blanks: the blanks */
        con_sel_word("abc", 3, &f, &t);
        CHECK(f == 3 && t == 3); /* past the text: nothing */
        con_sel_word("", 0, &f, &t);
        CHECK(f == 0 && t == 0);
        const char *bn = "\xe0\xa6\x86\xe0\xa6\xae\xe0\xa6\xbf \xe0\xa6\xac\xe0\xa6\xbe\xe0\xa6\x82\xe0\xa6\xb2\xe0\xa6\xbe"; /* আমি বাংলা */
        con_sel_word(bn, 11, &f, &t); /* in the middle of a letter of বাংলা */
        CHECK(f == 10 && t == 25); /* the 15 bytes of বাংলা, from the start of the letter the column fell in */
        con_sel_word(bn, 1, &f, &t);
        CHECK(f == 0 && t == 9); /* আমি: bytes 0..8 */
    }

    /* ---- the text copied ---- */
    {
        int con = console_open("SEL");
        put(con, "first line\nsecond line here\nthird\n\nlast");
        unsigned lines = console_lines(con); /* "last" is row 4 */
        CHECK(lines == 4);
        char buf[256];
        ConSel s = sel(pt(0, 6), pt(1, 6)); /* "line" + "\n" + "second" */
        CHECK(con_sel_text(con, &s, buf, sizeof buf) == 11 && strcmp(buf, "line\nsecond") == 0);
        s = sel(pt(1, 6), pt(0, 6)); /* dragged upwards: the same text */
        CHECK(con_sel_text(con, &s, buf, sizeof buf) == 11 && strcmp(buf, "line\nsecond") == 0);
        s = sel(pt(2, 0), pt(4, 2));
        CHECK(con_sel_text(con, &s, buf, sizeof buf) == 9 && strcmp(buf, "third\n\nla") == 0); /* an empty row in between stays */
        s = sel(pt(1, 0), pt(1, 6));
        CHECK(con_sel_text(con, &s, buf, sizeof buf) == 6 && strcmp(buf, "second") == 0);
        /* a selection that is cleared copies nothing; a small buffer is filled to its end, with a 0, whole characters only */
        s.on = 0;
        CHECK(con_sel_text(con, &s, buf, sizeof buf) == 0 && buf[0] == 0);
        s = sel(pt(0, 0), pt(1, 11));
        CHECK(con_sel_text(con, &s, buf, 8) == 7 && buf[7] == 0 && strncmp(buf, "first l", 7) == 0);
        CHECK(con_sel_text(con, &s, buf, 1) == 0 && buf[0] == 0);
        /* rows that scrolled out of the buffer are empty */
        for (int i = 0; i < CONSOLE_ROWS + 5; i++)
            put(con, "\nx");
        s = sel(pt(0, 0), pt(0, 5));
        CHECK(con_sel_text(con, &s, buf, sizeof buf) == 0);
        /* UTF-8: whole characters */
        int u = console_open("BN");
        put(u, "\xe0\xa6\x95\xe0\xa6\x96\xe0\xa6\x97"); /* কখগ */
        s = sel(pt(0, 3), pt(0, 9));
        CHECK(con_sel_text(u, &s, buf, sizeof buf) == 6 && strcmp(buf, "\xe0\xa6\x96\xe0\xa6\x97") == 0);
        CHECK(con_sel_text(u, &s, buf, 5) == 3 && strcmp(buf, "\xe0\xa6\x96") == 0); /* room for one letter + 0 only: not half of the second */
        console_close(u);
        console_close(con);
    }

    if (fails == 0)
        printf("console selection tests passed\n");
    return fails ? 1 : 0;
}
