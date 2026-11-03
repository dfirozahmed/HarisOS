/* Host-side tests for the HarisUI scrollbar (kernel/ui/ui_scroll.c): thumb maths, drag, page click, hover. Drawing is recorded. */
#include "ui_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- stubs ---- */
static UiTheme THEME_STORE;
const UiTheme *THEME = &THEME_STORE;
UiRect ui_rect(int x, int y, int w, int h) { UiRect r = {x, y, w, h}; return r; }
int ui_contains(UiRect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
GfxColor ui_color(UiRgb c) { (void)c; return 0; }
static int DRAWS, LAST_BAR_W, LAST_THUMB_Y, LAST_THUMB_H;
void gfx_fill_round_rect_alpha(int x, int y, int w, int h, int r, GfxColor c, unsigned char a)
{
    (void)x; (void)y; (void)h; (void)r; (void)c; (void)a;
    LAST_BAR_W = w;
    DRAWS++;
}
void gfx_fill_round_rect(int x, int y, int w, int h, int r, GfxColor c)
{
    (void)x; (void)w; (void)r; (void)c;
    LAST_THUMB_Y = y;
    LAST_THUMB_H = h;
    DRAWS++;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static UiScroll mk(int total, int page, int pos)
{
    UiScroll s;
    memset(&s, 0, sizeof s);
    s.total = total;
    s.page = page;
    s.pos = pos;
    s.track = ui_rect(100, 50, UI_SCROLL_W, 208); /* 208 - 8 = 200 px of travel space */
    return s;
}

int main(void)
{
    /* needed / max / clamp */
    UiScroll s = mk(10, 10, 0);
    CHECK(!ui_scroll_needed(&s));
    CHECK(ui_scroll_max(&s) == 0);
    s = mk(5, 10, 0);
    CHECK(!ui_scroll_needed(&s) && ui_scroll_max(&s) == 0);
    s = mk(100, 0, 0);
    CHECK(!ui_scroll_needed(&s)); /* no page yet: nothing to scroll */
    s = mk(100, 10, 0);
    CHECK(ui_scroll_needed(&s) && ui_scroll_max(&s) == 90);
    CHECK(ui_scroll_set(&s, 500) && s.pos == 90);
    CHECK(!ui_scroll_set(&s, 90));
    CHECK(ui_scroll_set(&s, -5) && s.pos == 0);
    CHECK(ui_scroll_by(&s, 3) && s.pos == 3);
    CHECK(ui_scroll_by(&s, -99) && s.pos == 0);
    CHECK(!ui_scroll_by(&s, -1));
    CHECK(ui_scroll_by(&s, 0x7fffffff) && s.pos == 90); /* no wrap-around */
    CHECK(ui_scroll_by(&s, -0x7fffffff - 1 + 1) && s.pos == 0);

    /* thumb: height proportional, never below the minimum, ends at the ends */
    s = mk(100, 10, 0);
    UiRect t = ui_scroll_thumb(&s, s.track);
    CHECK(t.h == 24 && t.y == 54); /* 200 * 10 / 100 = 20 -> raised to 24 */
    s.pos = 90;
    t = ui_scroll_thumb(&s, s.track);
    CHECK(t.y + t.h == 50 + 208 - 4); /* bottom of the travel */
    s = mk(100, 10, 99); /* pos past total - page (a list that may scroll to its last row): the thumb stays in the track */
    t = ui_scroll_thumb(&s, s.track);
    CHECK(t.y + t.h == 50 + 208 - 4);
    s.pos = -7;
    t = ui_scroll_thumb(&s, s.track);
    CHECK(t.y == 54);
    s = mk(20, 10, 0);
    t = ui_scroll_thumb(&s, s.track);
    CHECK(t.h == 100); /* half the content in view = half the track */
    s = mk(5, 10, 0);
    t = ui_scroll_thumb(&s, s.track);
    CHECK(t.w == 0 && t.h == 0); /* nothing to scroll: no thumb */
    UiScroll tiny = mk(1000, 10, 500);
    tiny.track = ui_rect(0, 0, UI_SCROLL_W, 20); /* travel space 12 < min thumb */
    t = ui_scroll_thumb(&tiny, tiny.track);
    CHECK(t.h == 12 && t.y >= 4 && t.y + t.h <= 16);
    tiny.track = ui_rect(0, 0, UI_SCROLL_W, 3); /* absurdly small */
    t = ui_scroll_thumb(&tiny, tiny.track);
    CHECK(t.h == 1);

    /* press outside / not needed */
    s = mk(100, 10, 0);
    CHECK(!ui_scroll_press(&s, 99, 100));
    CHECK(!ui_scroll_press(&s, 100 + UI_SCROLL_W, 100));
    UiScroll fit = mk(5, 10, 0);
    CHECK(!ui_scroll_press(&fit, 105, 100));

    /* click in the track: a page at a time, below the thumb = down, above = up */
    s = mk(100, 10, 0);
    CHECK(ui_scroll_press(&s, 105, 200) && s.pos == 9 && !s.dragging);
    CHECK(ui_scroll_press(&s, 105, 200) && s.pos == 18);
    s.pos = 50;
    t = ui_scroll_thumb(&s, s.track);
    CHECK(ui_scroll_press(&s, 105, 52) && s.pos == 41); /* 52 is above the thumb at pos 50 */
    s = mk(100, 1, 0);
    CHECK(ui_scroll_press(&s, 105, 250) && s.pos == 1); /* page 1: still moves */

    /* drag: grab the thumb in the middle, move, release */
    s = mk(100, 10, 0);
    t = ui_scroll_thumb(&s, s.track);
    int gy = t.y + 10;
    CHECK(ui_scroll_press(&s, 105, gy) && s.dragging && s.grab == 10 && s.pos == 0);
    CHECK(!ui_scroll_drag(&s, 105, gy, 1)); /* not moved */
    int travel = 200 - 24;
    CHECK(ui_scroll_drag(&s, 105, gy + travel / 2, 1));
    CHECK(s.pos == 45); /* half way: 90 / 2 */
    CHECK(ui_scroll_drag(&s, 0, gy + 5000, 1) && s.pos == 90); /* far below: clamped, x does not matter */
    CHECK(ui_scroll_drag(&s, 105, gy - 5000, 1) && s.pos == 0);
    CHECK(ui_scroll_drag(&s, 105, gy + travel / 2, 0)); /* release: reports a redraw */
    CHECK(!s.dragging && s.hover); /* still over the bar */
    CHECK(!ui_scroll_drag(&s, 105, gy, 1)); /* no drag going on */
    /* releasing away from the bar clears hover */
    CHECK(ui_scroll_press(&s, 105, ui_scroll_thumb(&s, s.track).y + 3));
    CHECK(ui_scroll_drag(&s, 400, 10, 0) && !s.hover);

    /* a drag must follow the content when it changes length under the pointer */
    s = mk(100, 10, 0);
    t = ui_scroll_thumb(&s, s.track);
    ui_scroll_press(&s, 105, t.y + 2);
    s.total = 10; /* everything fits now */
    CHECK(!ui_scroll_drag(&s, 105, 300, 1));
    CHECK(ui_scroll_drag(&s, 105, 300, 0) && !s.dragging);

    /* hover */
    s = mk(100, 10, 0);
    CHECK(ui_scroll_hover(&s, 105, 100) && s.hover);
    CHECK(!ui_scroll_hover(&s, 106, 120)); /* still over: no change */
    CHECK(ui_scroll_hover(&s, -1, -1) && !s.hover);
    fit = mk(5, 10, 0);
    CHECK(!ui_scroll_hover(&fit, 105, 100)); /* no bar: never hovered */

    /* draw: remembers the track, widens while hovered, draws nothing when not needed */
    s = mk(100, 10, 20);
    s.track = ui_rect(0, 0, 0, 0);
    DRAWS = 0;
    ui_scroll_draw(ui_rect(10, 20, UI_SCROLL_W, 208), &s);
    CHECK(s.track.x == 10 && s.track.y == 20 && s.track.h == 208 && DRAWS == 2 && LAST_BAR_W == 4);
    CHECK(LAST_THUMB_H == 24 && LAST_THUMB_Y > 24);
    s.hover = 1;
    ui_scroll_draw(s.track, &s);
    CHECK(LAST_BAR_W == 8);
    fit = mk(5, 10, 0);
    DRAWS = 0;
    ui_scroll_draw(fit.track, &fit);
    CHECK(DRAWS == 0);

    /* round trip: pos -> thumb -> drag to the same spot gives the same pos */
    for (int total = 11; total < 3000; total += 97)
        for (int pos = 0; pos <= total - 10; pos += (total / 7) + 1)
        {
            UiScroll r = mk(total, 10, pos);
            UiRect th = ui_scroll_thumb(&r, r.track);
            ui_scroll_press(&r, 105, th.y + th.h / 2);
            if (!r.dragging)
                continue;
            ui_scroll_drag(&r, 105, th.y + th.h / 2, 1);
            int travel2 = 200 - th.h;
            int tol = travel2 > 0 ? (total - 10) / travel2 + 1 : total; /* one pixel of thumb is this many units */
            int diff = r.pos > pos ? r.pos - pos : pos - r.pos;
            CHECK(diff <= tol);
        }

    printf("scroll tests passed\n");
    return 0;
}
