/* Notepad app: a plain-text editor with selection, undo / redo and New / Open / Save / Save As (Ctrl+N / O / S).
 * Typing, Enter, Tab (4 spaces), Backspace / Delete, arrows, Home / End, PageUp / PageDown, mouse click
 * to place the caret, mouse wheel to scroll. Selection: Shift + movement keys, mouse drag, Ctrl+A; Ctrl+C / X / V
 * (also Ctrl+Insert, Shift+Delete, Shift+Insert) with the shared HarisUI clipboard.
 * The text is one flat buffer; a line ends at '\n'.
 * An app uses only public headers (ui.h, wm.h, desktop.h, gfx.h) - never desktop_internal.h. */
#include "app.h"
#include "desktop.h"
#include "gfx.h"
#include "taskbar.h"
#include "ui.h"
#include "vfs.h"
#include "wm.h"

#define NP_MAX 16384 /* bytes of text */
#define NP_PAD 8
#define NP_TOOL_H 46 /* the row of buttons above the text */
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
static int NP_ANCHOR;  /* the other end of the selection; NP_ANCHOR == NP_CUR means nothing is selected */
static int NP_SELECTING; /* the left button went down in the text and is still held: dragging selects */
static int NP_BLINK = 1;
static char NP_PATH[UI_EDIT_MAX + 1]; /* the file the text came from or was saved to; "" = untitled */
static int NP_DIRTY;                  /* the text has changes that were not saved */
static const char *NP_NOTE = "";      /* a short message at the right of the status bar (a literal) */
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

/* ---- editing and undo ----
 * Every change of the text is one "replace": the `dl` characters at `pos` are replaced by `il` new ones
 * (typing = dl 0, deleting = il 0). An Undo record holds the position, both texts (in one pool) and where the
 * caret / selection was, so undo and redo are the same replace run backwards or forwards. Typing letters in a
 * row, or pressing Backspace / Delete in a row, grows the previous record instead of making a new one, so
 * one Ctrl+Z takes back a whole run. When the records or the pool are full the oldest are forgotten. */
#define NP_UNDO_OPS 256
#define NP_UNDO_POOL 40960 /* worst case for one record: the whole text deleted (16384) + a whole text pasted */
#define NP_RUN_MAX 200     /* a record of a typing run stops growing here */

enum
{
    NP_KIND_NONE,
    NP_KIND_TYPE, /* a letter typed at the caret */
    NP_KIND_BACK, /* Backspace */
    NP_KIND_FWD   /* Delete */
};

typedef struct
{
    int pos, dl, il;           /* replaced dl characters at pos by il characters */
    int off;                   /* start of the deleted text in the pool; the inserted text follows it */
    int cur_before, anchor_before;
} UndoOp;

static UndoOp NP_UNDO[NP_UNDO_OPS];
static char NP_POOL[NP_UNDO_POOL];
static int NP_UN;   /* records in NP_UNDO */
static int NP_UPOS; /* the first NP_UPOS can be undone, the rest redone */
static int NP_LASTKIND; /* kind of the last edit, while nothing else happened (for merging a run) */
static unsigned NP_EDITS; /* counts edits: a key handler can tell whether the key changed the text */

static int pool_used(void)
{
    return NP_UN ? NP_UNDO[NP_UN - 1].off + NP_UNDO[NP_UN - 1].dl + NP_UNDO[NP_UN - 1].il : 0;
}

/* the text becomes: [0,pos) + t[0,il) + the old text from pos+dl on */
static void raw_replace(int pos, int dl, const char *t, int il)
{
    int delta = il - dl;
    if (delta > 0)
        for (int i = NP_LEN - 1; i >= pos + dl; i--)
            NP_BUF[i + delta] = NP_BUF[i];
    else if (delta < 0)
        for (int i = pos + dl; i < NP_LEN; i++)
            NP_BUF[i + delta] = NP_BUF[i];
    for (int i = 0; i < il; i++)
        NP_BUF[pos + i] = t[i];
    NP_LEN += delta;
}

/* forget the oldest record */
static void drop_oldest(void)
{
    int size = NP_UNDO[0].dl + NP_UNDO[0].il;
    int used = pool_used();
    for (int i = 0; i < used - size; i++)
        NP_POOL[i] = NP_POOL[i + size];
    for (int i = 1; i < NP_UN; i++)
    {
        NP_UNDO[i - 1] = NP_UNDO[i];
        NP_UNDO[i - 1].off -= size;
    }
    NP_UN--;
    NP_UPOS--;
}

/* Replace the dl characters at pos by ins[0,il). `kind` says whether this may join the previous record. The
 * caret ends after the new text. */
