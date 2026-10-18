/* HarisUI: Single-line text editor (selection, clipboard, undo, mouse) and blinking caret. */
#include "ui_internal.h"
#include "timer.h"

#define DOUBLE_CLICK_MS 450

/* ---- small helpers ------------------------------------------- */
static int max_len(const UiEdit *e)
{
    return (e->max > 0 && e->max < UI_EDIT_MAX) ? e->max : UI_EDIT_MAX;
}

/* keep cursor / anchor / scroll inside the text (the struct may have been zeroed or edited by hand) */
static void fix(UiEdit *e)
{
    if (e->len < 0)
        e->len = 0;
    if (e->len > UI_EDIT_MAX)
        e->len = UI_EDIT_MAX;
    if (e->cursor < 0)
        e->cursor = 0;
    if (e->cursor > e->len)
        e->cursor = e->len;
    if (e->anchor < 0)
        e->anchor = 0;
    if (e->anchor > e->len)
        e->anchor = e->len;
    if (e->scroll < 0)
        e->scroll = 0;
    if (e->scroll > e->len)
        e->scroll = e->len;
}

int ui_edit_selection(const UiEdit *e, int *from, int *to)
{
    int a = e->anchor, b = e->cursor;
    if (a > e->len)
        a = e->len;
    if (b > e->len)
        b = e->len;
    if (a == b)
        return 0;
    if (a > b)
    {
        int t = a;
        a = b;
        b = t;
    }
    if (from)
        *from = a;
    if (to)
        *to = b;
    return 1;
}

int ui_edit_has_selection(const UiEdit *e) { return ui_edit_selection(e, 0, 0); }

static int is_word(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* A password field must not reveal where its words are: word jumps go to the ends. */
static int word_left(const UiEdit *e, int i)
{
    if (e->flags & UI_EDIT_PASSWORD)
        return 0;
    while (i > 0 && !is_word(e->text[i - 1]))
        i--;
    while (i > 0 && is_word(e->text[i - 1]))
        i--;
    return i;
}

static int word_right(const UiEdit *e, int i)
{
    if (e->flags & UI_EDIT_PASSWORD)
        return e->len;
    while (i < e->len && is_word(e->text[i]))
        i++;
    while (i < e->len && !is_word(e->text[i]))
        i++;
    return i;
}

/* ---- shared clipboard ---------------------------------------- */
static char CLIP[UI_EDIT_MAX + 1];
static int CLIP_LEN;

void ui_clipboard_set(const char *s, int len)
{
    if (len < 0)
        len = 0;
    if (len > UI_EDIT_MAX)
        len = UI_EDIT_MAX;
    for (int i = 0; i < len; i++)
        CLIP[i] = s[i];
    CLIP[len] = 0;
    CLIP_LEN = len;
}

int ui_clipboard_get(char *out, int cap)
{
    if (cap <= 0)
        return 0;
    int n = CLIP_LEN < cap - 1 ? CLIP_LEN : cap - 1;
    for (int i = 0; i < n; i++)
        out[i] = CLIP[i];
    out[n] = 0;
    return n;
}

/* ---- undo / redo (one step each way; typing or deleting in a row is one step) ---- */
static void copy_text(char *dst, const char *src, int len)
{
    for (int i = 0; i <= len; i++)
        dst[i] = src[i];
}

/* kind: 0 = always a new undo step, 1 = typing, 2 = Backspace/Delete: repeats of the same kind share a step */
static void begin_edit(UiEdit *e, int kind)
{
    if (kind == 0 || e->group != kind)
    {
        copy_text(e->undo_text, e->text, e->len);
        e->undo_len = e->len;
        e->undo_cur = e->cursor;
        e->undo_valid = 1;
        e->redo_valid = 0;
    }
    e->group = kind;
}

static int do_undo(UiEdit *e)
{
    if (!e->undo_valid || (e->flags & UI_EDIT_READONLY))
        return 0;
    copy_text(e->redo_text, e->text, e->len);
    e->redo_len = e->len;
    e->redo_cur = e->cursor;
    e->redo_valid = 1;
    copy_text(e->text, e->undo_text, e->undo_len);
    e->len = e->undo_len;
    e->cursor = e->anchor = e->undo_cur;
    e->undo_valid = 0;
    e->group = 0;
    fix(e);
    return 1;
}

static int do_redo(UiEdit *e)
{
    if (!e->redo_valid || (e->flags & UI_EDIT_READONLY))
        return 0;
    copy_text(e->undo_text, e->text, e->len);
    e->undo_len = e->len;
    e->undo_cur = e->cursor;
    e->undo_valid = 1;
    copy_text(e->text, e->redo_text, e->redo_len);
    e->len = e->redo_len;
    e->cursor = e->anchor = e->redo_cur;
    e->redo_valid = 0;
    e->group = 0;
    fix(e);
    return 1;
}

/* ---- editing primitives -------------------------------------- */
/* remove text[a .. b) and put the caret at a */
static void del_range(UiEdit *e, int a, int b)
{
    int n = b - a;
    for (int i = a; i <= e->len - n; i++)
        e->text[i] = e->text[i + n];
    e->len -= n;
    e->cursor = e->anchor = a;
}

/* type s at the caret (replacing the selection); only printable ASCII, respects max / numeric */
static int insert_str(UiEdit *e, const char *s, int kind)
{
    if (e->flags & UI_EDIT_READONLY)
        return 0;
    int a = 0, b = 0, has = ui_edit_selection(e, &a, &b);
    int room = max_len(e) - (e->len - (has ? b - a : 0));
    char tmp[UI_EDIT_MAX + 1];
    int n = 0;
    for (; *s && n < room; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126)
            continue;
        if ((e->flags & UI_EDIT_NUMERIC) && (c < '0' || c > '9'))
            continue;
        tmp[n++] = (char)c;
    }
    if (n == 0)
        return 0;
    if (has)
        e->group = 0; /* replacing a selection is a step of its own */
    begin_edit(e, kind);
    if (has)
        del_range(e, a, b);
    for (int i = e->len; i >= e->cursor; i--) /* includes the 0 at the end */
        e->text[i + n] = e->text[i];
    for (int i = 0; i < n; i++)
        e->text[e->cursor + i] = tmp[i];
    e->cursor += n;
    e->anchor = e->cursor;
    e->len += n;
    return n;
}

