/* Notepad app: a plain-text editor with selection, undo / redo and New / Open / Save / Save As (Ctrl+N / O / S).
 * Typing, Enter, Tab (4 spaces), Backspace / Delete, arrows, Home / End, PageUp / PageDown, mouse click
 * to place the caret, mouse wheel to scroll. Selection: Shift + movement keys, mouse drag, Ctrl+A; Ctrl+C / X / V
 * (also Ctrl+Insert, Shift+Delete, Shift+Insert) with the shared HarisUI clipboard.
 * Find and replace: Ctrl+F / Ctrl+H or the Find button open a bar with a Find and a Replace field (Enter = next,
 * Shift+Enter = previous, F3 / Shift+F3 from anywhere, "Aa" = match case, All = one undo step).
 * Word wrap: the Wrap button or Alt+Z. What is shown is a list of rows: lines, or pieces of a line with wrap (Row).
 * The text is one flat buffer in a block of the kernel heap (heap.h) that grows with it, up to 16 MB (a short text
 * still works if the heap has no memory). A line ends at '\n'.
 * An app uses only public headers (ui.h, wm.h, desktop.h, gfx.h) - never desktop_internal.h. */
#include "app.h"
#include "desktop.h"
#include "gfx.h"
#include "heap.h"
#include "notepad.h"
#include "ntfs.h"
#include "taskbar.h"
#include "timer.h"
#include "ui.h"
#include "vfs.h"
#include "wm.h"

#define NP_HARD_MAX (16 * 1024 * 1024) /* the most text there can be, in bytes */
#define NP_UNDO_MAX (4 * 1024 * 1024) /* the most the undo history may hold, in bytes */
#define NP_PAD 8
#define NP_TOOL_H 46 /* the row of buttons above the text */
#define NP_BAR_H 80  /* the find bar (two rows of fields) when it is shown */
#define NP_STATUS_H 22
#define NP_LINE_MAX 511 /* characters of one line that are measured / drawn */
#define NP_TAB 4

static Window *NP_WIN;
static char NP_EMPTY[1];
static char NP_FALLBACK[4096]; /* the text of a short document if the heap has no memory at all (no page allocator) */
static char *NP_BUF = NP_EMPTY; /* the text: a block of the kernel heap (NP_EMPTY while there is none) */
static int NP_CAP;               /* the size of that block */
static int NP_LIMIT = NP_HARD_MAX; /* the text may grow up to this (the tests lower it) */
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
static int NP_WRAP;  /* word wrap is on */
static int NP_WRAPW; /* the width in pixels the rows are wrapped to (see set_geometry) */
static int NP_WRAP_BTN; /* index of the "Wrap" toolbar button (it shows the state by its color) */
static int NP_BAR;        /* the find bar is shown */
static int NP_FOCUS;      /* where the keyboard is: 0 = the text, 1 = the Find field, 2 = the Replace field */
static int NP_BAR_DRAG;   /* the left button went down in a field of the bar: dragging selects there */
static int NP_MATCH_CASE; /* "Aa": upper and lower case count as different */
static int NP_FIND_BTN;   /* index of the "Find" toolbar button */
static UiEdit NP_FIND, NP_REPL;
static char NP_NOTE_BUF[48]; /* text for NP_NOTE that is made at run time ("3 of 7") */
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
static char *NP_POOL;  /* the texts of the records, in a heap block that grows (at most NP_UNDO_MAX) */
static int NP_POOL_CAP;
static int NP_UN;   /* records in NP_UNDO */
static int NP_UPOS; /* the first NP_UPOS can be undone, the rest redone */
static int NP_LASTKIND; /* kind of the last edit, while nothing else happened (for merging a run) */
static unsigned NP_EDITS; /* counts edits: a key handler can tell whether the key changed the text */

/* ---- the text buffer and the undo pool: blocks of the kernel heap (heap.h) that grow as needed ---- */
static int buf_on_heap(void)
{
    return NP_BUF != NP_EMPTY && NP_BUF != NP_FALLBACK;
}

static void buf_release(void)
{
    if (buf_on_heap())
        kfree(NP_BUF);
    NP_BUF = NP_EMPTY;
    NP_CAP = 0;
}

/* Room for `need` bytes of text (the block doubles when it is full). 0 if that is over NP_LIMIT or the heap has no
 * memory; nothing changes then. */
static int buf_reserve(int need)
{
    if (need <= NP_CAP)
        return 1;
    if (need > NP_LIMIT)
        return 0;
    long long cap = NP_CAP < 4096 ? 4096 : NP_CAP;
    while (cap < need)
        cap *= 2;
    if (cap > NP_LIMIT)
        cap = NP_LIMIT;
    for (;;) /* the doubled size, and if the heap cannot give that, just what is needed */
    {
        char *nb = buf_on_heap() ? krealloc(NP_BUF, (unsigned long long)cap) : kmalloc((unsigned long long)cap);
        if (nb)
        {
            if (!buf_on_heap()) /* leaving the fallback block (or the empty one): take the text along */
                for (int i = 0; i < NP_LEN; i++)
                    nb[i] = NP_BUF[i];
            NP_BUF = nb;
            NP_CAP = (int)cap;
            return 1;
        }
        if (cap == need)
            break;
        cap = need;
    }
    if (NP_BUF == NP_EMPTY && need <= (int)sizeof NP_FALLBACK) /* no heap memory: a short text still has a place */
    {
        NP_BUF = NP_FALLBACK;
        NP_CAP = (int)sizeof NP_FALLBACK;
        return 1;
    }
    return 0;
}

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

/* Room for `extra` more bytes at the end of the pool (it grows like the text buffer). The oldest records are forgotten
 * while the history would be over NP_UNDO_MAX, has too many records, or the heap cannot give the room. 0 only if
 * even an empty history is too small. */
static int pool_reserve(int extra)
{
    for (;;)
    {
        int need = pool_used() + extra;
        if (NP_UN == NP_UNDO_OPS || need > NP_UNDO_MAX)
        {
            if (NP_UN == 0)
                return 0;
            drop_oldest();
            continue;
        }
        if (need <= NP_POOL_CAP)
            return 1;
        long long cap = NP_POOL_CAP < 4096 ? 4096 : NP_POOL_CAP;
        while (cap < need)
            cap *= 2;
        if (cap > NP_UNDO_MAX)
            cap = NP_UNDO_MAX;
        char *np = 0;
        for (;;)
        {
            np = NP_POOL ? krealloc(NP_POOL, (unsigned long long)cap) : kmalloc((unsigned long long)cap);
            if (np || cap == need)
                break;
            cap = need;
        }
        if (np)
        {
            NP_POOL = np;
            NP_POOL_CAP = (int)cap;
            return 1;
        }
        if (NP_UN == 0)
            return 0;
        drop_oldest(); /* the heap is short of memory: forget history rather than fail */
    }
}

/* Replace the dl characters at pos by ins[0,il). `kind` says whether this may join the previous record. The
 * caret ends after the new text. */