static void edit_replace(int pos, int dl, const char *ins, int il, int kind)
{
    if (il > NP_MAX - (NP_LEN - dl)) /* the buffer is full: take what fits */
        il = NP_MAX - (NP_LEN - dl);
    if (il < 0)
        il = 0;
    if (dl == 0 && il == 0)
        return;
    int cur_before = NP_CUR, anchor_before = NP_ANCHOR;

    int merged = 0;
    if (kind && kind == NP_LASTKIND && NP_UPOS == NP_UN && NP_UN > 0)
    {
        UndoOp *o = &NP_UNDO[NP_UN - 1];
        int end = pool_used();
        if (o->dl + o->il + dl + il <= NP_RUN_MAX && end + dl + il <= NP_UNDO_POOL)
        {
            if (kind == NP_KIND_TYPE && o->dl == 0 && pos == o->pos + o->il)
            {
                for (int i = 0; i < il; i++)
                    NP_POOL[end + i] = ins[i];
                o->il += il;
                merged = 1;
            }
            else if (kind == NP_KIND_BACK && o->il == 0 && pos + dl == o->pos)
            {
                for (int i = end - 1; i >= o->off; i--) /* the earlier characters go in front */
                    NP_POOL[i + dl] = NP_POOL[i];
                for (int i = 0; i < dl; i++)
                    NP_POOL[o->off + i] = NP_BUF[pos + i];
                o->pos = pos;
                o->dl += dl;
                merged = 1;
            }
            else if (kind == NP_KIND_FWD && o->il == 0 && pos == o->pos)
            {
                for (int i = 0; i < dl; i++)
                    NP_POOL[end + i] = NP_BUF[pos + i];
                o->dl += dl;
                merged = 1;
            }
        }
    }
    if (!merged)
    {
        NP_UN = NP_UPOS; /* a new edit ends the redo history */
        while (NP_UN > 0 && (NP_UN == NP_UNDO_OPS || pool_used() + dl + il > NP_UNDO_POOL))
            drop_oldest();
        UndoOp *o = &NP_UNDO[NP_UN];
        o->pos = pos;
        o->dl = dl;
        o->il = il;
        o->off = pool_used();
        o->cur_before = cur_before;
        o->anchor_before = anchor_before;
        for (int i = 0; i < dl; i++)
            NP_POOL[o->off + i] = NP_BUF[pos + i];
        for (int i = 0; i < il; i++)
            NP_POOL[o->off + dl + i] = ins[i];
        NP_UN++;
        NP_UPOS++;
    }

    raw_replace(pos, dl, ins, il);
    NP_CUR = NP_ANCHOR = pos + il;
    NP_LASTKIND = kind;
    NP_EDITS++;
}

static void undo(void)
{
    if (NP_UPOS == 0)
        return;
    const UndoOp *o = &NP_UNDO[--NP_UPOS];
    raw_replace(o->pos, o->il, NP_POOL + o->off, o->dl);
    NP_CUR = o->cur_before;
    NP_ANCHOR = o->anchor_before;
    NP_LASTKIND = NP_KIND_NONE;
}

static void redo(void)
{
    if (NP_UPOS == NP_UN)
        return;
    const UndoOp *o = &NP_UNDO[NP_UPOS++];
    raw_replace(o->pos, o->dl, NP_POOL + o->off + o->dl, o->il);
    NP_CUR = NP_ANCHOR = o->pos + o->il;
    NP_LASTKIND = NP_KIND_NONE;
}

/* the selection [*a, *b); returns 0 if nothing is selected */
static int sel_range(int *a, int *b)
{
    if (NP_ANCHOR == NP_CUR)
        return 0;
    *a = NP_ANCHOR < NP_CUR ? NP_ANCHOR : NP_CUR;
    *b = NP_ANCHOR < NP_CUR ? NP_CUR : NP_ANCHOR;
    return 1;
}

/* remove the selected text and put the caret where it was; returns 0 if nothing was selected */
static int delete_selection(void)
{
    int a, b;
    if (!sel_range(&a, &b))
        return 0;
    edit_replace(a, b - a, 0, 0, NP_KIND_NONE);
    return 1;
}

/* type s[0,n) at the caret, over the selection if there is one (a single letter can join a typing run) */
static void insert_text(const char *s, int n)
{
    int a, b;
    if (sel_range(&a, &b))
        edit_replace(a, b - a, s, n, NP_KIND_NONE);
    else
        edit_replace(NP_CUR, 0, s, n, (n == 1 && s[0] != '\n') ? NP_KIND_TYPE : NP_KIND_NONE);
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
    return ui_rect(body.x + NP_PAD, body.y + NP_TOOL_H, body.w - 2 * NP_PAD,
                   body.h - NP_TOOL_H - NP_STATUS_H - 6 - NP_PAD / 2);
}

static UiRect status_rect(UiRect body)
{
    return ui_rect(body.x + NP_PAD, body.y + body.h - NP_STATUS_H - 4, body.w - 2 * NP_PAD, NP_STATUS_H);
}

/* Width in pixels of the `count` characters at buffer offset `from`. Measured in pieces of NP_LINE_MAX
 * characters, so a line of any length is handled (the font functions need a 0-terminated copy). */
static int span_w(int from, int count)
{
    char tmp[NP_LINE_MAX + 1];
    int w = 0;
    while (count > 0)
    {
        int n = count < NP_LINE_MAX ? count : NP_LINE_MAX;
        copy_text(from, n, tmp);
        w += gfx_text_w(np_font(), tmp);
        from += n;
        count -= n;
    }
    return w;
}

