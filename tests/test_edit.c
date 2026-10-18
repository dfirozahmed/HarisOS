/* Host-side tests for the HarisUI text input (kernel/ui/ui_edit.c). Drawing and the clock are stubbed. */
#include "ui_internal.h"
#include "timer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- stubs ---- */
static UiTheme THEME_STORE;
const UiTheme *THEME = &THEME_STORE;
static unsigned long long NOW;
void timer_init(void) {}
int timer_ready(void) { return 1; }
unsigned long long timer_ms(void) { return NOW; }
int gfx_text_w(const Font *f, const char *s) { (void)f; return (int)strlen(s) * 8; } /* 8 px per character */
UiRect ui_rect(int x, int y, int w, int h) { UiRect r = {x, y, w, h}; return r; }
int ui_text_height(UiTextStyle st) { (void)st; return 16; }
void ui_text_field(UiRect r, const char *t, const char *p, int f) { (void)r; (void)t; (void)p; (void)f; }
void ui_text_color(int x, int y, const char *s, UiTextStyle st, UiRgb c) { (void)x; (void)y; (void)s; (void)st; (void)c; }
UiRgb ui_rgb_mix(UiRgb a, UiRgb b, unsigned char t) { (void)b; (void)t; return a; }
GfxColor ui_color(UiRgb c) { (void)c; return 0; }
void gfx_fill_rect(int x, int y, int w, int h, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
GfxClip gfx_get_clip(void) { GfxClip c; memset(&c, 0, sizeof c); return c; }
void gfx_clip_intersect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void gfx_restore_clip(GfxClip c) { (void)c; }

/* ---- helpers ---- */
static UiEdit E;
static int press(int key, unsigned mods)
{
    KeyEvent k = {key, 0, 1, mods};
    if (key >= 32 && key < 127 && !(mods & (KMOD_CTRL | KMOD_ALT)))
    {
        int ch = key;
        if ((mods & KMOD_SHIFT) && key >= 'a' && key <= 'z')
            ch = key - 32;
        k.ch = ch;
    }
    return ui_edit_key(&E, &k);
}
static void type(const char *s)
{
    for (; *s; s++)
        press(*s, 0);
}
static void expect(const char *text, int cursor)
{
    if (strcmp(E.text, text) != 0 || E.cursor != cursor)
    {
        printf("FAIL: expected \"%s\" cursor %d, got \"%s\" cursor %d\n", text, cursor, E.text, E.cursor);
        assert(0);
    }
    assert(E.len == (int)strlen(E.text));
}
static void fresh(void)
{
    memset(&E, 0, sizeof E);
}

int main(void)
{
    /* basics still work */
    fresh();
    type("hello");
    expect("hello", 5);
    press(KEY_LEFT, 0);
    press(KEY_BACKSPACE, 0);
    expect("helo", 3);
    press(KEY_DELETE, 0);
    expect("hel", 3);
    press(KEY_HOME, 0);
    press(KEY_DELETE, 0);
    expect("el", 0);

    /* Shift+arrows select, typing replaces */
    fresh();
    type("hello world");
    press(KEY_HOME, 0);
    for (int i = 0; i < 5; i++)
        press(KEY_RIGHT, KMOD_SHIFT);
    assert(ui_edit_has_selection(&E));
    int a, b;
    assert(ui_edit_selection(&E, &a, &b) && a == 0 && b == 5);
    type("X");
    expect("X world", 1);
    assert(!ui_edit_has_selection(&E));

    /* Left/Right collapse a selection */
    press(KEY_END, KMOD_SHIFT);
    press(KEY_LEFT, 0);
    expect("X world", 1);
    assert(!ui_edit_has_selection(&E));
    press(KEY_END, KMOD_SHIFT);
    press(KEY_RIGHT, 0);
    expect("X world", 7);

    /* Ctrl+Left / Right word jumps */
    fresh();
    type("one two  three");
    press(KEY_LEFT, KMOD_CTRL);
    expect("one two  three", 9);
    press(KEY_LEFT, KMOD_CTRL);
    expect("one two  three", 4);
    press(KEY_LEFT, KMOD_CTRL);
    expect("one two  three", 0);
    press(KEY_RIGHT, KMOD_CTRL);
    expect("one two  three", 4);
    press(KEY_RIGHT, KMOD_CTRL);
    expect("one two  three", 9);
    press(KEY_RIGHT, KMOD_CTRL);
    expect("one two  three", 14);
    press(KEY_LEFT, KMOD_CTRL | KMOD_SHIFT);
    assert(ui_edit_selection(&E, &a, &b) && a == 9 && b == 14);

    /* Ctrl+Backspace / Ctrl+Delete */
    fresh();
    type("one two three");
    press(KEY_BACKSPACE, KMOD_CTRL);
    expect("one two ", 8);
    press(KEY_LEFT, KMOD_CTRL);
    press(KEY_DELETE, KMOD_CTRL);
    expect("one ", 4);

    /* select all, copy, cut, paste */
    fresh();
    type("copy me");
    assert(press('a', KMOD_CTRL));
    assert(ui_edit_selection(&E, &a, &b) && a == 0 && b == 7);
    assert(!press('c', KMOD_CTRL)); /* copy changes nothing on screen */
    press(KEY_RIGHT, 0);
    expect("copy me", 7);
    press('v', KMOD_CTRL);
    expect("copy mecopy me", 14);
    press('a', KMOD_CTRL);
    press('x', KMOD_CTRL);
    expect("", 0);
    press('v', KMOD_CTRL);
    expect("copy mecopy me", 14);
    press('z', KMOD_CTRL); /* undo the paste */
    expect("", 0);
    press('z', KMOD_CTRL); /* undo does not go further back than one step */
    expect("", 0);
    press('y', KMOD_CTRL); /* redo */
    expect("copy mecopy me", 14);
    press('z', KMOD_CTRL);
    press('z', KMOD_CTRL | KMOD_SHIFT); /* Ctrl+Shift+Z = redo */
    expect("copy mecopy me", 14);

    /* Shift+Delete = cut, Ctrl+Insert = copy, Shift+Insert = paste */
    fresh();
    type("abcdef");
    press(KEY_LEFT, KMOD_SHIFT);
    press(KEY_LEFT, KMOD_SHIFT);
    press(KEY_DELETE, KMOD_SHIFT);
    expect("abcd", 4);
    press(KEY_INSERT, KMOD_SHIFT);
    expect("abcdef", 6);
    press(KEY_LEFT, KMOD_SHIFT);
    press(KEY_INSERT, KMOD_CTRL);
    press(KEY_END, 0);
    press(KEY_INSERT, KMOD_SHIFT);
    expect("abcdeff", 7);

    /* typing is one undo step; so is a run of Backspaces */
    fresh();
    type("hello");
    press('z', KMOD_CTRL);
    expect("", 0);
    press('y', KMOD_CTRL);
    expect("hello", 5);
    press(KEY_BACKSPACE, 0);
    press(KEY_BACKSPACE, 0);
    press(KEY_BACKSPACE, 0);
    expect("he", 2);
    press('z', KMOD_CTRL);
    expect("hello", 5);

    /* Escape-style clear can be undone */
    ui_edit_clear(&E);
    expect("", 0);
    press('z', KMOD_CTRL);
    expect("hello", 5);

    /* max length */
    fresh();
    E.max = 5;
    type("abcdefgh");
    expect("abcde", 5);
    ui_edit_clear(&E);
    ui_clipboard_set("123456789", 9);
    press('v', KMOD_CTRL);
    expect("12345", 5);

    /* numeric */
    fresh();
    E.flags = UI_EDIT_NUMERIC;
    type("a1b2c3");
    expect("123", 3);

    /* read-only: move / select / copy only */
    fresh();
    ui_edit_set_text(&E, "locked");
    E.flags = UI_EDIT_READONLY;
    type("x");
    press(KEY_BACKSPACE, 0);
    press(KEY_DELETE, KMOD_CTRL);
    expect("locked", 6);
    press('a', KMOD_CTRL);
    press('x', KMOD_CTRL);
    expect("locked", 6);
    press('c', KMOD_CTRL);
    char tmp[32];
    ui_clipboard_get(tmp, sizeof tmp);
    assert(strcmp(tmp, "locked") == 0);

    /* password: no copy, no word jumps */
    fresh();
    E.flags = UI_EDIT_PASSWORD;
    ui_clipboard_set("keep", 4);
    type("my secret");
    press('a', KMOD_CTRL);
    press('c', KMOD_CTRL);
    ui_clipboard_get(tmp, sizeof tmp);
    assert(strcmp(tmp, "keep") == 0);
    press(KEY_RIGHT, 0);
    press(KEY_LEFT, KMOD_CTRL);
    expect("my secret", 0);

    /* insert helper replaces the selection and filters non-printables */
    fresh();
    type("hello world");
    press(KEY_HOME, 0);
    for (int i = 0; i < 5; i++)
        press(KEY_RIGHT, KMOD_SHIFT);
    assert(ui_edit_insert(&E, "bye\n") == 3);
    expect("bye world", 3);

    /* mouse: drawing first, so the field and scroll are known (8 px per char, text starts at x = 12) */
    fresh();
    ui_edit_set_text(&E, "hello big world");
    UiRect r = {0, 0, 200, 34};
    ui_edit_draw(r, &E, "", 1);
    assert(E.scroll == 0);
    NOW = 10000;
    ui_edit_mouse(&E, 12 + 8 * 3 + 1, 10, 1); /* press inside the 4th char (left half-ish) */
    assert(E.cursor == 3 && E.anchor == 3);
    ui_edit_mouse(&E, 12 + 8 * 8 + 1, 10, 1); /* drag */
    assert(E.cursor == 8 && E.anchor == 3);
    ui_edit_mouse(&E, 12 + 8 * 8 + 1, 10, 0); /* release */
    assert(!E.dragging);
    assert(ui_edit_selection(&E, &a, &b) && a == 3 && b == 8);

    /* double click selects a word, triple click everything */
    NOW = 20000;
    ui_edit_mouse(&E, 12 + 8 * 7, 10, 1);
    ui_edit_mouse(&E, 12 + 8 * 7, 10, 0);
    assert(!ui_edit_has_selection(&E));
    NOW += 200;
    ui_edit_mouse(&E, 12 + 8 * 7, 10, 1);
    ui_edit_mouse(&E, 12 + 8 * 7, 10, 0);
    assert(ui_edit_selection(&E, &a, &b) && a == 6 && b == 9); /* "big" */
    NOW += 200;
    ui_edit_mouse(&E, 12 + 8 * 7, 10, 1);
    ui_edit_mouse(&E, 12 + 8 * 7, 10, 0);
    assert(ui_edit_selection(&E, &a, &b) && a == 0 && b == E.len);
    NOW += 2000; /* slow second click = a plain click again */
    ui_edit_mouse(&E, 12 + 8 * 2, 10, 1);
    ui_edit_mouse(&E, 12 + 8 * 2, 10, 0);
    assert(!ui_edit_has_selection(&E) && E.cursor == 2);

    /* dragging past the right edge keeps extending */
    ui_edit_mouse(&E, 12, 10, 1);
    for (int i = 0; i < 20; i++)
        ui_edit_mouse(&E, 500, 10, 1);
    ui_edit_mouse(&E, 500, 10, 0);
    assert(E.cursor == E.len && E.anchor == 0);

    /* long text scrolls so the caret stays visible, and clicks use the scrolled position */
    fresh();
    ui_edit_set_text(&E, "0123456789012345678901234567890123456789");
    ui_edit_draw(r, &E, "", 1);
    assert(E.scroll > 0);
    int vis = (r.w - 24 - 3) / 8;
    assert(E.len - E.scroll <= vis);
    NOW += 5000;
    ui_edit_mouse(&E, 12 + 8 * 2 + 1, 10, 1);
    ui_edit_mouse(&E, 12 + 8 * 2 + 1, 10, 0);
    assert(E.cursor == E.scroll + 2);

    /* stale / hand-edited state is repaired */
    fresh();
    ui_edit_set_text(&E, "abc");
    E.cursor = 99;
    E.anchor = -4;
    press(KEY_LEFT, 0);
    assert(E.cursor >= 0 && E.cursor <= E.len);

    printf("edit tests passed\n");
    return 0;
}