int ui_edit_insert(UiEdit *e, const char *s)
{
    fix(e);
    return insert_str(e, s, 0);
}

static int copy_selection(UiEdit *e)
{
    int a, b;
    if ((e->flags & UI_EDIT_PASSWORD) || !ui_edit_selection(e, &a, &b))
        return 0;
    ui_clipboard_set(e->text + a, b - a);
    return 1;
}

static int delete_range_undoable(UiEdit *e, int a, int b, int kind)
{
    if (a >= b || (e->flags & UI_EDIT_READONLY))
        return 0;
    begin_edit(e, kind);
    del_range(e, a, b);
    return 1;
}

/* move the caret; extend = keep the anchor (grow the selection) */
static int move_to(UiEdit *e, int pos, int extend)
{
    int oc = e->cursor, oa = e->anchor;
    if (pos < 0)
        pos = 0;
    if (pos > e->len)
        pos = e->len;
    e->cursor = pos;
    if (!extend)
        e->anchor = pos;
    e->group = 0;
    return e->cursor != oc || e->anchor != oa;
}

void ui_edit_select_all(UiEdit *e)
{
    fix(e);
    e->anchor = 0;
    e->cursor = e->len;
    e->group = 0;
}

void ui_edit_clear(UiEdit *e)
{
    fix(e);
    if (e->len > 0 && !(e->flags & UI_EDIT_READONLY))
        begin_edit(e, 0); /* "clear" can be undone with Ctrl+Z */
    e->text[0] = 0;
    e->len = 0;
    e->cursor = 0;
    e->anchor = 0;
    e->scroll = 0;
    e->group = 0;
    e->dragging = 0;
    e->clicks = 0;
}

void ui_edit_set_text(UiEdit *e, const char *s)
{
    int n = 0;
    for (; *s && n < max_len(e); s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126)
            continue;
        if ((e->flags & UI_EDIT_NUMERIC) && (c < '0' || c > '9'))
            continue;
        e->text[n++] = (char)c;
    }
    e->text[n] = 0;
    e->len = n;
    e->cursor = e->anchor = n;
    e->scroll = 0;
    e->group = 0;
    e->undo_valid = e->redo_valid = 0;
}