/* How many characters of the line at `ls` (length `len`) fit completely into `px` pixels from its left edge. */
static int cols_in(int ls, int len, int px)
{
    char tmp[NP_LINE_MAX + 1];
    int col = 0;
    while (col < len && px > 0)
    {
        int n = len - col < NP_LINE_MAX ? len - col : NP_LINE_MAX;
        copy_text(ls + col, n, tmp);
        int fit = gfx_text_fit(np_font(), tmp, px);
        col += fit;
        if (fit < n)
            break;
        px -= gfx_text_w(np_font(), tmp);
    }
    return col;
}

/* x position of the caret inside a line, in pixels from the line's left edge */
static int caret_x(void)
{
    int ls = line_start(NP_CUR);
    return span_w(ls, NP_CUR - ls);
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
    /* Scrolling sideways repaints the whole text area, so it jumps: the caret lands a quarter of the width from
     * the edge it just left, and the next keys inside that room need no scrolling (one repaint per many keys
     * instead of one per key). */
    if (cx < NP_HSCROLL)
    {
        NP_HSCROLL = cx - in.w * 3 / 4;
        if (NP_HSCROLL < 0)
            NP_HSCROLL = 0;
    }
    else if (cx + 2 > NP_HSCROLL + in.w)
        NP_HSCROLL = cx - in.w / 4;
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

    UiRect in = ui_inset(ta, NP_PAD);

    /* The rounded, anti-aliased box is slow to paint. When the redraw is only inside the straight part (a caret
     * blink, typing, scrolling), a plain rectangle gives the same picture. */
    GfxClip cl = gfx_get_clip();
    UiRect safe = ui_inset(ta, t->radius_s + 2);
    if (cl.x0 >= safe.x && cl.y0 >= safe.y && cl.x1 <= safe.x + safe.w && cl.y1 <= safe.y + safe.h)
        gfx_fill_rect(safe.x, safe.y, safe.w, safe.h, ui_color(t->surface_alt));
    else
    {
        gfx_fill_round_rect(ta.x, ta.y, ta.w, ta.h, t->radius_s, ui_color(t->surface_alt));
        gfx_round_rect(ta.x, ta.y, ta.w, ta.h, t->radius_s, 1, ui_color(t->border));
    }

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
    /* a partial redraw (caret blink) only needs the rows it touches: see cl above */
    GfxColor ink = ui_color(t->text);
    int sel_a = 0, sel_b = 0, has_sel = sel_range(&sel_a, &sel_b);
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
            if (has_sel && sel_b > p && sel_a <= e) /* selected part of this line (and its line break) */
            {
                int from = sel_a > p ? sel_a : p, to = sel_b < e ? sel_b : e;
                int hx = in.x - NP_HSCROLL + span_w(p, from - p);
                int hw = span_w(p, to - p) - span_w(p, from - p) + (sel_b > e ? 6 : 0); /* +6: the selected line break */
                if (hw > 0)
                    gfx_fill_rect_alpha(hx, in.y + row * lh, hw, lh, t->accent.r, t->accent.g, t->accent.b, 90);
            }
            /* only the characters that are on screen: skip those left of the scroll position, stop at the right edge */
            char tmp[NP_LINE_MAX + 1];
            int first = cols_in(p, e - p, NP_HSCROLL);
            int x0 = span_w(p, first);
            copy_text(p + first, e - p - first, tmp);
            int shown = gfx_text_fit(f, tmp, in.w + NP_HSCROLL - x0 + 8) + 1; /* +1: a letter cut by the edge */
            if (shown < NP_LINE_MAX)
                tmp[shown] = 0;
            gfx_text(f, in.x - NP_HSCROLL + x0, y, tmp, ink);
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
    if (has_sel)
    {
        put_str(st, &n, "     ");
        put_dec(st, &n, (unsigned)(sel_b - sel_a));
        put_str(st, &n, " selected");
    }
    ui_text_in_color(status_rect(body), st, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    if (NP_NOTE[0])
        ui_text_in_color(status_rect(body), NP_NOTE, UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
}

/* ---- input ---- */

/* put the caret on line `line` at column `col` (clamped to the line) */
static void goto_line_col(int line, int col)
{
    int ls = line_offset(line);
    int len = line_end(ls) - ls;
    NP_CUR = ls + (col < len ? col : len);
}

/* the first and last line the selection (or just the caret) touches */
static void sel_lines(int *lo, int *hi)
{
    int a = line_of(NP_CUR), b = line_of(NP_ANCHOR);
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
}

/* Tell the window manager what really changed (after a key or a click), so it does not redraw the whole
 * window: the status bar always, and either the rows from the first to the last line the caret or the
 * selection touched before and after, or (if the text scrolled or the number of lines changed, so the rows
 * below moved) the whole text area. */
static void mark_changed(Window *w, int old_lo, int old_hi, int old_lines, int old_top, int old_hscroll)
{
    UiRect body = wm_content(w);
    UiRect in = ui_inset(text_area(body), NP_PAD);
    follow(in);
    int new_lo, new_hi;
    sel_lines(&new_lo, &new_hi);
    UiRect rows = in;
    if (NP_TOP == old_top && NP_HSCROLL == old_hscroll && line_count() == old_lines)
    {
        int a = old_lo < new_lo ? old_lo : new_lo;
        int b = old_hi > new_hi ? old_hi : new_hi;
        if (a < NP_TOP)
            a = NP_TOP;
        if (b > NP_TOP + NP_ROWS - 1)
            b = NP_TOP + NP_ROWS - 1;
        rows = ui_rect(in.x, in.y + (a - NP_TOP) * np_line_h(), in.w, (b - a + 1) * np_line_h());
    }
    if (rows.h > 0)
        wm_add_damage(rows);
    wm_add_damage(status_rect(body));
}

static void copy_selection(void)
{
    int a, b;
    if (sel_range(&a, &b))
        ui_clipboard_set(NP_BUF + a, b - a);
}

/* paste the clipboard at the caret over the selection: '\r' and other control characters are dropped, a Tab becomes spaces */
static void paste(void)
{
    static char clip[NP_MAX + 1], text[NP_MAX + 1];
    int n = ui_clipboard_get(clip, (int)sizeof clip);
    int len = 0;
    for (int i = 0; i < n && len < NP_MAX; i++)
    {
        unsigned char c = (unsigned char)clip[i];
        if (c == '\n' || (c >= 32 && c < 127))
            text[len++] = (char)c;
        else if (c == '\t')
            for (int k = 0; k < NP_TAB && len < NP_MAX; k++)
                text[len++] = ' ';
    }
    if (len > 0)
        insert_text(text, len); /* one undo step, whatever its size */
}

/* ---- files: New / Open / Save / Save As ----
 * Text is plain ASCII (the font has no other letters). A file is found by its full path in the virtual file
 * system, e.g. "/HARISOS (nvme0p1)/notes.txt". Only a drive can hold content: the RAM Disk's files are just
 * names. Opening turns Tab into spaces and drops carriage returns (so Windows line endings become '\n'). */

static void copy_str(char *dst, int cap, const char *src)
{
    int n = 0;
    while (src[n] && n < cap - 1)
    {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

static int same_str(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* the file name part of a path ("" for a path that ends with '/') */
static const char *path_name(const char *path)
{
    const char *name = path;
    for (const char *s = path; *s; s++)
        if (*s == '/')
            name = s + 1;
    return name;
}

/* "name* - Notepad": a star while there are changes that were not saved */
static void np_title(Window *w)
{
    char t[WM_TITLE_MAX];
    int n = 0;
    const char *name = NP_PATH[0] ? path_name(NP_PATH) : "Untitled";
    int len = 0;
    while (name[len])
        len++;
    if (len > 16)
        len = 14;
    for (int i = 0; i < len; i++)
        t[n++] = name[i];
    if (name[len]) /* cut: show that there is more */
    {
        t[n++] = '.';
        t[n++] = '.';
    }
    if (NP_DIRTY)
        t[n++] = '*';
    for (const char *s = " - Notepad"; *s; s++)
        t[n++] = *s;
    t[n] = 0;
    if (same_str(t, w->title))
        return;
    copy_str(w->title, WM_TITLE_MAX, t);
    wm_add_damage(ui_rect(w->r.x, w->r.y, w->r.w, ui_theme()->titlebar_h));
}

static void new_document(void)
{
    NP_LEN = NP_CUR = NP_ANCHOR = 0;
    NP_UN = NP_UPOS = 0;
    NP_LASTKIND = NP_KIND_NONE;
    NP_TOP = NP_HSCROLL = 0;
    NP_DESIRED = 0;
    NP_PATH[0] = 0;
    NP_DIRTY = 0;
    NP_FOLLOW = 1;
}

static const char *vfs_problem(void)
{
    if (vfs_last_error() == VFS_ERR_UNSUPPORTED)
        return "Files cannot be stored here (RAM Disk / This PC)";
    return vfs_error_text(vfs_last_error());
}

/* Replace the text with the file at `path`. Returns NULL, or a sentence saying why not (the text is then untouched). */
static const char *open_file(const char *path)
{
    static unsigned char raw[NP_MAX];
    int id = vfs_lookup(path);
    if (id < 0)
        return "File not found";
    const VfsNode *n = vfs_node(id);
    if (n->is_dir)
        return "That is a folder, not a file";
    if (n->size > NP_MAX)
        return "File is too large (the limit is 16384 bytes)";
    unsigned size = n->size, done = 0;
    while (done < size)
    {
        int r = vfs_read(id, done, raw + done, size - done);
        if (r < 0)
            return vfs_problem();
        if (r == 0)
            break;
        done += (unsigned)r;
    }
    int out = 0;
    for (unsigned i = 0; i < done; i++)
    {
        unsigned char c = raw[i];
        if (c == '\t')
            out += NP_TAB;
        else if (c == '\n' || (c >= 32 && c < 127))
            out++;
        else if (c != '\r')
            return "Only plain ASCII text can be opened";
    }
    if (out > NP_MAX)
        return "File is too large (the limit is 16384 bytes)";
    new_document();
    for (unsigned i = 0; i < done; i++)
    {
        unsigned char c = raw[i];
        if (c == '\t')
            for (int k = 0; k < NP_TAB; k++)
                NP_BUF[NP_LEN++] = ' ';
        else if (c != '\r')
            NP_BUF[NP_LEN++] = (char)c;
    }
    copy_str(NP_PATH, (int)sizeof NP_PATH, path);
    return 0;
}

/* Save the text as the file at `path` (an existing file is replaced). NULL, or why not. */
static const char *save_file(const char *path)
{
    const char *name = path_name(path);
    if (path[0] != '/')
        return "The path must start with /";
    if (!name[0])
        return "Type a file name after the folder";
    char folder[UI_EDIT_MAX + 1];
    int flen = (int)(name - path) - 1; /* without the '/' before the name */
    if (flen == 0)
        copy_str(folder, (int)sizeof folder, "/");
    else
    {
        for (int i = 0; i < flen; i++)
            folder[i] = path[i];
        folder[flen] = 0;
    }
    int parent = vfs_lookup(folder);
    if (parent < 0)
        return "Folder not found";
    if (vfs_save(parent, name, NP_BUF, (unsigned)NP_LEN) < 0)
        return vfs_problem();
    copy_str(NP_PATH, (int)sizeof NP_PATH, path);
    NP_DIRTY = 0;
    return 0;
}

/* a first suggestion for the path field: the first drive that holds real files */
static void default_path(char *out, int cap)
{
    copy_str(out, cap, "/");
    for (int i = 0; i < vfs_volume_count(); i++)
    {
        VfsVolInfo vi;
        if (vfs_volume_info(i, &vi) == 0 && vi.writable && !same_str(vi.fs_name, "RAM"))
        {
            int n = 0;
            out[n++] = '/';
            for (const char *s = vfs_node(vi.node)->name; *s && n < cap - 2; s++)
                out[n++] = *s;
            out[n++] = '/';
            out[n] = 0;
            return;
        }
    }
}

/* ---- the Save As / Open dialog: one path field ---- */
enum
{
    NP_DLG_SAVE,
    NP_DLG_OPEN
};
static Window *NP_DLG;
static int NP_DLG_MODE;
static const char *NP_DLG_ERR;               /* what went wrong, shown under the field (a literal) */
static char NP_DLG_WARNED[UI_EDIT_MAX + 1];  /* the path the "file exists" warning was shown for */

static void dlg_draw(Window *w, UiRect body)
{
    const UiTheme *t = ui_theme();
    body = ui_inset(body, t->pad);
    ui_text(body.x, body.y, NP_DLG_MODE == NP_DLG_SAVE ? "Save as" : "Open file", UI_TEXT_TITLE);
    ui_text_color(body.x, body.y + 28, "Type the full path, for example /Drive name/notes.txt", UI_TEXT_SMALL, t->text_dim);
    ui_edit_draw(ui_rect(body.x, body.y + 54, body.w, 34), &w->edits[0], "/", 1);
    if (NP_DLG_ERR)
        ui_text_in_color(ui_rect(body.x, body.y + 94, body.w, 22), NP_DLG_ERR, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->danger);

    ui_text_color(body.x, body.y + 124, "Drives", UI_TEXT_SMALL, t->text_dim);
    for (int i = 0; i < vfs_volume_count() && i < 4; i++)
    {
        VfsVolInfo vi;
        if (vfs_volume_info(i, &vi) != 0)
            continue;
        char line[VFS_NAME_MAX + 40];
        int n = 0;
        line[n++] = '/';
        for (const char *s = vfs_node(vi.node)->name; *s && n < VFS_NAME_MAX; s++)
            line[n++] = *s;
        if (same_str(vi.fs_name, "RAM"))
            for (const char *s = "   (no file content)"; *s; s++)
                line[n++] = *s;
        else if (!vi.writable || vi.locked)
            for (const char *s = "   (read only)"; *s; s++)
                line[n++] = *s;
        line[n] = 0;
        ui_text_in_color(ui_rect(body.x, body.y + 146 + i * 20, body.w, 20), line, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text);
    }
}

static void np_after_change(void)
{
    if (NP_WIN)
    {
        np_title(NP_WIN);
        wm_add_damage(NP_WIN->r); /* the text was replaced or the file name changed */
    }
}

static void dlg_ok(void)
{
    if (!NP_DLG)
        return;
    const char *path = NP_DLG->edits[0].text;
    const char *err;
    if (!path[0])
    {
        NP_DLG_ERR = "Type a path first";
        return;
    }
    if (NP_DLG_MODE == NP_DLG_SAVE)
    {
        int id = vfs_lookup(path);
        if (id >= 0 && vfs_node(id)->is_dir)
        {
            NP_DLG_ERR = "That is a folder: add a file name";
            return;
        }
        if (id >= 0 && !same_str(path, NP_PATH) && !same_str(path, NP_DLG_WARNED)) /* asked once before replacing another file */
        {
            copy_str(NP_DLG_WARNED, (int)sizeof NP_DLG_WARNED, path);
            NP_DLG_ERR = "This file exists. Press Save again to replace it";
            return;
        }
        err = save_file(path);
    }
    else
        err = open_file(path);
    if (err)
    {
        NP_DLG_ERR = err;
        return;
    }
    NP_NOTE = NP_DLG_MODE == NP_DLG_SAVE ? "Saved" : "Opened";
    wm_close(NP_DLG);
    np_after_change();
}

static void on_dlg_ok(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    dlg_ok();
}

static void on_dlg_cancel(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    if (NP_DLG)
        wm_close(NP_DLG);
}

static void dlg_key(Window *w, const KeyEvent *k)
{
    if (k->pressed && k->key == KEY_ENTER)
        dlg_ok();
    else if (k->pressed && k->key == KEY_ESC)
        wm_close(w);
    else if (ui_edit_key(&w->edits[0], k))
    {
        NP_DLG_ERR = 0; /* the path changed: the old complaint no longer applies */
        ui_caret_restart();
    }
}

static int dlg_press(Window *w, int x, int y)
{
    UiRect f = w->edits[0].field;
    if (f.w > 0 && ui_contains(f, x, y))
    {
        ui_edit_mouse(&w->edits[0], x, y, 1);
        ui_caret_restart();
        return 1;
    }
    return 0;
}

static int dlg_drag(Window *w, int x, int y, unsigned buttons)
{
    if (!w->edits[0].dragging || !ui_edit_mouse(&w->edits[0], x, y, buttons))
        return 0;
    ui_caret_restart();
    wm_add_damage(ui_inset(w->edits[0].field, -3));
    return 2;
}

static void dlg_close(Window *w)
{
    (void)w;
    NP_DLG = 0;
}

/* put a dialog of w x h over the middle of the Notepad window */
static Window *open_dialog_window(const char *title, int w, int h, WmDrawFn draw)
{
    int W = (int)gfx_width(), H = (int)gfx_height();
    int x = NP_WIN->r.x + (NP_WIN->r.w - w) / 2, y = NP_WIN->r.y + (NP_WIN->r.h - h) / 2;
    if (x + w > W)
        x = W - w;
    if (y + h > H - TASKBAR_HEIGHT)
        y = H - TASKBAR_HEIGHT - h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    Window *win = wm_create_dialog(NP_WIN, title, x, y, w, h, draw, 0);
    if (!win)
        NP_NOTE = "Window limit reached - close one first";
    return win;
}

static void open_path_dialog(int mode)
{
    if (!NP_WIN)
        return;
    if (NP_DLG)
    {
        wm_activate(NP_DLG);
        return;
    }
    int w = 520, h = 340;
    Window *win = open_dialog_window(mode == NP_DLG_SAVE ? "Save As" : "Open", w, h, dlg_draw);
    if (!win)
        return;
    NP_DLG = win;
    NP_DLG_MODE = mode;
    NP_DLG_ERR = 0;
    NP_DLG_WARNED[0] = 0;
    win->on_key = dlg_key;
    win->on_press = dlg_press;
    win->on_drag = dlg_drag;
    win->on_close = dlg_close;
    win->edit_count = 1;
    win->focus = 0;
    win->caret = 1;
    char start[UI_EDIT_MAX + 1];
    if (NP_PATH[0])
        copy_str(start, (int)sizeof start, NP_PATH);
    else
        default_path(start, (int)sizeof start);
    ui_edit_set_text(&win->edits[0], start);
    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    int anchor = WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM;
    wm_add_button_ex(win, "Cancel", UI_BTN_SECONDARY, ui_rect(cw - 20 - 100 - 10 - 100, ch - 20 - 34, 100, 34), anchor,
                     on_dlg_cancel, 0);
    wm_add_button_ex(win, mode == NP_DLG_SAVE ? "Save" : "Open", UI_BTN_PRIMARY,
                     ui_rect(cw - 20 - 100, ch - 20 - 34, 100, 34), anchor, on_dlg_ok, 0);
}

/* ---- "discard unsaved changes?" ---- */
enum
{
    NP_ACT_NEW,
    NP_ACT_OPEN
};
static Window *NP_CONF;
static int NP_PENDING;

static void conf_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    body = ui_inset(body, t->pad);
    ui_text(body.x, body.y, "Discard unsaved changes?", UI_TEXT_TITLE);
    ui_text_color(body.x, body.y + 32, "The text has changes that were not saved.", UI_TEXT_BODY, t->text_dim);
}

static void conf_close(Window *w)
{
    (void)w;
    NP_CONF = 0;
}

static void on_conf_no(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    if (NP_CONF)
        wm_close(NP_CONF);
}

static void on_conf_yes(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    if (NP_CONF)
        wm_close(NP_CONF);
    if (!NP_WIN)
        return;
    if (NP_PENDING == NP_ACT_NEW)
    {
        new_document();
        NP_NOTE = "";
        np_after_change();
    }
    else
    {
        NP_DIRTY = 0; /* the question was answered: Open does not ask again */
        open_path_dialog(NP_DLG_OPEN);
    }
}

static void conf_key(Window *w, const KeyEvent *k)
{
    if (k->pressed && k->key == KEY_ESC)
        wm_close(w); /* no default answer on Enter: discarding must be a deliberate click */
}

static void ask_discard(int action)
{
    if (NP_CONF)
    {
        wm_activate(NP_CONF);
        return;
    }
    int w = 420, h = 190;
    Window *win = open_dialog_window("Notepad", w, h, conf_draw);
    if (!win)
        return;
    NP_CONF = win;
    NP_PENDING = action;
    win->on_key = conf_key;
    win->on_close = conf_close;
    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    int anchor = WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM;
    wm_add_button_ex(win, "Cancel", UI_BTN_SECONDARY, ui_rect(cw - 20 - 100 - 10 - 100, ch - 20 - 34, 100, 34), anchor,
                     on_conf_no, 0);
    wm_add_button_ex(win, "Discard", UI_BTN_DANGER, ui_rect(cw - 20 - 100, ch - 20 - 34, 100, 34), anchor, on_conf_yes, 0);
}

/* ---- the commands (toolbar buttons and Ctrl+N / O / S) ---- */
static void cmd_new(void)
{
    if (NP_DIRTY)
        ask_discard(NP_ACT_NEW);
    else
    {
        new_document();
        NP_NOTE = "";
        np_after_change();
    }
}

static void cmd_open(void)
{
    if (NP_DIRTY)
        ask_discard(NP_ACT_OPEN);
    else
        open_path_dialog(NP_DLG_OPEN);
}

static void cmd_save(void)
{
    if (!NP_PATH[0])
    {
        open_path_dialog(NP_DLG_SAVE);
        return;
    }
    const char *err = save_file(NP_PATH);
    NP_NOTE = err ? err : "Saved";
    np_after_change();
}

static void cmd_save_as(void)
{
    open_path_dialog(NP_DLG_SAVE);
}

static void on_new(UiButton *b, void *user) { (void)b; (void)user; cmd_new(); }
static void on_open(UiButton *b, void *user) { (void)b; (void)user; cmd_open(); }
static void on_save(UiButton *b, void *user) { (void)b; (void)user; cmd_save(); }
static void on_save_as(UiButton *b, void *user) { (void)b; (void)user; cmd_save_as(); }

static void np_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
    {
        wm_key_partial(); /* a release changes nothing: no redraw at all */
        return;
    }
    NP_NOTE = ""; /* a message stays until the next key */
    unsigned edits_before = NP_EDITS;
    int upos_before = NP_UPOS;
    int full = 0; /* the key opened a dialog or replaced the text: redraw the whole window */
    int vertical = 0;
    int motion = 0; /* a key that only moves the caret: with Shift it extends the selection */
    int keep_anchor = 0;
    int shift = (k->mods & KMOD_SHIFT) != 0, ctrl = (k->mods & KMOD_CTRL) != 0;
    int old_lines = line_count(), old_lo, old_hi;
    sel_lines(&old_lo, &old_hi);
    int old_top = NP_TOP, old_hscroll = NP_HSCROLL;
    int sa = 0, sb = 0, has_sel = sel_range(&sa, &sb);

    if (ctrl)
    {
        switch (k->key)
        {
        case 'a':
            NP_ANCHOR = 0;
            NP_CUR = NP_LEN;
            keep_anchor = 1;
            break;
        case 'c':
        case KEY_INSERT:
            copy_selection();
            break;
        case 'x':
            copy_selection();
            delete_selection();
            break;
        case 'v':
            paste();
            break;
        case 'z':
            if (shift)
                redo();
            else
                undo();
            keep_anchor = 1; /* undo puts the old selection back */
            old_lines = -1;  /* it may have changed lines far from the caret: redraw the whole text area */
            break;
        case 'y':
            redo();
            keep_anchor = 1;
            old_lines = -1;
            break;
        case 'n':
            cmd_new();
            full = 1;
            break;
        case 'o':
            cmd_open();
            full = 1;
            break;
        case 's':
            if (shift)
                cmd_save_as();
            else
                cmd_save();
            full = 1;
            break;
        default:
            break;
        }
    }
    else
    {
        switch (k->key)
        {
        case KEY_LEFT:
            motion = 1;
            if (has_sel && !shift)
                NP_CUR = sa; /* the selection collapses to its start */
            else if (NP_CUR > 0)
                NP_CUR--;
            break;
        case KEY_RIGHT:
            motion = 1;
            if (has_sel && !shift)
                NP_CUR = sb;
            else if (NP_CUR < NP_LEN)
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
            motion = 1;
            break;
        }
        case KEY_HOME:
            NP_CUR = line_start(NP_CUR);
            motion = 1;
            break;
        case KEY_END:
            NP_CUR = line_end(NP_CUR);
            motion = 1;
            break;
        case KEY_BACKSPACE:
            if (!delete_selection() && NP_CUR > 0)
                edit_replace(NP_CUR - 1, 1, 0, 0, NP_KIND_BACK);
            break;
        case KEY_DELETE:
            if (shift) /* Shift+Delete = cut */
            {
                copy_selection();
                delete_selection();
            }
            else if (!delete_selection() && NP_CUR < NP_LEN)
                edit_replace(NP_CUR, 1, 0, 0, NP_KIND_FWD);
            break;
        case KEY_INSERT:
            if (shift) /* Shift+Insert = paste */
                paste();
            break;
        case KEY_ENTER:
            insert_text("\n", 1);
            break;
        case KEY_TAB:
            insert_text("    ", NP_TAB);
            break;
        default:
            if (k->ch >= 32 && k->ch < 127 && !(k->mods & KMOD_ALT))
            {
                char c = (char)k->ch;
                insert_text(&c, 1);
            }
            break;
        }
    }
    if (NP_EDITS == edits_before)
        NP_LASTKIND = NP_KIND_NONE; /* a key that did not edit (movement, copy ...) ends a typing run */

    if (!(motion && shift) && !keep_anchor)
        NP_ANCHOR = NP_CUR; /* no selection, unless Shift extended it */
    if (!vertical)
        NP_DESIRED = NP_CUR - line_start(NP_CUR);
    NP_FOLLOW = 1;
    ui_caret_restart();
    NP_BLINK = 1;

    if (NP_EDITS != edits_before || NP_UPOS != upos_before)
        NP_DIRTY = 1; /* undo / redo count too: whether the text is back to the saved state is not tracked */
    np_title(w);      /* the star in the title bar (damages only the title bar) */

    mark_changed(w, old_lo, old_hi, old_lines, old_top, old_hscroll);
    if (!full)
        wm_key_partial();
}

/* buffer offset of the character nearest to the screen point (x, y); a point outside the text area counts as
 * the nearest row / column */
static int pos_at(UiRect in, int x, int y)
{
    int dy = y - in.y, lh = np_line_h();
    int row = dy >= 0 ? dy / lh : -((-dy + lh - 1) / lh);
    int line = NP_TOP + row;
    if (line > line_count() - 1)
        line = line_count() - 1;
    if (line < 0)
        line = 0;
    int ls = line_offset(line);
    return ls + cols_in(ls, line_end(ls) - ls, x - in.x + NP_HSCROLL + 3); /* +3: nearer half of a letter wins */
}

static int np_press(Window *w, int x, int y)
{
    UiRect body = wm_content(w);
    UiRect in = ui_inset(text_area(body), NP_PAD);
    if (!ui_contains(in, x, y))
        return 0;

    int old_lo, old_hi;
    sel_lines(&old_lo, &old_hi);
    int old_lines = line_count();
    int old_top = NP_TOP, old_hscroll = NP_HSCROLL;
    NP_CUR = NP_ANCHOR = pos_at(in, x, y);
    NP_SELECTING = 1; /* dragging from here selects */
    NP_LASTKIND = NP_KIND_NONE; /* clicking ends a typing run */
    NP_DESIRED = NP_CUR - line_start(NP_CUR);
    NP_FOLLOW = 1;
    ui_caret_restart();
    NP_BLINK = 1;
    mark_changed(w, old_lo, old_hi, old_lines, old_top, old_hscroll);
    return 2; /* the damage is already marked: only the touched rows and the status bar are redrawn */
}

static int np_drag(Window *w, int x, int y, unsigned buttons)
{
    if (!(buttons & 1))
    {
        NP_SELECTING = 0;
        return 0;
    }
    if (!NP_SELECTING)
        return 0;
    UiRect in = ui_inset(text_area(wm_content(w)), NP_PAD);
    int pos = pos_at(in, x, y);
    if (pos == NP_CUR)
        return 0;
    int old_lo, old_hi;
    sel_lines(&old_lo, &old_hi);
    int old_lines = line_count();
    int old_top = NP_TOP, old_hscroll = NP_HSCROLL;
    NP_CUR = pos; /* the anchor stays where the press was */
    NP_DESIRED = NP_CUR - line_start(NP_CUR);
    NP_FOLLOW = 1;
    mark_changed(w, old_lo, old_hi, old_lines, old_top, old_hscroll);
    return 2;
}

static int np_wheel(Window *w, int steps)
{
    int top = NP_TOP + steps * 3;
    if (top > line_count() - 1)
        top = line_count() - 1;
    if (top < 0)
        top = 0;
    if (top == NP_TOP)
        return 0;
    NP_TOP = top;
    wm_add_damage(ui_inset(text_area(wm_content(w)), NP_PAD)); /* only the text moved, not the frame or the status bar */
    return 2;
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
    NP_SELECTING = 0;
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
    win->on_drag = np_drag;
    win->on_wheel = np_wheel;
    win->on_tick = np_tick;
    win->on_close = np_close;
    win->icon = TASKBAR_ICON_NOTEPAD;
    wm_set_min_size(win, 330, 230);
    NP_WIN = win;
    desktop_set_status(0);
    NP_FOLLOW = 1;
    np_title(win);

    wm_add_button(win, "New", UI_BTN_SECONDARY, ui_rect(NP_PAD, 8, 64, 30), on_new, 0);
    wm_add_button(win, "Open", UI_BTN_SECONDARY, ui_rect(NP_PAD + 72, 8, 64, 30), on_open, 0);
    wm_add_button(win, "Save", UI_BTN_SECONDARY, ui_rect(NP_PAD + 144, 8, 64, 30), on_save, 0);
    wm_add_button(win, "Save As", UI_BTN_SECONDARY, ui_rect(NP_PAD + 216, 8, 84, 30), on_save_as, 0);
}

const AppInfo APP_NOTEPAD = {
    .name = "Notepad",
    .desc = "Write plain text",
    .icon = UI_ICON_NOTEPAD,
    .open = notepad_app_open,
};