static void edit_replace(int pos, int dl, const char *ins, int il, int kind)
{
    int wanted = il;
    if (il > NP_LIMIT - (NP_LEN - dl)) /* the text is at its limit: take what fits */
        il = NP_LIMIT - (NP_LEN - dl);
    if (il < 0)
        il = 0;
    if (!buf_reserve(NP_LEN - dl + il)) /* the heap has no more memory: take what fits in the block there is */
    {
        il = NP_CAP - (NP_LEN - dl);
        if (il < 0)
            il = 0;
    }
    int cut = il < wanted; /* not everything fits: said at the end, so that no other message hides it */
    if (dl == 0 && il == 0)
    {
        if (cut)
            NP_NOTE = "The text is full: not everything fit";
        return;
    }
    int cur_before = NP_CUR, anchor_before = NP_ANCHOR;

    int merged = 0;
    if (kind && kind == NP_LASTKIND && NP_UPOS == NP_UN && NP_UN > 0)
    {
        UndoOp *o = &NP_UNDO[NP_UN - 1];
        int end = pool_used();
        if (o->dl + o->il + dl + il <= NP_RUN_MAX && end + dl + il <= NP_POOL_CAP)
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
        if (!pool_reserve(dl + il))
        {
            /* too big for the history: the edit is made without a record, and the history is forgotten (its records
             * describe a text that this change makes different) */
            NP_UN = NP_UPOS = 0;
            NP_NOTE = "Too large to undo";
        }
        else
        {
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
    }

    raw_replace(pos, dl, ins, il);
    NP_CUR = NP_ANCHOR = pos + il;
    NP_LASTKIND = kind;
    NP_EDITS++;
    if (cut)
        NP_NOTE = "The text is full: not everything fit";
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
    int top = NP_TOOL_H + (NP_BAR ? NP_BAR_H : 0);
    return ui_rect(body.x + NP_PAD, body.y + top, body.w - 2 * NP_PAD, body.h - top - NP_STATUS_H - 6 - NP_PAD / 2);
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

/* ---- rows ----
 * The text is shown in rows. Without word wrap a row is a line (up to the '\n'). With it a line is cut into rows no
 * wider than NP_WRAPW pixels: after the last space that fits, or inside a word that is too long for a row. A row is
 * the characters [start, end) of the buffer; `lend` is where its line ends. A row that is not the last of its line
 * ends where the next one starts (a "soft" break); the caret cannot be shown at that offset on this row (it is
 * the start of the next one), so the last caret position of such a row is end - 1. */
typedef struct
{
    int start, end, lend;
} Row;

static int seg_end(int s, int lend)
{
    if (!NP_WRAP || NP_WRAPW < 16)
        return lend;
    int n = cols_in(s, lend - s, NP_WRAPW);
    if (s + n >= lend)
        return lend;
    if (n < 1)
        n = 1; /* a letter wider than the row still gets a row of its own */
    if (NP_BUF[s + n] == ' ')
        return s + n + 1; /* the word ends exactly at the edge: the space hangs over it */
    for (int i = s + n - 1; i > s; i--)
        if (NP_BUF[i] == ' ')
            return i + 1;
    return s + n; /* no space: break inside the word */
}

static void row_set(Row *r, int s, int lend)
{
    r->start = s;
    r->lend = lend;
    r->end = seg_end(s, lend);
}

static void row_first(Row *r)
{
    row_set(r, 0, line_end(0));
}

/* step to the next row; 0 (and *r unchanged) if r is the last one */
static int row_next(Row *r)
{
    if (r->end < r->lend)
    {
        row_set(r, r->end, r->lend);
        return 1;
    }
    if (r->end >= NP_LEN)
        return 0;
    int s = r->end + 1;
    row_set(r, s, line_end(s));
    return 1;
}

static int row_soft(const Row *r)
{
    return r->end < r->lend;
}

/* the last offset the caret can have on this row */
static int row_last_pos(const Row *r)
{
    return r->end - (row_soft(r) ? 1 : 0);
}

/* the index of the row the caret at offset pos is on (and the row in *r) */
static int row_find(int pos, Row *r)
{
    int i = 0;
    row_first(r);
    for (;;)
    {
        if (pos < r->end || (pos == r->end && !row_soft(r)))
            return i;
        if (!row_next(r))
            return i;
        i++;
    }
}

/* row number `index` into *r; past the end it gives the last row. Returns the number of the row it gave. */
static int row_at(int index, Row *r)
{
    int i = 0;
    row_first(r);
    while (i < index && row_next(r))
        i++;
    return i;
}

static int row_count(void)
{
    Row r;
    int n = 1;
    row_first(&r);
    while (row_next(&r))
        n++;
    return n;
}

/* the width the rows are wrapped to: the text area, a little less for the caret (set before any row is used) */
static void set_geometry(UiRect body)
{
    UiRect in = ui_inset(text_area(body), NP_PAD);
    NP_WRAPW = in.w - 4;
    if (NP_WRAPW < 24)
        NP_WRAPW = 24;
}

/* x position of the caret inside its row, in pixels from the row's left edge */
static int caret_x(void)
{
    Row r;
    row_find(NP_CUR, &r);
    return span_w(r.start, NP_CUR - r.start);
}

/* the caret's column in its row (what Up / Down try to keep) */
static int col_in_row(void)
{
    Row r;
    row_find(NP_CUR, &r);
    return NP_CUR - r.start;
}

/* scroll so that the caret is visible (when NP_FOLLOW is set); `in` is the text area inside its padding */
static void follow(UiRect in)
{
    if (!NP_FOLLOW)
        return;
    NP_ROWS = in.h / np_line_h();
    if (NP_ROWS < 1)
        NP_ROWS = 1;
    Row r;
    int cur_row = row_find(NP_CUR, &r);
    if (cur_row < NP_TOP)
        NP_TOP = cur_row;
    if (cur_row >= NP_TOP + NP_ROWS)
        NP_TOP = cur_row - NP_ROWS + 1;
    if (NP_WRAP)
    {
        NP_HSCROLL = 0; /* wrapped text never needs scrolling sideways */
        NP_FOLLOW = 0;
        return;
    }
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

/* ---- the find bar: a Find and a Replace field with their buttons, under the toolbar ---- */
typedef struct
{
    UiRect bar, find_f, repl_f, prev, next, mcase, close, repl, all;
} BarLayout;

static BarLayout bar_layout(UiRect body)
{
    BarLayout l;
    l.bar = ui_rect(body.x + NP_PAD, body.y + NP_TOOL_H, body.w - 2 * NP_PAD, NP_BAR_H);
    int right = l.bar.x + l.bar.w;
    l.close = ui_rect(right - 34, l.bar.y, 34, 34);
    l.mcase = ui_rect(l.close.x - 8 - 44, l.bar.y, 44, 34);
    l.next = ui_rect(l.mcase.x - 8 - 60, l.bar.y, 60, 34);
    l.prev = ui_rect(l.next.x - 8 - 60, l.bar.y, 60, 34);
    int fw = l.prev.x - 8 - l.bar.x;
    if (fw < 60)
        fw = 60;
    int y2 = l.bar.y + 40;
    l.find_f = ui_rect(l.bar.x, l.bar.y, fw, 34);
    l.repl_f = ui_rect(l.bar.x, y2, fw, 34);
    l.repl = ui_rect(l.prev.x, y2, 82, 34);
    l.all = ui_rect(l.repl.x + 82 + 8, y2, 52, 34);
    return l;
}

static void bar_draw(UiRect body)
{
    BarLayout l = bar_layout(body);
    ui_edit_draw(l.find_f, &NP_FIND, "Find", NP_FOCUS == 1);
    ui_edit_draw(l.repl_f, &NP_REPL, "Replace with", NP_FOCUS == 2);
    ui_button(l.prev, "Prev", UI_BTN_SECONDARY, UI_STATE_NORMAL);
    ui_button(l.next, "Next", UI_BTN_SECONDARY, UI_STATE_NORMAL);
    ui_button(l.mcase, "Aa", NP_MATCH_CASE ? UI_BTN_PRIMARY : UI_BTN_SECONDARY, UI_STATE_NORMAL); /* match case */
    ui_button_icon(l.close, "", UI_ICON_CLOSE, UI_BTN_GHOST, UI_STATE_NORMAL);
    ui_button(l.repl, "Replace", UI_BTN_SECONDARY, UI_STATE_NORMAL);
    ui_button(l.all, "All", UI_BTN_SECONDARY, UI_STATE_NORMAL);
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

    set_geometry(body);
    int total = row_count();
    Row cur;
    int cur_row = row_find(NP_CUR, &cur);
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
    Row rw;
    row_at(NP_TOP, &rw);
    NP_CARET = ui_rect(0, 0, 0, 0);
    for (int row = 0; row < NP_ROWS + 1; row++)
    {
        int p = rw.start, e = rw.end;
        int y = in.y + row * lh + 2;
        int is_cur = NP_TOP + row == cur_row;
        if (is_cur)
            NP_CARET = ui_rect(in.x - NP_HSCROLL + caret_x(), y, 2, lh - 4);
        if (y + lh >= cl.y0 && y <= cl.y1)
        {
            if (has_sel && sel_b > p && sel_a <= e && !(row_soft(&rw) && sel_a == e)) /* selected part of this row (and its line break) */
            {
                int from = sel_a > p ? sel_a : p, to = sel_b < e ? sel_b : e;
                int ext = (sel_b > e && !row_soft(&rw)) ? 6 : 0; /* +6: the selected line break */
                int hx = in.x - NP_HSCROLL + span_w(p, from - p);
                int hw = span_w(p, to - p) - span_w(p, from - p) + ext;
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
            if (is_cur && NP_BLINK && NP_FOCUS == 0)
                gfx_fill_rect(NP_CARET.x, NP_CARET.y, NP_CARET.w, NP_CARET.h, ui_color(t->accent));
        }
        if (!row_next(&rw))
            break;
    }
    gfx_restore_clip(saved);
    if (NP_BAR)
        bar_draw(body);

    /* status bar: line, column and how full the buffer is */
    char st[64];
    int n = 0;
    put_str(st, &n, "Ln ");
    put_dec(st, &n, (unsigned)line_of(NP_CUR) + 1); /* the line of the text, not the row it is shown in */
    put_str(st, &n, ", Col ");
    put_dec(st, &n, (unsigned)(NP_CUR - line_start(NP_CUR)) + 1);
    put_str(st, &n, "     ");
    put_dec(st, &n, (unsigned)NP_LEN);
    put_str(st, &n, " bytes");
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

/* put the caret on row `index` at column `col` (clamped to the row) */
static void goto_row_col(int index, int col)
{
    Row r;
    row_at(index, &r);
    int len = row_last_pos(&r) - r.start;
    NP_CUR = r.start + (col < len ? col : len);
}

/* the first and last row the selection (or just the caret) touches */
static void sel_lines(int *lo, int *hi)
{
    Row r;
    int a = row_find(NP_CUR, &r), b = row_find(NP_ANCHOR, &r);
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
}

/* Tell the window manager what really changed (after a key or a click), so it does not redraw the whole
 * window: the status bar always, and either the rows from the first to the last row the caret or the
 * selection touched before and after, or (if the text scrolled or the number of rows changed, so the rows
 * below moved) the whole text area. With word wrap an edit can also move words from row to row, so after an
 * edit everything from the first touched row to the bottom is redrawn. */
static void mark_changed(Window *w, int old_lo, int old_hi, int old_lines, int old_top, int old_hscroll, int edited)
{
    UiRect body = wm_content(w);
    UiRect in = ui_inset(text_area(body), NP_PAD);
    set_geometry(body);
    follow(in);
    int new_lo, new_hi;
    sel_lines(&new_lo, &new_hi);
    UiRect rows = in;
    if (NP_TOP == old_top && NP_HSCROLL == old_hscroll && row_count() == old_lines)
    {
        int a = old_lo < new_lo ? old_lo : new_lo;
        int b = old_hi > new_hi ? old_hi : new_hi;
        if (edited && NP_WRAP)
            b = NP_TOP + NP_ROWS - 1;
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

/* 1 if a selection was copied; a text too big for the clipboard says so and copies nothing */
static int copy_selection(void)
{
    int a, b;
    if (!sel_range(&a, &b))
        return 0;
    if (!ui_clipboard_set(NP_BUF + a, b - a))
    {
        NP_NOTE = "Too large to copy";
        return 0;
    }
    return 1;
}

/* What of the clipboard text can be pasted: '\n' and printable ASCII stay, a Tab becomes spaces, '\r' and other control
 * characters are dropped. Writes into out (if not NULL) and returns the length; stops at cap. */
static int filter_clip(const char *clip, int n, char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < n && o < cap; i++)
    {
        unsigned char c = (unsigned char)clip[i];
        if (c == '\n' || (c >= 32 && c < 127))
        {
            if (out)
                out[o] = (char)c;
            o++;
        }
        else if (c == '\t')
            for (int k = 0; k < NP_TAB && o < cap; k++, o++)
                if (out)
                    out[o] = ' ';
    }
    return o;
}

/* paste the clipboard at the caret, over the selection */
static void paste(void)
{
    int n;
    const char *clip = ui_clipboard_text(&n); /* the clipboard's own memory: a big text is not copied twice */
    int len = filter_clip(clip, n, 0, NP_LIMIT);
    if (len == 0)
        return;
    char *text = kmalloc((unsigned long long)len);
    if (!text)
    {
        NP_NOTE = "Out of memory";
        return;
    }
    filter_clip(clip, n, text, len);
    insert_text(text, len); /* one undo step, whatever its size */
    kfree(text);
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
    buf_release(); /* the memory goes back to the heap */
    if (NP_POOL)
        kfree(NP_POOL);
    NP_POOL = 0;
    NP_POOL_CAP = 0;
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

/* a few words for what a drive refused */
static const char *short_error(int code)
{
    switch (code)
    {
    case VFS_ERR_UNSUPPORTED:
        return "not supported yet";
    case VFS_ERR_IO:
        return "disk error";
    case VFS_ERR_FULL:
        return "drive full";
    case VFS_ERR_EXISTS:
        return "name already used";
    case VFS_ERR_RO:
        return "drive locked";
    case VFS_ERR_NOWRITE:
        return "drive is read-only";
    case VFS_ERR_BADNAME:
        return "bad file name";
    case VFS_ERR_NOTREADABLE:
        return "compressed or encrypted file";
    case VFS_ERR_DIRTY:
        return "drive needs a check in Windows";
    default:
        return vfs_error_text(code);
    }
}

/* Why vfs_save failed in folder `parent`: "NTFS: could not delete the old file (not supported yet)" - the file
 * system and the step say what the drive refused, which a bare "not supported" never could. */
static const char *save_problem(int parent)
{
    static char msg[112];
    const char *step = vfs_save_step();
    VfsVolInfo vi;
    if (!step[0] || vfs_volume_info(vfs_volume_of(parent), &vi) != 0)
        return vfs_problem();
    int n = 0;
    put_str(msg, &n, vi.fs_name[0] ? vi.fs_name : "Drive");
    if (vfs_last_error() == VFS_ERR_UNSUPPORTED && same_str(vi.fs_name, "NTFS") && ntfs_unsupported_why())
    {
        put_str(msg, &n, ": ");
        put_str(msg, &n, ntfs_unsupported_why()); /* the driver knows the reason: better than "not supported yet" */
        return msg;
    }
    put_str(msg, &n, ": could not ");
    put_str(msg, &n, step);
    put_str(msg, &n, " (");
    put_str(msg, &n, short_error(vfs_last_error()));
    if (vfs_last_error() == VFS_ERR_UNSUPPORTED && same_str(vi.fs_name, "NTFS") && ntfs_unsupported_line())
    {
        put_str(msg, &n, ", ntfs.c:"); /* which limit of the NTFS driver was hit (see ntfs_unsupported_line) */
        put_dec(msg, &n, (unsigned)ntfs_unsupported_line());
    }
    put_str(msg, &n, ")");
    return msg;
}

/* Why a file cannot be saved into folder `dir`, as a short sentence, or NULL if it can. Drives are write-locked
 * until the user unlocks them in the File Explorer, and the RAM Disk and This PC hold no file content at all. */
static const char *folder_problem(int dir)
{
    if (dir == VFS_ROOT)
        return "Open a drive first: nothing can be saved in This PC";
    VfsVolInfo vi;
    if (vfs_volume_info(vfs_volume_of(dir), &vi) != 0)
        return "Folder not found";
    if (same_str(vi.fs_name, "RAM"))
        return "The RAM Disk keeps no file content: pick a real drive";
    if (!vi.writable)
        return vfs_error_text(VFS_ERR_NOWRITE);
    if (vi.lockable && vi.locked)
        return "Drive is locked: unlock it in File Explorer first";
    return 0;
}

/* Replace the text with the file at `path`. Returns NULL, or a sentence saying why not (the text is then untouched).
 * The file is read into a temporary block of the heap and checked first; only then does it replace the text. */
static const char *open_file(const char *path)
{
    int id = vfs_lookup(path);
    if (id < 0)
        return "File not found";
    const VfsNode *n = vfs_node(id);
    if (n->is_dir)
        return "That is a folder, not a file";
    if (n->size > (unsigned)NP_LIMIT)
        return "File is too large (the limit is 16 MB)";
    unsigned size = n->size, done = 0;
    unsigned char *raw = size ? kmalloc(size) : 0;
    if (size && !raw)
        return "Out of memory";
    while (done < size)
    {
        int r = vfs_read(id, done, raw + done, size - done);
        if (r < 0)
        {
            kfree(raw);
            return vfs_problem();
        }
        if (r == 0)
            break;
        done += (unsigned)r;
    }
    long long out = 0; /* the length of the text: Tab becomes spaces, carriage returns go */
    for (unsigned i = 0; i < done; i++)
    {
        unsigned char c = raw[i];
        if (c == '\t')
            out += NP_TAB;
        else if (c == '\n' || (c >= 32 && c < 127))
            out++;
        else if (c != '\r')
        {
            kfree(raw);
            return "Only plain ASCII text can be opened";
        }
    }
    if (out > NP_LIMIT)
    {
        kfree(raw);
        return "File is too large (the limit is 16 MB)";
    }
    char *text = out ? kmalloc((unsigned long long)out) : 0;
    if (out && !text)
    {
        kfree(raw);
        return "Out of memory";
    }
    int len = 0;
    for (unsigned i = 0; i < done; i++)
    {
        unsigned char c = raw[i];
        if (c == '\t')
            for (int k = 0; k < NP_TAB; k++)
                text[len++] = ' ';
        else if (c != '\r')
            text[len++] = (char)c;
    }
    kfree(raw);
    new_document(); /* frees the old text and the undo history */
    if (text) /* the new block becomes the text buffer */
    {
        NP_BUF = text;
        NP_CAP = len;
        NP_LEN = len;
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
    const char *why = folder_problem(parent);
    if (why)
        return why;
    if (vfs_save(parent, name, NP_BUF, (unsigned)NP_LEN) < 0)
        return save_problem(parent);
    copy_str(NP_PATH, (int)sizeof NP_PATH, path);
    NP_DIRTY = 0;
    return 0;
}

/* ---- the Save As / Open dialog: a small file browser ----
 * The top line shows the folder, "Up" goes to its parent, the list shows what is in it. One click selects
 * (a file puts its name into the "File name" field), a double click or Enter opens a folder (or, on a file,
 * does Open / Save). The "File name" field takes a name for the shown folder, or a full path starting with '/'. */
#define DLG_W 580
#define DLG_H 480
#define DLG_M 20 /* margin inside the dialog */
#define DLG_ROW 26
#define DLG_MAX_ITEMS 256
#define DLG_DOUBLE_MS 450

enum
{
    NP_DLG_SAVE,
    NP_DLG_OPEN
};
static Window *NP_DLG;
static int NP_DLG_MODE;
static const char *NP_DLG_ERR;              /* what went wrong, shown above the field (a literal) */
static char NP_DLG_WARNED[UI_EDIT_MAX + 1]; /* the path the "file exists" warning was shown for */
static int NP_DLG_DIR;                      /* the folder being shown (VFS_ROOT = "This PC", the drives) */
static int NP_AFTER = -1;                   /* an NP_ACT_*: what to do once this dialog has saved (the "Save" answer of the
                                             * unsaved-changes question for an untitled text); -1 = nothing */
static void run_action(int action);
static int NP_DLG_ITEMS[DLG_MAX_ITEMS];     /* its entries: folders first, then by name */
static int NP_DLG_N;
static int NP_DLG_SEL = -1;                 /* selected row (index into NP_DLG_ITEMS), -1 = none */
static int NP_DLG_TOP;                      /* first visible row */
static int NP_DLG_LAST_ROW = -1;            /* the row of the last click and when, for the double click */
static unsigned long long NP_DLG_LAST_MS;

typedef struct
{
    UiRect path, list, label, edit;
} DlgLayout;

/* where things are, from the dialog's content area (the same for drawing and for the mouse) */
static DlgLayout dlg_layout(UiRect content)
{
    UiRect b = ui_rect(content.x + DLG_M, content.y + DLG_M, content.w - 2 * DLG_M, content.h - 2 * DLG_M);
    DlgLayout l;
    int list_h = b.h - 148; /* what is left above the name field and the buttons */
    if (list_h < DLG_ROW)
        list_h = DLG_ROW;
    l.path = ui_rect(b.x, b.y, b.w - 90, 30);
    l.list = ui_rect(b.x, b.y + 42, b.w, list_h);
    l.label = ui_rect(b.x, l.list.y + l.list.h + 8, b.w, 20);
    l.edit = ui_rect(b.x, l.label.y + 22, b.w, 34);
    return l;
}

static int dlg_rows(const DlgLayout *l)
{
    return l->list.h / DLG_ROW;
}

/* show folder `dir`: read it from the drive again and list it */
static void dlg_enter(int dir)
{
    NP_DLG_DIR = dir;
    vfs_refresh(dir);
    NP_DLG_N = vfs_list(dir, NP_DLG_ITEMS, DLG_MAX_ITEMS);
    if (NP_DLG_N > DLG_MAX_ITEMS)
        NP_DLG_N = DLG_MAX_ITEMS;
    NP_DLG_SEL = -1;
    NP_DLG_TOP = 0;
    NP_DLG_LAST_ROW = -1;
    NP_DLG_ERR = 0;
}

/* the full path of file `name` as the dialog means it: a name starting with '/' is already one */
static void dlg_full_path(const char *name, char *out, int cap)
{
    if (name[0] == '/')
    {
        copy_str(out, cap, name);
        return;
    }
    int n = vfs_path(NP_DLG_DIR, out, cap);
    if (n > 0 && out[n - 1] != '/' && n < cap - 1)
        out[n++] = '/';
    for (const char *s = name; *s && n < cap - 1; s++)
        out[n++] = *s;
    out[n] = 0;
}

/* the first drive that holds real files: where a dialog starts when nothing is open */
static int default_dir(void)
{
    int locked = VFS_ROOT; /* a drive that is only locked is better than none: the dialog then says so */
    for (int i = 0; i < vfs_volume_count(); i++)
    {
        VfsVolInfo vi;
        if (vfs_volume_info(i, &vi) != 0 || !vi.writable || same_str(vi.fs_name, "RAM"))
            continue;
        if (!(vi.lockable && vi.locked))
            return vi.node;
        if (locked == VFS_ROOT)
            locked = vi.node;
    }
    return locked;
}

/* "512 B", "12 KB", "3 MB" */
static void size_text(unsigned size, char *out)
{
    int n = 0;
    if (size < 1024)
    {
        put_dec(out, &n, size);
        put_str(out, &n, " B");
    }
    else if (size < 1024u * 1024u)
    {
        put_dec(out, &n, size / 1024);
        put_str(out, &n, " KB");
    }
    else
    {
        put_dec(out, &n, size / (1024u * 1024u));
        put_str(out, &n, " MB");
    }
}

/* a short remark for a drive in the This PC list: what stops files being saved on it, or NULL */
static const char *volume_badge(int id)
{
    if (!vfs_is_volume_root(id))
        return 0;
    VfsVolInfo vi;
    if (vfs_volume_info(vfs_volume_of(id), &vi) != 0)
        return 0;
    if (same_str(vi.fs_name, "RAM"))
        return "no file content";
    if (!vi.writable)
        return "read only";
    if (vi.lockable && vi.locked)
        return "locked";
    return 0;
}

static void dlg_draw(Window *w, UiRect body)
{
    const UiTheme *t = ui_theme();
    DlgLayout l = dlg_layout(body);

    char path[UI_EDIT_MAX + 1];
    if (NP_DLG_DIR == VFS_ROOT)
        copy_str(path, (int)sizeof path, "This PC");
    else
        vfs_path(NP_DLG_DIR, path, (int)sizeof path);
    ui_text_in(l.path, path, UI_TEXT_LABEL, UI_ALIGN_LEFT);

    gfx_fill_round_rect(l.list.x, l.list.y, l.list.w, l.list.h, t->radius_s, ui_color(t->surface_alt));
    gfx_round_rect(l.list.x, l.list.y, l.list.w, l.list.h, t->radius_s, 1, ui_color(t->border));
    int rows = dlg_rows(&l);
    if (NP_DLG_N == 0)
        ui_text_in_color(l.list, "This folder is empty", UI_TEXT_SMALL, UI_ALIGN_CENTER, t->text_dim);
    for (int r = 0; r < rows && NP_DLG_TOP + r < NP_DLG_N; r++)
    {
        int idx = NP_DLG_TOP + r;
        const VfsNode *n = vfs_node(NP_DLG_ITEMS[idx]);
        if (!n)
            continue;
        UiRect row = ui_rect(l.list.x + 4, l.list.y + r * DLG_ROW, l.list.w - 12, DLG_ROW);
        if (idx == NP_DLG_SEL)
            gfx_fill_rect_alpha(row.x, row.y + 1, row.w, row.h - 2, t->accent.r, t->accent.g, t->accent.b, 90);
        ui_icon(n->is_dir ? UI_ICON_FILES : UI_ICON_NOTEPAD, row.x + 16, row.y + row.h / 2, 18, t->text);
        const char *badge = n->is_dir ? volume_badge(NP_DLG_ITEMS[idx]) : 0;
        ui_text_in(ui_rect(row.x + 34, row.y, row.w - 34 - (n->is_dir ? (badge ? 130 : 0) : 80), row.h), n->name,
                   UI_TEXT_BODY, UI_ALIGN_LEFT);
        if (badge)
            ui_text_in_color(ui_rect(row.x + row.w - 126, row.y, 122, row.h), badge, UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                             t->warning);
        if (!n->is_dir)
        {
            char sz[16];
            size_text(n->size, sz);
            ui_text_in_color(ui_rect(row.x + row.w - 76, row.y, 72, row.h), sz, UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
        }
    }
    if (NP_DLG_N > rows && rows > 0) /* a thin scroll thumb */
    {
        int thumb = l.list.h * rows / NP_DLG_N;
        if (thumb < 16)
            thumb = 16;
        int y = l.list.y + (l.list.h - thumb) * NP_DLG_TOP / (NP_DLG_N - rows);
        gfx_fill_round_rect(l.list.x + l.list.w - 6, y, 3, thumb, 1, ui_color(t->border));
    }

    ui_text_color(l.label.x, l.label.y, "File name", UI_TEXT_SMALL, t->text_dim);
    const char *hint = NP_DLG_MODE == NP_DLG_SAVE ? folder_problem(NP_DLG_DIR) : 0; /* this folder cannot take a file */
    if (NP_DLG_ERR)
        ui_text_in_color(ui_rect(l.label.x + 90, l.label.y, l.label.w - 90, 20), NP_DLG_ERR, UI_TEXT_SMALL, UI_ALIGN_LEFT,
                         t->danger);
    else if (hint)
        ui_text_in_color(ui_rect(l.label.x + 90, l.label.y, l.label.w - 90, 20), hint, UI_TEXT_SMALL, UI_ALIGN_LEFT,
                         t->warning);
    ui_edit_draw(l.edit, &w->edits[0], NP_DLG_MODE == NP_DLG_SAVE ? "name.txt" : "file name", 1);
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
    const char *name = NP_DLG->edits[0].text;
    if (!name[0])
    {
        NP_DLG_ERR = "Type a file name";
        return;
    }
    char path[UI_EDIT_MAX + 1];
    dlg_full_path(name, path, (int)sizeof path);
    const char *err;
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
    int after = NP_DLG_MODE == NP_DLG_SAVE ? NP_AFTER : -1;
    wm_close(NP_DLG); /* (this forgets NP_AFTER, like a Cancel does) */
    np_after_change();
    if (after >= 0)
        run_action(after); /* saved: now what the user was doing (New, Open, close ...) */
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

static void on_dlg_up(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    const VfsNode *n = vfs_node(NP_DLG_DIR);
    if (NP_DLG && n && n->parent >= 0)
        dlg_enter(n->parent);
}

/* select row idx: it scrolls into view, and a file's name goes into the field */
static void dlg_select(int idx)
{
    DlgLayout l = dlg_layout(wm_content(NP_DLG));
    int rows = dlg_rows(&l);
    if (idx < 0)
        idx = 0;
    if (idx > NP_DLG_N - 1)
        idx = NP_DLG_N - 1;
    if (idx < 0)
        return;
    NP_DLG_SEL = idx;
    if (idx < NP_DLG_TOP)
        NP_DLG_TOP = idx;
    if (idx >= NP_DLG_TOP + rows)
        NP_DLG_TOP = idx - rows + 1;
    const VfsNode *n = vfs_node(NP_DLG_ITEMS[idx]);
    if (n && !n->is_dir)
        ui_edit_set_text(&NP_DLG->edits[0], n->name);
    NP_DLG_ERR = 0;
}

static void dlg_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    DlgLayout l = dlg_layout(wm_content(w));
    int rows = dlg_rows(&l);
    switch (k->key)
    {
    case KEY_ENTER:
    {
        const VfsNode *n = NP_DLG_SEL >= 0 ? vfs_node(NP_DLG_ITEMS[NP_DLG_SEL]) : 0;
        if (n && n->is_dir) /* a folder chosen in the list: go into it */
            dlg_enter(NP_DLG_ITEMS[NP_DLG_SEL]);
        else
            dlg_ok();
        break;
    }
    case KEY_ESC:
        wm_close(w);
        break;
    case KEY_UP:
        dlg_select(NP_DLG_SEL < 0 ? NP_DLG_N - 1 : NP_DLG_SEL - 1);
        break;
    case KEY_DOWN:
        dlg_select(NP_DLG_SEL + 1);
        break;
    case KEY_PAGEUP:
        dlg_select(NP_DLG_SEL - rows);
        break;
    case KEY_PAGEDOWN:
        dlg_select(NP_DLG_SEL + rows);
        break;
    default:
        if (ui_edit_key(&w->edits[0], k))
        {
            NP_DLG_ERR = 0; /* the name changed: the old complaint no longer applies, and Enter means OK */
            NP_DLG_SEL = -1;
            ui_caret_restart();
        }
        break;
    }
}

static int dlg_press(Window *w, int x, int y)
{
    DlgLayout l = dlg_layout(wm_content(w));
    if (ui_contains(l.list, x, y))
    {
        int idx = NP_DLG_TOP + (y - l.list.y) / DLG_ROW;
        if ((y - l.list.y) / DLG_ROW >= dlg_rows(&l) || idx >= NP_DLG_N)
            return 0;
        unsigned long long now = timer_ms();
        int dbl = idx == NP_DLG_LAST_ROW && now - NP_DLG_LAST_MS < DLG_DOUBLE_MS;
        NP_DLG_LAST_ROW = idx;
        NP_DLG_LAST_MS = now;
        int id = NP_DLG_ITEMS[idx];
        dlg_select(idx);
        if (dbl)
        {
            if (vfs_node(id)->is_dir)
                dlg_enter(id);
            else
                dlg_ok();
        }
        return 1;
    }
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

static int dlg_wheel(Window *w, int steps)
{
    DlgLayout l = dlg_layout(wm_content(w));
    int top = NP_DLG_TOP + steps * 3, max = NP_DLG_N - dlg_rows(&l);
    if (top > max)
        top = max;
    if (top < 0)
        top = 0;
    if (top == NP_DLG_TOP)
        return 0;
    NP_DLG_TOP = top;
    return 1;
}

static void dlg_close(Window *w)
{
    (void)w;
    NP_DLG = 0;
    NP_AFTER = -1; /* closed without saving (Cancel): the pending New / Open / close is dropped */
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
    int w = DLG_W, h = DLG_H;
    if (w > (int)gfx_width() - 20)
        w = (int)gfx_width() - 20;
    if (h > (int)gfx_height() - TASKBAR_HEIGHT - 8)
        h = (int)gfx_height() - TASKBAR_HEIGHT - 8;
    Window *win = open_dialog_window(mode == NP_DLG_SAVE ? "Save As" : "Open", w, h, dlg_draw);
    if (!win)
        return;
    NP_DLG = win;
    NP_DLG_MODE = mode;
    NP_DLG_WARNED[0] = 0;
    win->on_key = dlg_key;
    win->on_press = dlg_press;
    win->on_drag = dlg_drag;
    win->on_wheel = dlg_wheel;
    win->on_close = dlg_close;
    win->edit_count = 1;
    win->focus = 0;
    win->caret = 1;

    /* start in the folder of the current file, else on the first drive */
    int id = NP_PATH[0] ? vfs_lookup(NP_PATH) : -1;
    dlg_enter(id >= 0 && vfs_node(id)->parent >= 0 ? vfs_node(id)->parent : default_dir());
    if (NP_PATH[0])
        ui_edit_set_text(&win->edits[0], path_name(NP_PATH));
    else
        ui_edit_set_text(&win->edits[0], mode == NP_DLG_SAVE ? "untitled.txt" : "");
    if (id >= 0)
        for (int i = 0; i < NP_DLG_N; i++) /* the open file is the selected row */
            if (NP_DLG_ITEMS[i] == id)
                NP_DLG_SEL = i;

    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    int anchor = WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM;
    wm_add_button_ex(win, "Up", UI_BTN_SECONDARY, ui_rect(cw - DLG_M - 80, DLG_M, 80, 30), WM_ANCHOR_RIGHT, on_dlg_up, 0);
    wm_set_button_icon(win, 0, UI_ICON_ARROW_UP);
    wm_add_button_ex(win, "Cancel", UI_BTN_SECONDARY, ui_rect(cw - 20 - 100 - 10 - 100, ch - 20 - 34, 100, 34), anchor,
                     on_dlg_cancel, 0);
    wm_add_button_ex(win, mode == NP_DLG_SAVE ? "Save" : "Open", UI_BTN_PRIMARY,
                     ui_rect(cw - 20 - 100, ch - 20 - 34, 100, 34), anchor, on_dlg_ok, 0);
}

/* ---- "discard unsaved changes?" ---- */
enum
{
    NP_ACT_NEW,
    NP_ACT_OPEN,
    NP_ACT_PATH, /* open the file in NP_PENDING_PATH (asked by another app) */
    NP_ACT_CLOSE /* the user closed the window */
};
static Window *NP_CONF;
static int NP_PENDING;
static char NP_PENDING_PATH[UI_EDIT_MAX + 1];

static void conf_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    body = ui_inset(body, t->pad);
    ui_text(body.x, body.y, NP_PENDING == NP_ACT_CLOSE ? "Save changes before closing?" : "Save your changes first?",
            UI_TEXT_TITLE);
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

/* carry out what the unsaved-changes question was about (the text is saved or given up: NP_DIRTY is off) */
static void run_action(int action)
{
    if (!NP_WIN)
        return;
    NP_DIRTY = 0;
    if (action == NP_ACT_NEW)
    {
        new_document();
        NP_NOTE = "";
        np_after_change();
    }
    else if (action == NP_ACT_CLOSE)
        wm_close(NP_WIN);
    else if (action == NP_ACT_PATH)
    {
        const char *err = open_file(NP_PENDING_PATH);
        NP_NOTE = err ? err : "Opened";
        np_after_change();
    }
    else
        open_path_dialog(NP_DLG_OPEN);
}

/* "Don't save" */
static void on_conf_yes(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    if (NP_CONF)
        wm_close(NP_CONF);
    run_action(NP_PENDING);
}

/* "Save": a named file is saved at once, an untitled text goes through the Save As dialog; the action then
 * follows. If saving fails the message is shown and nothing else happens: the text is still there. */
static void on_conf_save(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    int action = NP_PENDING;
    if (NP_CONF)
        wm_close(NP_CONF);
    if (!NP_WIN)
        return;
    if (!NP_PATH[0])
    {
        if (NP_DLG)
            wm_close(NP_DLG); /* another path dialog is open (an Open one, say): this one is for saving */
        open_path_dialog(NP_DLG_SAVE);
        NP_AFTER = NP_DLG ? action : -1; /* continue when the dialog has saved (dlg_ok) */
        return;
    }
    const char *err = save_file(NP_PATH);
    NP_NOTE = err ? err : "Saved";
    np_after_change();
    if (!err)
        run_action(action);
}

static void conf_key(Window *w, const KeyEvent *k)
{
    if (k->pressed && k->key == KEY_ESC)
        wm_close(w); /* no default answer on Enter: discarding must be a deliberate click */
}

/* returns 0 if the question could not be shown (no free window) */
static int ask_discard(int action)
{
    if (NP_CONF)
    {
        NP_PENDING = action; /* the newest request is the one the answer applies to */
        wm_activate(NP_CONF);
        return 1;
    }
    int w = 420, h = 190;
    Window *win = open_dialog_window("Notepad", w, h, conf_draw);
    if (!win)
        return 0;
    NP_CONF = win;
    NP_PENDING = action;
    win->on_key = conf_key;
    win->on_close = conf_close;
    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    int anchor = WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM;
    wm_add_button_ex(win, "Cancel", UI_BTN_SECONDARY, ui_rect(cw - 20 - 100 - 10 - 120 - 10 - 100, ch - 20 - 34, 100, 34),
                     anchor, on_conf_no, 0);
    wm_add_button_ex(win, "Don't save", UI_BTN_DANGER, ui_rect(cw - 20 - 100 - 10 - 120, ch - 20 - 34, 120, 34), anchor,
                     on_conf_yes, 0);
    wm_add_button_ex(win, "Save", UI_BTN_PRIMARY, ui_rect(cw - 20 - 100, ch - 20 - 34, 100, 34), anchor, on_conf_save, 0);
    return 1;
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

/* turn word wrap on or off; the row at the top of the view stays where it is */
static void cmd_wrap(void)
{
    if (NP_WIN)
        set_geometry(wm_content(NP_WIN));
    Row top, r;
    row_at(NP_TOP, &top);
    int p = top.start;
    NP_WRAP = !NP_WRAP;
    NP_HSCROLL = 0;
    NP_TOP = row_find(p, &r);
    NP_FOLLOW = 1;
    if (NP_WIN)
        NP_WIN->btn[NP_WRAP_BTN].kind = NP_WRAP ? UI_BTN_PRIMARY : UI_BTN_SECONDARY; /* the button shows the state */
}

static void on_new(UiButton *b, void *user) { (void)b; (void)user; cmd_new(); }
static void on_open(UiButton *b, void *user) { (void)b; (void)user; cmd_open(); }
static void on_save(UiButton *b, void *user) { (void)b; (void)user; cmd_save(); }
static void on_save_as(UiButton *b, void *user) { (void)b; (void)user; cmd_save_as(); }
static void on_wrap(UiButton *b, void *user) { (void)b; (void)user; cmd_wrap(); }

/* ---- find and replace ----
 * The pattern is NP_FIND (one line, ASCII). Matching ignores upper / lower case unless "Aa" is on. Find Next starts
 * at the end of the selection (the caret if nothing is selected), Prev at its start, both continue from the other
 * end of the text. A found match becomes the selection, so typing replaces it. Replace changes the selected match
 * (if the selection is one) and finds the next; All replaces every match left to right as ONE undo step. */
static int lc(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int match_at(int i, const char *pat, int n)
{
    if (i < 0 || n <= 0 || i + n > NP_LEN)
        return 0;
    for (int k = 0; k < n; k++)
    {
        int a = (unsigned char)NP_BUF[i + k], b = (unsigned char)pat[k];
        if (NP_MATCH_CASE ? a != b : lc(a) != lc(b))
            return 0;
    }
    return 1;
}

/* the first match at or after `from`, continuing from the top; -1 if none */
static int find_forward(int from, int n)
{
    for (int i = from < 0 ? 0 : from; i + n <= NP_LEN; i++)
        if (match_at(i, NP_FIND.text, n))
            return i;
    for (int i = 0; i < from && i + n <= NP_LEN; i++)
        if (match_at(i, NP_FIND.text, n))
            return i;
    return -1;
}

/* the last match starting before `before`, continuing from the end; -1 if none */
static int find_backward(int before, int n)
{
    for (int i = (before - 1 < NP_LEN - n ? before - 1 : NP_LEN - n); i >= 0; i--)
        if (match_at(i, NP_FIND.text, n))
            return i;
    for (int i = NP_LEN - n; i >= before && i >= 0; i--)
        if (match_at(i, NP_FIND.text, n))
            return i;
    return -1;
}

/* how many matches there are (left to right, not overlapping), and the number of the one starting at `at` */
static int count_matches(int n, int at, int *which)
{
    int total = 0;
    *which = 0;
    for (int i = 0; i + n <= NP_LEN;)
    {
        if (match_at(i, NP_FIND.text, n))
        {
            total++;
            if (i == at)
                *which = total;
            i += n;
        }
        else
            i++;
    }
    return total;
}

static void select_match(int i, int n)
{
    NP_ANCHOR = i;
    NP_CUR = i + n;
    NP_DESIRED = col_in_row();
    NP_FOLLOW = 1;
    NP_LASTKIND = NP_KIND_NONE;
    ui_caret_restart();
}

/* Find Next / Prev: selects the match and says "3 of 7"; 0 if there is none */
static int do_find(int backwards)
{
    int n = NP_FIND.len;
    if (n == 0)
    {
        NP_NOTE = "Type what to find";
        return 0;
    }
    int a = 0, b = 0, has = sel_range(&a, &b);
    int from = backwards ? (has ? a : NP_CUR) : (has ? b : NP_CUR);
    int i = backwards ? find_backward(from, n) : find_forward(from, n);
    if (i < 0)
    {
        NP_NOTE = "Not found";
        return 0;
    }
    select_match(i, n);
    int which, total = count_matches(n, i, &which), k = 0;
    put_dec(NP_NOTE_BUF, &k, (unsigned)which);
    put_str(NP_NOTE_BUF, &k, " of ");
    put_dec(NP_NOTE_BUF, &k, (unsigned)total);
    NP_NOTE = NP_NOTE_BUF;
    return 1;
}

/* the text was changed by Replace: the star in the title (edit_replace already keeps the undo history) */
static void replaced(Window *w)
{
    NP_DIRTY = 1;
    NP_FOLLOW = 1;
    if (w)
        np_title(w);
}

static void do_replace(Window *w)
{
    int n = NP_FIND.len, rn = NP_REPL.len;
    int a = 0, b = 0;
    if (n && sel_range(&a, &b) && b - a == n && match_at(a, NP_FIND.text, n)) /* the selection is a match: replace it */
    {
        if (NP_LEN - n + rn > NP_LIMIT)
        {
            NP_NOTE = "The text would get too long";
            return;
        }
        edit_replace(a, n, NP_REPL.text, rn, NP_KIND_NONE);
        replaced(w);
        if (!do_find(0))
            NP_NOTE = "Replaced - no more matches";
    }
    else
        do_find(0);
}

static void do_replace_all(Window *w)
{
    int n = NP_FIND.len, rn = NP_REPL.len, which;
    if (n == 0)
    {
        NP_NOTE = "Type what to find";
        return;
    }
    int count = count_matches(n, -1, &which);
    if (count == 0)
    {
        NP_NOTE = "Not found";
        return;
    }
    long long olen = (long long)NP_LEN + (long long)count * (rn - n); /* the length of the whole result */
    if (olen > NP_LIMIT)
    {
        NP_NOTE = "The text would get too long";
        return;
    }
    /* Only the part from the first match to the end of the last one changes: that part is replaced as one record
     * (not the whole text, which would fill the undo history on a big text). */
    int first = -1, last_end = 0;
    for (int i = 0; i + n <= NP_LEN;)
    {
        if (match_at(i, NP_FIND.text, n))
        {
            if (first < 0)
                first = i;
            last_end = i + n;
            i += n;
        }
        else
            i++;
    }
    long long span = (long long)(last_end - first) + (long long)count * (rn - n); /* its length after the change */
    char *out = kmalloc(span > 0 ? (unsigned long long)span : 1);
    if (!out)
    {
        NP_NOTE = "Out of memory";
        return;
    }
    int o = 0;
    for (int i = first; i < last_end;)
    {
        if (match_at(i, NP_FIND.text, n))
        {
            for (int k = 0; k < rn; k++)
                out[o++] = NP_REPL.text[k];
            i += n;
        }
        else
            out[o++] = NP_BUF[i++];
    }
    edit_replace(first, last_end - first, out, o, NP_KIND_NONE); /* one record: one Ctrl+Z takes it all back */
    kfree(out);
    NP_CUR = NP_ANCHOR = first; /* the view goes to the first change */
    NP_DESIRED = col_in_row();
    NP_FOCUS = 0; /* the keyboard goes back to the text, so that Ctrl+Z undoes this */
    replaced(w);
    int k = 0;
    put_dec(NP_NOTE_BUF, &k, (unsigned)count);
    put_str(NP_NOTE_BUF, &k, " replaced");
    NP_NOTE = NP_NOTE_BUF;
}

static void bar_set_button(Window *w)
{
    if (w)
        w->btn[NP_FIND_BTN].kind = NP_BAR ? UI_BTN_PRIMARY : UI_BTN_SECONDARY; /* the toolbar button shows the state */
}

/* show the bar with the keyboard in the Find (1) or Replace (2) field; a one-line selection becomes the pattern */
static void bar_open(Window *w, int focus)
{
    int a = 0, b = 0;
    if (focus == 1 && sel_range(&a, &b) && b - a <= 60)
    {
        char s[64];
        int n = 0, ok = 1;
        for (int i = a; i < b; i++)
        {
            if (NP_BUF[i] == '\n')
                ok = 0;
            s[n++] = NP_BUF[i];
        }
        s[n] = 0;
        if (ok)
            ui_edit_set_text(&NP_FIND, s);
    }
    NP_BAR = 1;
    NP_FOCUS = focus;
    NP_FOLLOW = 1; /* the text area got shorter: keep the caret in view */
    ui_edit_select_all(focus == 1 ? &NP_FIND : &NP_REPL);
    ui_caret_restart();
    bar_set_button(w);
    if (w)
        wm_add_damage(w->r);
}

static void bar_close(Window *w)
{
    NP_BAR = 0;
    NP_FOCUS = 0;
    NP_BAR_DRAG = 0;
    NP_FOLLOW = 1;
    bar_set_button(w);
    if (w)
        wm_add_damage(w->r);
}

/* after Find / Replace changed the selection or the text: redraw the text area and the status line */
static void find_damage(Window *w)
{
    UiRect body = wm_content(w);
    wm_add_damage(ui_inset(text_area(body), NP_PAD));
    wm_add_damage(status_rect(body));
}

/* Keys for the find bar and the find shortcuts. 0 = not handled; 1 = handled, damage is marked; 2 = handled, redraw
 * the whole window (the bar appeared or went away). */
static int find_key(Window *w, const KeyEvent *k)
{
    int ctrl = (k->mods & KMOD_CTRL) != 0, shift = (k->mods & KMOD_SHIFT) != 0;
    if (ctrl && k->key == 'f')
    {
        bar_open(w, 1);
        return 2;
    }
    if (ctrl && k->key == 'h')
    {
        bar_open(w, 2);
        return 2;
    }
    if (k->key == KEY_F3) /* Find Next / Prev from anywhere; with no pattern yet it opens the bar */
    {
        if (!NP_FIND.len)
        {
            bar_open(w, 1);
            return 2;
        }
        set_geometry(wm_content(w));
        do_find(shift);
        find_damage(w);
        return 1;
    }
    if (!NP_BAR || NP_FOCUS == 0)
        return 0;

    UiEdit *e = NP_FOCUS == 1 ? &NP_FIND : &NP_REPL;
    switch (k->key)
    {
    case KEY_ESC:
        bar_close(w);
        return 2;
    case KEY_TAB:
        NP_FOCUS = NP_FOCUS == 1 ? 2 : 1;
        ui_edit_select_all(NP_FOCUS == 1 ? &NP_FIND : &NP_REPL);
        ui_caret_restart();
        wm_add_damage(bar_layout(wm_content(w)).bar);
        return 1;
    case KEY_ENTER:
        set_geometry(wm_content(w));
        if (NP_FOCUS == 1)
            do_find(shift);
        else
            do_replace(w);
        find_damage(w);
        wm_add_damage(bar_layout(wm_content(w)).bar);
        return 1;
    default:
        break;
    }
    if (ui_edit_key(e, k))
    {
        NP_NOTE = "";
        ui_caret_restart();
        wm_add_damage(bar_layout(wm_content(w)).bar);
        wm_add_damage(status_rect(wm_content(w)));
        return 1;
    }
    /* Ctrl+Z Y A C X V are the field's own (they must not undo the text when the field has nothing to undo); other
     * chords (Ctrl+S, Alt+Z ...) go on to the text, plain keys the field did not use are dropped */
    if (ctrl && (k->key == 'z' || k->key == 'y' || k->key == 'a' || k->key == 'c' || k->key == 'x' || k->key == 'v'))
        return 1;
    return (ctrl || (k->mods & KMOD_ALT)) ? 0 : 1;
}

/* a click on the bar: 0 = not on it; 1 = handled (the window is redrawn) */
static int bar_press(Window *w, int x, int y)
{
    if (!NP_BAR)
        return 0;
    BarLayout l = bar_layout(wm_content(w));
    if (!ui_contains(l.bar, x, y))
        return 0;
    set_geometry(wm_content(w));
    NP_NOTE = "";
    if (ui_contains(l.find_f, x, y) || ui_contains(l.repl_f, x, y))
    {
        NP_FOCUS = ui_contains(l.find_f, x, y) ? 1 : 2;
        ui_edit_mouse(NP_FOCUS == 1 ? &NP_FIND : &NP_REPL, x, y, 1);
        NP_BAR_DRAG = 1; /* dragging selects inside the field */
        ui_caret_restart();
    }
    else if (ui_contains(l.prev, x, y))
        do_find(1);
    else if (ui_contains(l.next, x, y))
        do_find(0);
    else if (ui_contains(l.mcase, x, y))
        NP_MATCH_CASE = !NP_MATCH_CASE;
    else if (ui_contains(l.close, x, y))
        bar_close(w);
    else if (ui_contains(l.repl, x, y))
        do_replace(w);
    else if (ui_contains(l.all, x, y))
        do_replace_all(w);
    return 1;
}

/* the toolbar's Find button: show the bar, or hide it */
static void cmd_find(void)
{
    if (!NP_WIN)
        return;
    if (NP_BAR)
        bar_close(NP_WIN);
    else
        bar_open(NP_WIN, 1);
}

static void on_find(UiButton *b, void *user) { (void)b; (void)user; cmd_find(); }

static void np_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
    {
        wm_key_partial(); /* a release changes nothing: no redraw at all */
        return;
    }
    NP_NOTE = ""; /* a message stays until the next key */
    int fk = find_key(w, k); /* Ctrl+F / H, F3, and every key while the keyboard is in the find bar */
    if (fk)
    {
        if (fk == 1)
            wm_key_partial();
        return;
    }
    unsigned edits_before = NP_EDITS;
    int upos_before = NP_UPOS;
    int full = 0; /* the key opened a dialog or replaced the text: redraw the whole window */
    int vertical = 0;
    int motion = 0; /* a key that only moves the caret: with Shift it extends the selection */
    int keep_anchor = 0;
    int shift = (k->mods & KMOD_SHIFT) != 0, ctrl = (k->mods & KMOD_CTRL) != 0;
    set_geometry(wm_content(w));
    int old_lines = row_count(), old_lo, old_hi;
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
            if (copy_selection()) /* a text the clipboard cannot take is not cut */
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
            Row cr;
            int line = row_find(NP_CUR, &cr) + dir * step;
            if (line < 0)
                line = 0;
            if (line > row_count() - 1)
                line = row_count() - 1;
            goto_row_col(line, NP_DESIRED);
            vertical = 1;
            motion = 1;
            break;
        }
        case KEY_HOME:
        case KEY_END:
        {
            Row hr;
            row_find(NP_CUR, &hr); /* the start / end of the row (of the line, without word wrap) */
            NP_CUR = k->key == KEY_HOME ? hr.start : row_last_pos(&hr);
            motion = 1;
            break;
        }
        case KEY_BACKSPACE:
            if (!delete_selection() && NP_CUR > 0)
                edit_replace(NP_CUR - 1, 1, 0, 0, NP_KIND_BACK);
            break;
        case KEY_DELETE:
            if (shift) /* Shift+Delete = cut */
            {
                if (copy_selection())
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
            if ((k->mods & KMOD_ALT) && k->key == 'z') /* Alt+Z: word wrap on / off, as in Windows Notepad */
            {
                cmd_wrap();
                full = 1;
            }
            else if (k->ch >= 32 && k->ch < 127 && !(k->mods & KMOD_ALT))
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
        NP_DESIRED = col_in_row();
    NP_FOLLOW = 1;
    ui_caret_restart();
    NP_BLINK = 1;

    if (NP_EDITS != edits_before || NP_UPOS != upos_before)
        NP_DIRTY = 1; /* undo / redo count too: whether the text is back to the saved state is not tracked */
    np_title(w);      /* the star in the title bar (damages only the title bar) */

    mark_changed(w, old_lo, old_hi, old_lines, old_top, old_hscroll, NP_EDITS != edits_before || NP_UPOS != upos_before);
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
    if (line > row_count() - 1)
        line = row_count() - 1;
    if (line < 0)
        line = 0;
    Row r;
    row_at(line, &r);
    int col = cols_in(r.start, r.end - r.start, x - in.x + NP_HSCROLL + 3); /* +3: nearer half of a letter wins */
    int pos = r.start + col;
    return pos > row_last_pos(&r) ? row_last_pos(&r) : pos; /* not past the end of a wrapped row: that is the next row */
}

static int np_press(Window *w, int x, int y)
{
    int on_bar = bar_press(w, x, y);
    if (on_bar)
        return on_bar;
    UiRect body = wm_content(w);
    UiRect in = ui_inset(text_area(body), NP_PAD);
    if (!ui_contains(in, x, y))
        return 0;
    int left_bar = NP_FOCUS != 0; /* the keyboard comes back to the text: the bar loses its caret */
    NP_FOCUS = 0;

    set_geometry(body);
    int old_lo, old_hi;
    sel_lines(&old_lo, &old_hi);
    int old_lines = row_count();
    int old_top = NP_TOP, old_hscroll = NP_HSCROLL;
    NP_CUR = NP_ANCHOR = pos_at(in, x, y);
    NP_SELECTING = 1; /* dragging from here selects */
    NP_LASTKIND = NP_KIND_NONE; /* clicking ends a typing run */
    NP_DESIRED = col_in_row();
    NP_FOLLOW = 1;
    ui_caret_restart();
    NP_BLINK = 1;
    mark_changed(w, old_lo, old_hi, old_lines, old_top, old_hscroll, 0);
    return left_bar ? 1 : 2; /* 2: the damage is already marked, only the touched rows and the status bar are redrawn */
}

static int np_drag(Window *w, int x, int y, unsigned buttons)
{
    if (NP_BAR_DRAG) /* selecting inside a field of the find bar */
    {
        UiEdit *e = NP_FOCUS == 2 ? &NP_REPL : &NP_FIND;
        if (!(buttons & 1))
        {
            NP_BAR_DRAG = 0;
            return 0;
        }
        if (!ui_edit_mouse(e, x, y, buttons))
            return 0;
        ui_caret_restart();
        wm_add_damage(ui_inset(e->field, -3));
        return 2;
    }
    if (!(buttons & 1))
    {
        NP_SELECTING = 0;
        return 0;
    }
    if (!NP_SELECTING)
        return 0;
    set_geometry(wm_content(w));
    UiRect in = ui_inset(text_area(wm_content(w)), NP_PAD);
    int pos = pos_at(in, x, y);
    if (pos == NP_CUR)
        return 0;
    int old_lo, old_hi;
    sel_lines(&old_lo, &old_hi);
    int old_lines = row_count();
    int old_top = NP_TOP, old_hscroll = NP_HSCROLL;
    NP_CUR = pos; /* the anchor stays where the press was */
    NP_DESIRED = col_in_row();
    NP_FOLLOW = 1;
    mark_changed(w, old_lo, old_hi, old_lines, old_top, old_hscroll, 0);
    return 2;
}

static int np_wheel(Window *w, int steps)
{
    set_geometry(wm_content(w));
    int top = NP_TOP + steps * 3;
    if (top > row_count() - 1)
        top = row_count() - 1;
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
    if (NP_BAR && NP_FOCUS != 0) /* the caret is in a field of the find bar */
    {
        UiEdit *fe = NP_FOCUS == 2 ? &NP_REPL : &NP_FIND;
        wm_add_damage(ui_inset(fe->field, -3));
        return 2;
    }
    if (NP_CARET.w == 0)
        return 0;
    wm_add_damage(ui_rect(NP_CARET.x - 1, NP_CARET.y - 1, NP_CARET.w + 2, NP_CARET.h + 2)); /* just the caret, not the window */
    return 2;
}

/* the close button, Alt+F4 or the taskbar menu: with unsaved text the window stays and asks first */
static int np_close_request(Window *w)
{
    (void)w;
    if (!NP_DIRTY)
        return 0;
    if (!ask_discard(NP_ACT_CLOSE))
        NP_NOTE = "Window limit reached - close one first"; /* it cannot ask: keep the text rather than lose it */
    return 1;
}

static void np_close(Window *w)
{
    (void)w;
    NP_WIN = 0;
    NP_SELECTING = 0;
    NP_BAR = 0; /* the next window starts clean: no text, no file name, no undo history, no find bar */
    NP_FOCUS = 0;
    NP_BAR_DRAG = 0;
    NP_AFTER = -1;
    NP_NOTE = "";
    new_document();
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
    win->on_close_request = np_close_request;
    win->icon = TASKBAR_ICON_NOTEPAD;
    wm_set_min_size(win, 470, 300);
    NP_WIN = win;
    desktop_set_status(0);
    NP_FOLLOW = 1;
    np_title(win);

    wm_add_button(win, "New", UI_BTN_SECONDARY, ui_rect(NP_PAD, 8, 64, 30), on_new, 0);
    wm_add_button(win, "Open", UI_BTN_SECONDARY, ui_rect(NP_PAD + 72, 8, 64, 30), on_open, 0);
    wm_add_button(win, "Save", UI_BTN_SECONDARY, ui_rect(NP_PAD + 144, 8, 64, 30), on_save, 0);
    wm_add_button(win, "Save As", UI_BTN_SECONDARY, ui_rect(NP_PAD + 216, 8, 84, 30), on_save_as, 0);
    NP_WRAP_BTN = wm_add_button(win, "Wrap", UI_BTN_SECONDARY, ui_rect(NP_PAD + 308, 8, 64, 30), on_wrap, 0);
    if (NP_WRAP_BTN < 0)
        NP_WRAP_BTN = 0;
    win->btn[NP_WRAP_BTN].kind = NP_WRAP ? UI_BTN_PRIMARY : UI_BTN_SECONDARY;
    NP_FIND_BTN = wm_add_button(win, "Find", UI_BTN_SECONDARY, ui_rect(NP_PAD + 380, 8, 64, 30), on_find, 0);
    if (NP_FIND_BTN < 0)
        NP_FIND_BTN = 0;
    NP_BAR = 0; /* a new window starts without the find bar */
    NP_FOCUS = 0;
    NP_BAR_DRAG = 0;
}

const char *notepad_open_file(const char *path)
{
    if (!path || !path[0])
        return "No file given";
    notepad_app_open(); /* the window, in front (a new one if none is open) */
    if (!NP_WIN)
        return NP_NOTE[0] ? NP_NOTE : "Could not open Notepad";
    if (NP_DIRTY) /* unsaved text: ask first; the file opens when the user agrees */
    {
        copy_str(NP_PENDING_PATH, (int)sizeof NP_PENDING_PATH, path);
        return ask_discard(NP_ACT_PATH) ? 0 : "Window limit reached - close one first";
    }
    const char *err = open_file(path);
    NP_NOTE = err ? err : "Opened";
    np_after_change();
    return err;
}

const AppInfo APP_NOTEPAD = {
    .name = "Notepad",
    .desc = "Write plain text",
    .icon = UI_ICON_NOTEPAD,
    .open = notepad_app_open,
};