/* ---- keyboard ------------------------------------------------ */
int ui_edit_key(UiEdit *e, const KeyEvent *k)
{
    if (!k->pressed)
        return 0;
    fix(e);
    int ctrl = (k->mods & KMOD_CTRL) != 0, shift = (k->mods & KMOD_SHIFT) != 0;
    int ro = (e->flags & UI_EDIT_READONLY) != 0;
    int a = 0, b = 0, has = ui_edit_selection(e, &a, &b);

    /* clipboard, select all, undo */
    if (ctrl)
    {
        switch (k->key)
        {
        case 'a':
            if (has && a == 0 && b == e->len)
                return 0;
            if (e->len == 0)
                return 0;
            ui_edit_select_all(e);
            return 1;
        case 'c':
        case KEY_INSERT:
            copy_selection(e);
            return 0;
        case 'x':
            if (!has || ro || !copy_selection(e))
                return 0;
            return delete_range_undoable(e, a, b, 0);
        case 'v':
        {
            char tmp[UI_EDIT_MAX + 1];
            ui_clipboard_get(tmp, sizeof tmp);
            return insert_str(e, tmp, 0) > 0;
        }
        case 'z':
            return shift ? do_redo(e) : do_undo(e);
        case 'y':
            return do_redo(e);
        default:
            break;
        }
    }
    else if (shift && k->key == KEY_DELETE) /* Shift+Delete = cut */
    {
        if (!has || ro || !copy_selection(e))
            return 0;
        return delete_range_undoable(e, a, b, 0);
    }
    else if (shift && k->key == KEY_INSERT) /* Shift+Insert = paste */
    {
        char tmp[UI_EDIT_MAX + 1];
        ui_clipboard_get(tmp, sizeof tmp);
        return insert_str(e, tmp, 0) > 0;
    }

    switch (k->key)
    {
    case KEY_LEFT:
        if (!shift && has)
            return move_to(e, a, 0); /* collapse to the left end of the selection */
        return move_to(e, ctrl ? word_left(e, e->cursor) : e->cursor - 1, shift);
    case KEY_RIGHT:
        if (!shift && has)
            return move_to(e, b, 0);
        return move_to(e, ctrl ? word_right(e, e->cursor) : e->cursor + 1, shift);
    case KEY_HOME:
        return move_to(e, 0, shift);
    case KEY_END:
        return move_to(e, e->len, shift);
    case KEY_BACKSPACE:
        if (has)
            return delete_range_undoable(e, a, b, 0);
        if (e->cursor == 0)
            return 0;
        if (ctrl)
            return delete_range_undoable(e, word_left(e, e->cursor), e->cursor, 0);
        return delete_range_undoable(e, e->cursor - 1, e->cursor, 2);
    case KEY_DELETE:
        if (has)
            return delete_range_undoable(e, a, b, 0);
        if (e->cursor == e->len)
            return 0;
        if (ctrl)
            return delete_range_undoable(e, e->cursor, word_right(e, e->cursor), 0);
        return delete_range_undoable(e, e->cursor, e->cursor + 1, 2);
    default:
        break;
    }
    if (k->ch >= 32 && k->ch < 127)
    {
        char one[2] = {(char)k->ch, 0};
        return insert_str(e, one, 1) > 0;
    }
    return 0;
}

/* ---- blinking caret ---- */
#define CARET_BLINK_MS 530
static int CARET_ON = 1;
static unsigned long long CARET_EPOCH;

void ui_caret_restart(void)
{
    CARET_ON = 1;
    CARET_EPOCH = timer_ms();
}

int ui_caret_visible(void) { return CARET_ON; }

int ui_caret_update(void)
{
    if (!timer_ready())
        return 0; /* no clock: the caret just stays visible */
    int on = ((timer_ms() - CARET_EPOCH) / CARET_BLINK_MS) % 2 == 0;
    if (on == CARET_ON)
        return 0;
    CARET_ON = on;
    return 1;
}

/* ---- geometry shared by drawing and the mouse ---------------- */
static UiRect inner(const UiEdit *e)
{
    return ui_rect(e->field.x + 12, e->field.y, e->field.w - 24, e->field.h);
}

/* what is shown for text[from .. to): the text itself, or '*' in a password field */
static int show_chars(const UiEdit *e, int from, int to, char *out)
{
    int n = 0;
    for (int i = from; i < to && i < e->len; i++)
        out[n++] = (e->flags & UI_EDIT_PASSWORD) ? '*' : e->text[i];
    out[n] = 0;
    return n;
}

/* pixel width of text[from .. to) */
static int edit_width(const UiEdit *e, int from, int to)
{
    char tmp[UI_EDIT_MAX + 1];
    show_chars(e, from, to, tmp);
    return gfx_text_w(THEME->font_body, tmp);
}

/* the caret position (0..len) nearest to pixel x */
static int hit_index(const UiEdit *e, int x)
{
    int rel = x - inner(e).x, prev = 0;
    for (int i = e->scroll; i < e->len; i++)
    {
        int w = edit_width(e, e->scroll, i + 1);
        if (rel < (prev + w) / 2)
            return i;
        prev = w;
    }
    return e->len;
}

/* ---- mouse --------------------------------------------------- */
static void select_word_at(UiEdit *e, int idx)
{
    int c = idx;
    if ((c >= e->len || !is_word(e->text[c])) && c > 0 && is_word(e->text[c - 1]))
        c--;
    int a = c, b = c;
    if (c < e->len && is_word(e->text[c]))
    {
        while (a > 0 && is_word(e->text[a - 1]))
            a--;
        while (b < e->len && is_word(e->text[b]))
            b++;
    }
    else
        b = c < e->len ? c + 1 : c;
    e->anchor = a;
    e->cursor = b;
}

