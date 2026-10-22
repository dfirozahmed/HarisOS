/* Host-side tests for the Notepad text buffer, selection and undo / redo (apps/notepad/notepad_app.c).
 * The app file is included so its static functions can be called; drawing and the window manager are stubbed. */
#include "../apps/notepad/notepad_app.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- stubs (defined after the include, so the compiler checks them against the real prototypes) ---- */
static UiTheme THEME_STORE;
static char CLIP_STORE[NP_MAX + 1];
static int CLIP_N;
void desktop_set_status(const char *s) { (void)s; }
void gfx_clip_intersect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void gfx_fill_rect(int x, int y, int w, int h, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void gfx_fill_rect_alpha(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    (void)x; (void)y; (void)w; (void)h; (void)r; (void)g; (void)b; (void)a;
}
void gfx_fill_round_rect(int x, int y, int w, int h, int r, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)c; }
GfxClip gfx_get_clip(void) { GfxClip c = {0, 0, 0, 0}; return c; }
unsigned gfx_height(void) { return 768; }
unsigned gfx_width(void) { return 1024; }
void gfx_restore_clip(GfxClip c) { (void)c; }
void gfx_round_rect(int x, int y, int w, int h, int r, int t, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)t; (void)c; }
void gfx_text(const Font *f, int x, int y, const char *s, GfxColor c) { (void)f; (void)x; (void)y; (void)s; (void)c; }
int gfx_text_fit(const Font *f, const char *s, int max_w)
{
    (void)f;
    int n = (int)strlen(s), fit = max_w / 8; /* 8 px per character */
    return fit < n ? (fit < 0 ? 0 : fit) : n;
}
int gfx_text_w(const Font *f, const char *s) { (void)f; return (int)strlen(s) * 8; }
void ui_caret_restart(void) {}
int ui_caret_visible(void) { return 1; }
void ui_clipboard_set(const char *s, int len)
{
    memcpy(CLIP_STORE, s, (size_t)len);
    CLIP_N = len;
}
int ui_clipboard_get(char *out, int cap)
{
    int n = CLIP_N < cap - 1 ? CLIP_N : cap - 1;
    memcpy(out, CLIP_STORE, (size_t)n);
    out[n] = 0;
    return n;
}
GfxColor ui_color(UiRgb c) { (void)c; return 0; }
int ui_contains(UiRect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
const Font *ui_font(UiTextStyle s) { (void)s; return 0; }
UiRect ui_rect(int x, int y, int w, int h) { UiRect r = {x, y, w, h}; return r; }
UiRect ui_inset(UiRect r, int a) { return ui_rect(r.x + a, r.y + a, r.w - 2 * a, r.h - 2 * a); }
int ui_text_height(UiTextStyle s) { (void)s; return 16; }
void ui_text_in_color(UiRect r, const char *s, UiTextStyle st, UiAlign al, UiRgb c) { (void)r; (void)s; (void)st; (void)al; (void)c; }
const UiTheme *ui_theme(void) { return &THEME_STORE; }
void wm_activate(Window *w) { (void)w; }
void wm_add_damage(UiRect r) { (void)r; }
UiRect wm_content(const Window *w) { (void)w; return ui_rect(0, 0, 400, 300); }
Window *wm_create(const char *t, int x, int y, int w, int h, WmDrawFn d, void *u)
{
    (void)t; (void)x; (void)y; (void)w; (void)h; (void)d; (void)u;
    return 0;
}
void wm_invalidate(void) {}
void wm_key_partial(void) {}
void wm_set_min_size(Window *w, int a, int b) { (void)w; (void)a; (void)b; }

/* ---- helpers ---- */
static void reset(void)
{
    NP_LEN = NP_CUR = NP_ANCHOR = 0;
    NP_UN = NP_UPOS = 0;
    NP_LASTKIND = NP_KIND_NONE;
    NP_EDITS = 0;
}

static void type(const char *s)
{
    for (; *s; s++)
        insert_text(s, 1);
}

static int is(const char *expect)
{
    return NP_LEN == (int)strlen(expect) && memcmp(NP_BUF, expect, (size_t)NP_LEN) == 0;
}

static void end_run(void) { NP_LASTKIND = NP_KIND_NONE; } /* what a movement key or a click does */

int main(void)
{
    /* typing in a row is one undo step */
    reset();
    type("hello");
    assert(is("hello") && NP_UN == 1);
    undo();
    assert(is("") && NP_CUR == 0);
    redo();
    assert(is("hello") && NP_CUR == 5);

    /* a movement in between starts a new step; Enter is a step of its own */
    reset();
    type("ab");
    end_run();
    type("cd");
    insert_text("\n", 1);
    type("e");
    assert(is("abcd\ne") && NP_UN == 4);
    undo();
    assert(is("abcd\n"));
    undo();
    assert(is("abcd"));
    undo();
    assert(is("ab"));
    undo();
    assert(is(""));
    undo(); /* nothing left: harmless */
    assert(is("") && NP_UPOS == 0);

    /* Backspace in a row is one step, and undo puts the text back in the right order */
    reset();
    type("abcdef");
    end_run();
    for (int i = 0; i < 3; i++)
        edit_replace(NP_CUR - 1, 1, 0, 0, NP_KIND_BACK);
    assert(is("abc") && NP_UN == 2);
    undo();
    assert(is("abcdef") && NP_CUR == 6);
    redo();
    assert(is("abc") && NP_CUR == 3);

    /* Delete in a row, from the middle */
    reset();
    type("abcdef");
    NP_CUR = NP_ANCHOR = 2;
    end_run();
    for (int i = 0; i < 3; i++)
        edit_replace(NP_CUR, 1, 0, 0, NP_KIND_FWD);
    assert(is("abf") && NP_UN == 2);
    undo();
    assert(is("abcdef") && NP_CUR == 2);
    redo();
    assert(is("abf") && NP_CUR == 2);

    /* typing over a selection is one step; undo selects the old text again */
    reset();
    type("hello world");
    NP_ANCHOR = 6;
    NP_CUR = 11;
    end_run();
    insert_text("x", 1);
    assert(is("hello x") && NP_CUR == 7 && NP_ANCHOR == 7);
    undo();
    assert(is("hello world") && NP_ANCHOR == 6 && NP_CUR == 11);
    redo();
    assert(is("hello x"));

    /* cut and paste, with Tab and carriage return in the clipboard */
    reset();
    type("one two");
    NP_ANCHOR = 0;
    NP_CUR = 3;
    copy_selection();
    assert(CLIP_N == 3 && memcmp(CLIP_STORE, "one", 3) == 0);
    delete_selection();
    assert(is(" two"));
    undo();
    assert(is("one two"));
    CLIP_N = 5;
    memcpy(CLIP_STORE, "a\tb\r\n", 5);
    NP_CUR = NP_ANCHOR = 7;
    end_run();
    paste();
    assert(is("one twoa    b\n"));
    undo();
    assert(is("one two"));

    /* a new edit ends the redo history */
    reset();
    type("abc");
    end_run();
    type("def");
    undo();
    assert(is("abc") && NP_UPOS == 1 && NP_UN == 2);
    type("X");
    assert(is("abcX") && NP_UN == 2 && NP_UPOS == 2);
    redo(); /* nothing to redo */
    assert(is("abcX"));

    /* the buffer limit: a paste is cut to what fits and still undoes cleanly */
    reset();
    for (int i = 0; i < NP_MAX - 2; i++)
        NP_BUF[i] = 'a';
    NP_LEN = NP_MAX - 2;
    NP_CUR = NP_ANCHOR = NP_LEN;
    insert_text("12345", 5);
    assert(NP_LEN == NP_MAX && NP_BUF[NP_MAX - 1] == '2');
    undo();
    assert(NP_LEN == NP_MAX - 2);

    /* random edits: undo everything gives the empty text, redo everything gives the final text */
    unsigned seed = 12345;
    for (int round = 0; round < 200; round++)
    {
        reset();
        int steps = 20 + (int)(seed % 60);
        for (int i = 0; i < steps; i++)
        {
            seed = seed * 1103515245u + 12345u;
            int r = (int)((seed >> 16) % 9);
            if (r < 4)
            {
                char c = (char)('a' + (seed >> 8) % 26);
                insert_text(&c, 1);
            }
            else if (r == 4 && NP_CUR > 0)
                edit_replace(NP_CUR - 1, 1, 0, 0, NP_KIND_BACK);
            else if (r == 5 && NP_CUR < NP_LEN)
                edit_replace(NP_CUR, 1, 0, 0, NP_KIND_FWD);
            else if (r == 6)
                insert_text("\n", 1);
            else if (r == 7)
            {
                NP_CUR = NP_ANCHOR = NP_LEN ? (int)((seed >> 4) % (unsigned)(NP_LEN + 1)) : 0;
                end_run();
            }
            else if (r == 8 && NP_LEN > 2)
            {
                NP_ANCHOR = (int)((seed >> 3) % (unsigned)NP_LEN);
                NP_CUR = (int)((seed >> 9) % (unsigned)NP_LEN);
                insert_text("zz", 2);
            }
        }
        static char final_text[NP_MAX + 1];
        int final_len = NP_LEN;
        memcpy(final_text, NP_BUF, (size_t)NP_LEN);
        while (NP_UPOS > 0)
            undo();
        assert(NP_LEN == 0);
        while (NP_UPOS < NP_UN)
            redo();
        assert(NP_LEN == final_len && memcmp(NP_BUF, final_text, (size_t)final_len) == 0);
    }

    /* more edits than records: the oldest are forgotten, the newest still undo */
    reset();
    for (int i = 0; i < NP_UNDO_OPS + 50; i++)
    {
        insert_text("\n", 1);
        if (NP_LEN > 3000)
            NP_LEN = NP_CUR = NP_ANCHOR = 0, NP_UN = NP_UPOS = 0;
    }
    assert(NP_UN <= NP_UNDO_OPS);
    int len_now = NP_LEN;
    undo();
    assert(NP_LEN == len_now - 1);
    redo();
    assert(NP_LEN == len_now);

    printf("notepad tests passed\n");
    return 0;
}
