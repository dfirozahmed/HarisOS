/* Host-side tests for the HarisUI pop-up menu (kernel/ui/ui_menu.c): layout, hit testing, mouse and key handling. Drawing is recorded. */
#include "ui_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- stubs ---- */
static UiTheme THEME_STORE;
const UiTheme *THEME = &THEME_STORE;
UiRect ui_rect(int x, int y, int w, int h) { UiRect r = {x, y, w, h}; return r; }
int ui_contains(UiRect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
int ui_text_width(const char *s, UiTextStyle st) { (void)st; return (int)strlen(s) * 8; }
GfxColor ui_color(UiRgb c) { (void)c; return 0; }
static int SHADOWS, ALPHAS, SEPS, ICONS, LABELS, DIM_LABELS;
void gfx_shadow(int x, int y, int w, int h, int r, int b, int dx, int dy, unsigned char s) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)b; (void)dx; (void)dy; (void)s; SHADOWS++; }
void gfx_fill_round_rect(int x, int y, int w, int h, int r, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)c; }
void gfx_round_rect(int x, int y, int w, int h, int r, int t, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)t; (void)c; }
void gfx_fill_round_rect_alpha(int x, int y, int w, int h, int r, GfxColor c, unsigned char a) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)c; (void)a; ALPHAS++; }
void ui_separator(int x, int y, int w) { (void)x; (void)y; (void)w; SEPS++; }
void ui_icon(UiIcon i, int cx, int cy, int size, UiRgb t) { (void)i; (void)cx; (void)cy; (void)size; (void)t; ICONS++; }
void ui_text_in_color(UiRect r, const char *s, UiTextStyle st, UiAlign a, UiRgb c)
{
    (void)r; (void)s; (void)st; (void)a;
    LABELS++;
    if (c.r == 1) /* the theme's text_dim is marked with r == 1 below */
        DIM_LABELS++;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

enum { A = 10, B, C, D, E };

static UiRect AREA = {0, 0, 600, 400};

static void build(UiMenu *m)
{
    ui_menu_clear(m);
    ui_menu_add(m, A, "Open", "Enter", 0);
    ui_menu_separator(m);
    ui_menu_add(m, B, "Cut", "Ctrl+X", UI_MENU_DISABLED);
    ui_menu_add(m, C, "Copy", "Ctrl+C", 0);
    ui_menu_separator(m);
    ui_menu_add(m, D, "Delete", "Del", UI_MENU_DANGER);
}

static KeyEvent key(int k) { KeyEvent e = {k, 0, 1, 0}; return e; }

int main(void)
{
    THEME_STORE.text_dim.r = 1;
    UiMenu m;

    /* separators: none first, none doubled, none last */
    ui_menu_clear(&m);
    CHECK(ui_menu_separator(&m) == 0 && m.count == 0);
    ui_menu_add(&m, A, "One", 0, 0);
    ui_menu_separator(&m);
    ui_menu_separator(&m);
    CHECK(m.count == 2);
    ui_menu_open(&m, 10, 10, AREA);
    CHECK(m.count == 1 && m.open); /* the trailing one is dropped */
    ui_menu_clear(&m);
    ui_menu_open(&m, 10, 10, AREA);
    CHECK(!m.open); /* nothing to show */
    ui_menu_clear(&m);
    CHECK(ui_menu_add(&m, 1, 0, 0, 0) == -1); /* a label is required: a separator is made by ui_menu_separator */
    for (int i = 0; i < UI_MENU_MAX; i++)
        CHECK(ui_menu_add(&m, i, "x", 0, 0) == 0);
    CHECK(ui_menu_add(&m, 99, "x", 0, 0) == -1);

    /* layout: size from the items, placed at the click */
    build(&m);
    ui_menu_open(&m, 100, 50, AREA);
    CHECK(m.open && m.x == 100 && m.y == 50);
    CHECK(m.h == 6 * 2 + 30 * 4 + 9 * 2); /* 4 rows and 2 separators */
    CHECK(m.w >= 180);
    UiRect r0 = ui_menu_item_rect(&m, 0), r1 = ui_menu_item_rect(&m, 1), r2 = ui_menu_item_rect(&m, 2);
    CHECK(r0.y == 56 && r0.h == 30 && r1.y == 86 && r1.h == 9 && r2.y == 95 && r2.h == 30);
    CHECK(r0.x == 106 && r0.w == m.w - 12);

    /* a wide label makes the menu wider than the minimum */
    ui_menu_clear(&m);
    ui_menu_add(&m, A, "A rather long menu entry here", "Ctrl+Shift+N", 0);
    ui_menu_open(&m, 0, 4, AREA);
    CHECK(m.w == 12 + 12 + 29 * 8 + 24 + 12 * 8 + 12);

    /* kept inside the area: right and bottom edge, then left and top */
    build(&m);
    ui_menu_open(&m, 590, 390, AREA);
    CHECK(m.x + m.w == 600 - 4 && m.y + m.h == 400 - 4);
    ui_menu_open(&m, -50, -50, AREA);
    CHECK(m.x == 4 && m.y == 4);
    UiRect small = {10, 20, 150, 120}; /* an area narrower than the menu: the menu shrinks to fit, never leaves it */
    ui_menu_open(&m, 500, 500, small);
    CHECK(m.x >= small.x + 4 && m.x + m.w <= small.x + small.w - 4);
    CHECK(m.y >= small.y + 4 && m.y + m.h <= small.y + small.h - 4);

    /* hit testing: separators and disabled items are not hit */
    build(&m);
    ui_menu_open(&m, 100, 50, AREA);
    CHECK(ui_menu_hit(&m, 120, 70) == 0);   /* Open */
    CHECK(ui_menu_hit(&m, 120, 90) == -1);  /* separator */
    CHECK(ui_menu_hit(&m, 120, 110) == -1); /* Cut: disabled */
    CHECK(ui_menu_hit(&m, 120, 140) == 3);  /* Copy */
    CHECK(ui_menu_hit(&m, 50, 70) == -1);   /* outside */
    CHECK(ui_menu_hit(&m, 101, 70) == -1);  /* in the padding, left of the rows */
    ui_menu_close(&m);
    CHECK(ui_menu_hit(&m, 120, 70) == -1);  /* closed */

    /* hover */
    ui_menu_open(&m, 100, 50, AREA);
    CHECK(ui_menu_hover(&m, 120, 70) && m.hover == 0);
    CHECK(!ui_menu_hover(&m, 130, 72)); /* same item */
    CHECK(ui_menu_hover(&m, 120, 110) && m.hover == -1); /* a disabled item takes no highlight */
    ui_menu_hover(&m, 120, 70);
    CHECK(ui_menu_hover(&m, -1, -1) && m.hover == -1); /* the pointer left the window */
    ui_menu_close(&m);
    CHECK(!ui_menu_hover(&m, 120, 70));

    /* mouse: press and release on the same item runs it, once */
    int id = -1;
    ui_menu_open(&m, 100, 50, AREA);
    CHECK(ui_menu_press(&m, 120, 140) == 1 && m.press == 3 && m.press_on);
    CHECK(ui_menu_release(&m, 120, 142, 1, &id) == 0 && m.open); /* still held on it: nothing changed */
    CHECK(ui_menu_release(&m, 120, 200, 1, &id) == 1 && !m.press_on); /* slid off: the pressed look goes */
    CHECK(ui_menu_release(&m, 120, 140, 1, &id) == 1 && m.press_on);  /* and back */
    CHECK(ui_menu_release(&m, 120, 140, 0, &id) == 2 && id == C && !m.open && m.press == -1);
    CHECK(ui_menu_release(&m, 120, 140, 0, &id) == 0); /* nothing pressed any more */

    /* released away from the pressed item: runs nothing, the menu stays */
    id = -1;
    ui_menu_open(&m, 100, 50, AREA);
    ui_menu_press(&m, 120, 70);
    CHECK(ui_menu_release(&m, 120, 140, 0, &id) == 1 && id == -1 && m.open && m.press == -1);

    /* press on a separator, a disabled item or the padding: swallowed, nothing pressed, menu stays */
    CHECK(ui_menu_press(&m, 120, 90) == 1 && m.press == -1 && m.open);
    CHECK(ui_menu_press(&m, 120, 110) == 1 && m.press == -1 && m.open);
    CHECK(ui_menu_release(&m, 120, 110, 0, &id) == 0);

    /* press outside: closes, reports it */
    CHECK(ui_menu_press(&m, 5, 5) == 0 && !m.open);
    CHECK(ui_menu_press(&m, 120, 70) == 0); /* a closed menu takes nothing */

    /* keys: Down skips separators and disabled items and wraps */
    build(&m);
    ui_menu_open(&m, 100, 50, AREA);
    KeyEvent k = key(KEY_DOWN);
    CHECK(ui_menu_key(&m, &k, &id) == 1 && m.hover == 0);
    CHECK(ui_menu_key(&m, &k, &id) == 1 && m.hover == 3); /* over the separator and the disabled Cut */
    CHECK(ui_menu_key(&m, &k, &id) == 1 && m.hover == 5); /* Delete */
    CHECK(ui_menu_key(&m, &k, &id) == 1 && m.hover == 0); /* wraps */
    k = key(KEY_UP);
    CHECK(ui_menu_key(&m, &k, &id) == 1 && m.hover == 5);
    k = key(KEY_HOME);
    CHECK(ui_menu_key(&m, &k, &id) == 1 && m.hover == 0);
    k = key(KEY_END);
    CHECK(ui_menu_key(&m, &k, &id) == 1 && m.hover == 5);
    k = key(KEY_UP);
    CHECK(ui_menu_key(&m, &k, &id) == 1 && m.hover == 3);
    k = key(KEY_ENTER);
    CHECK(ui_menu_key(&m, &k, &id) == 2 && id == C && !m.open);

    /* Enter with nothing highlighted does nothing; Esc closes; releases and unknown keys do nothing */
    ui_menu_open(&m, 100, 50, AREA);
    k = key(KEY_ENTER);
    CHECK(ui_menu_key(&m, &k, &id) == 0 && m.open);
    k = key('q');
    CHECK(ui_menu_key(&m, &k, &id) == 0 && m.open);
    k = key(KEY_DOWN);
    k.pressed = 0;
    CHECK(ui_menu_key(&m, &k, &id) == 0 && m.hover == -1);
    k = key(KEY_ESC);
    CHECK(ui_menu_key(&m, &k, &id) == 1 && !m.open);
    k = key(KEY_DOWN);
    CHECK(ui_menu_key(&m, &k, &id) == 0); /* closed */

    /* every item disabled: the keys find nothing */
    ui_menu_clear(&m);
    ui_menu_add(&m, A, "a", 0, UI_MENU_DISABLED);
    ui_menu_add(&m, B, "b", 0, UI_MENU_DISABLED);
    ui_menu_open(&m, 0, 0, AREA);
    k = key(KEY_DOWN);
    CHECK(ui_menu_key(&m, &k, &id) == 0 && m.hover == -1);

    /* the mouse and the keys share one highlight */
    build(&m);
    ui_menu_open(&m, 100, 50, AREA);
    ui_menu_hover(&m, 120, 140); /* Copy */
    k = key(KEY_DOWN);
    ui_menu_key(&m, &k, &id);
    CHECK(m.hover == 5);

    /* checks: labels are indented, and the check mark is drawn */
    ui_menu_clear(&m);
    ui_menu_add(&m, A, "Name", 0, UI_MENU_CHECKED);
    ui_menu_add(&m, B, "Size", 0, 0);
    CHECK(m.checks);
    ui_menu_open(&m, 0, 0, AREA);
    SHADOWS = ALPHAS = SEPS = ICONS = LABELS = DIM_LABELS = 0;
    ui_menu_draw(&m);
    CHECK(SHADOWS == 1 && ICONS == 1 && LABELS == 2 && ALPHAS == 0 && SEPS == 0);

    /* draw: highlight for the hovered item only, dim label for a disabled one, separators, nothing while closed */
    build(&m);
    ui_menu_open(&m, 100, 50, AREA);
    ui_menu_hover(&m, 120, 70);
    SHADOWS = ALPHAS = SEPS = ICONS = LABELS = DIM_LABELS = 0;
    ui_menu_draw(&m);
    CHECK(ALPHAS == 1 && SEPS == 2 && LABELS == 4 + 4); /* 4 labels and 4 shortcut hints */
    CHECK(DIM_LABELS == 1 + 4);                          /* Cut (disabled) and the four hints */
    ui_menu_close(&m);
    SHADOWS = 0;
    ui_menu_draw(&m);
    CHECK(SHADOWS == 0);

    /* the redraw area covers the panel and its shadow */
    ui_menu_open(&m, 100, 50, AREA);
    UiRect b = ui_menu_bounds(&m), p = ui_menu_rect(&m);
    CHECK(b.x < p.x && b.y < p.y && b.x + b.w > p.x + p.w + 14 && b.y + b.h > p.y + p.h + 19);

    printf("menu tests passed\n");
    return 0;
}