int ui_edit_mouse(UiEdit *e, int x, int y, unsigned buttons)
{
    (void)y;
    fix(e);
    int down = (buttons & 1) != 0;
    if (down && !e->dragging)
    {
        /* press: place the caret; a second / third click in a row selects a word / everything */
        e->dragging = 1;
        e->group = 0;
        int dx = x - e->click_x;
        if (dx < 0)
            dx = -dx;
        unsigned long long now = timer_ready() ? timer_ms() : 0;
        if (timer_ready() && e->clicks > 0 && now - e->click_t <= DOUBLE_CLICK_MS && dx <= 6)
            e->clicks = e->clicks >= 3 ? 1 : e->clicks + 1;
        else
            e->clicks = 1;
        e->click_t = now;
        e->click_x = x;
        int idx = hit_index(e, x);
        if (e->clicks >= 3 || (e->clicks == 2 && (e->flags & UI_EDIT_PASSWORD)))
        {
            e->anchor = 0;
            e->cursor = e->len;
        }
        else if (e->clicks == 2)
            select_word_at(e, idx);
        else
            e->cursor = e->anchor = idx;
        return 1; /* caret and selection changed: redraw */
    }
    if (down && e->dragging)
    {
        if (e->clicks > 1)
            return 0; /* after a double click the selection is kept */
        UiRect in = inner(e);
        int idx;
        if (x < in.x)
            idx = e->cursor > e->scroll ? e->scroll : (e->cursor > 0 ? e->cursor - 1 : 0);
        else if (x > in.x + in.w)
        {
            int last = hit_index(e, in.x + in.w);
            idx = e->cursor < last ? last : (e->cursor < e->len ? e->cursor + 1 : e->len);
        }
        else
            idx = hit_index(e, x);
        if (idx == e->cursor)
            return 0;
        e->cursor = idx; /* the anchor stays: this grows the selection */
        return 1;
    }
    if (!down && e->dragging)
        e->dragging = 0;
    return 0;
}

/* ---- drawing ------------------------------------------------- */
static void draw_segment(const UiEdit *e, int from, int to, int x0, int y, UiRgb color)
{
    if (from >= to)
        return;
    char tmp[UI_EDIT_MAX + 1];
    show_chars(e, from, to, tmp);
    ui_text_color(x0 + edit_width(e, e->scroll, from), y, tmp, UI_TEXT_BODY, color);
}

void ui_edit_draw(UiRect r, UiEdit *e, const char *placeholder, int focused)
{
    e->field = r;
    fix(e);
    const UiTheme *t = THEME;
    ui_text_field(r, "", e->len ? 0 : placeholder, focused);
    UiRect in = inner(e);
    if (in.w <= 4)
        return;
    int avail = in.w - 3;
    int cur = e->cursor;

    /* scroll: keep the caret visible, and use free room on the right */
    if (e->scroll > cur)
        e->scroll = cur;
    while (e->scroll < cur && edit_width(e, e->scroll, cur) > avail)
        e->scroll++;
    while (e->scroll > 0 && edit_width(e, e->scroll - 1, e->len) <= avail)
        e->scroll--;

    int a = 0, b = 0, has = ui_edit_selection(e, &a, &b);
    int ty = r.y + (r.h - ui_text_height(UI_TEXT_BODY)) / 2;
    int x = in.x;
    if (e->len)
    {
        GfxClip saved = gfx_get_clip();
        gfx_clip_intersect(in.x, in.y, in.w, in.h);
        if (has && b > e->scroll)
        {
            int sa = a < e->scroll ? e->scroll : a;
            int x0 = x + edit_width(e, e->scroll, sa), x1 = x + edit_width(e, e->scroll, b);
            UiRgb fill = focused ? t->accent : ui_rgb_mix(t->surface_alt, t->border, 200);
            gfx_fill_rect(x0, r.y + 6, x1 - x0, r.h - 12, ui_color(fill));
            draw_segment(e, e->scroll, sa, x, ty, t->text);
            draw_segment(e, sa, b, x, ty, focused ? t->text_on_accent : t->text);
            draw_segment(e, b, e->len, x, ty, t->text);
        }
        else
            draw_segment(e, e->scroll, e->len, x, ty, t->text);
        gfx_restore_clip(saved);
    }
    if (focused && CARET_ON && !has && !(e->flags & UI_EDIT_READONLY))
    {
        int cx = x + edit_width(e, e->scroll, cur);
        if (cx > in.x + in.w - 2)
            cx = in.x + in.w - 2;
        gfx_fill_rect(cx, r.y + 7, 2, r.h - 14, ui_color(t->accent_hi));
    }
}
