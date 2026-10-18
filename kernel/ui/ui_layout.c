/* HarisUI: Rectangle layout and clipping helpers. */
#include "ui.h"

/* ---- layout -------------------------------------------------- */
UiRect ui_rect(int x, int y, int w, int h)
{
    UiRect r = {x, y, w, h};
    return r;
}

UiRect ui_inset(UiRect r, int a)
{
    return ui_rect(r.x + a, r.y + a, r.w - 2 * a > 0 ? r.w - 2 * a : 0, r.h - 2 * a > 0 ? r.h - 2 * a : 0);
}

UiRect ui_center(UiRect o, int w, int h)
{
    return ui_rect(o.x + (o.w - w) / 2, o.y + (o.h - h) / 2, w, h);
}

UiRect ui_cut_top(UiRect *r, int h)
{
    if (h > r->h)
        h = r->h;
    UiRect s = ui_rect(r->x, r->y, r->w, h);
    r->y += h;
    r->h -= h;
    return s;
}

UiRect ui_cut_bottom(UiRect *r, int h)
{
    if (h > r->h)
        h = r->h;
    UiRect s = ui_rect(r->x, r->y + r->h - h, r->w, h);
    r->h -= h;
    return s;
}

UiRect ui_cut_left(UiRect *r, int w)
{
    if (w > r->w)
        w = r->w;
    UiRect s = ui_rect(r->x, r->y, w, r->h);
    r->x += w;
    r->w -= w;
    return s;
}

UiRect ui_cut_right(UiRect *r, int w)
{
    if (w > r->w)
        w = r->w;
    UiRect s = ui_rect(r->x + r->w - w, r->y, w, r->h);
    r->w -= w;
    return s;
}

int ui_contains(UiRect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void ui_clip(UiRect r) { gfx_set_clip(r.x, r.y, r.w, r.h); }

