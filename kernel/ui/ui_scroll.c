/* HarisUI vertical scrollbar: the thumb maths, mouse handling (drag, page click, hover) and drawing.
 * The state is a UiScroll the app owns (see ui.h); no allocation, nothing global. */
#include "ui_internal.h"

#define SB_MARGIN 4    /* gap above and below the thumb's travel */
#define SB_MIN_THUMB 24
#define SB_THIN 4      /* drawn width at rest */
#define SB_WIDE 8      /* drawn width while hovered or dragged */

int ui_scroll_needed(const UiScroll *s)
{
    return s->page > 0 && s->total > s->page;
}

int ui_scroll_max(const UiScroll *s)
{
    int m = s->total - s->page;
    return m > 0 ? m : 0;
}

int ui_scroll_set(UiScroll *s, int pos)
{
    int max = ui_scroll_max(s);
    if (pos > max)
        pos = max;
    if (pos < 0)
        pos = 0;
    int changed = pos != s->pos;
    s->pos = pos;
    return changed;
}

int ui_scroll_by(UiScroll *s, int delta)
{
    long long p = (long long)s->pos + delta; /* a huge delta must not wrap */
    if (p > 0x7fffffffLL)
        p = 0x7fffffffLL;
    if (p < -0x7fffffffLL)
        p = -0x7fffffffLL;
    return ui_scroll_set(s, (int)p);
}

/* The thumb's height and how far it can travel inside a track of height th. */
static void sb_geometry(const UiScroll *s, UiRect tr, int *thumb_h, int *travel)
{
    int range = tr.h - 2 * SB_MARGIN;
    if (range < 1)
        range = 1;
    long long h = (long long)range * s->page / s->total;
    int min = SB_MIN_THUMB < range ? SB_MIN_THUMB : range; /* a tiny track still gets a thumb that fits */
    if (h < min)
        h = min;
    if (h > range)
        h = range;
    *thumb_h = (int)h;
    *travel = range - (int)h;
}

UiRect ui_scroll_thumb(const UiScroll *s, UiRect tr)
{
    if (!ui_scroll_needed(s))
        return ui_rect(tr.x, tr.y, 0, 0);
    int th, travel;
    sb_geometry(s, tr, &th, &travel);
    int max = ui_scroll_max(s);
    int pos = s->pos < 0 ? 0 : (s->pos > max ? max : s->pos); /* an app may let pos run past the end (Notepad's wheel does) */
    int off = (int)((long long)travel * pos / max);
    return ui_rect(tr.x, tr.y + SB_MARGIN + off, tr.w, th);
}

void ui_scroll_draw(UiRect tr, UiScroll *s)
{
    s->track = tr;
    if (!ui_scroll_needed(s))
        return;
    const UiTheme *t = THEME;
    int active = s->hover || s->dragging;
    int bw = active ? SB_WIDE : SB_THIN;
    int bx = tr.x + (tr.w - bw) / 2;
    UiRect th = ui_scroll_thumb(s, tr);
    gfx_fill_round_rect_alpha(bx, tr.y + SB_MARGIN, bw, tr.h - 2 * SB_MARGIN, bw / 2, ui_color(t->surface_alt),
                              active ? 200 : 120);
    gfx_fill_round_rect(bx, th.y, bw, th.h, bw / 2, ui_color(active ? t->text : t->text_dim));
}

int ui_scroll_press(UiScroll *s, int x, int y)
{
    if (!ui_scroll_needed(s) || !ui_contains(s->track, x, y))
        return 0;
    UiRect th = ui_scroll_thumb(s, s->track);
    if (y >= th.y && y < th.y + th.h)
    {
        s->dragging = 1;
        s->hover = 1;
        s->grab = y - th.y;
    }
    else
    {
        int step = s->page > 1 ? s->page - 1 : 1; /* one page, keeping a row of context */
        ui_scroll_by(s, y < th.y ? -step : step);
    }
    return 1;
}

int ui_scroll_drag(UiScroll *s, int x, int y, unsigned buttons)
{
    if (!s->dragging)
        return 0;
    if (!(buttons & 1)) /* released: the bar goes thin again unless the pointer is still on it */
    {
        s->dragging = 0;
        s->hover = ui_contains(s->track, x, y);
        return 1;
    }
    if (!ui_scroll_needed(s))
        return 0;
    int th, travel;
    sb_geometry(s, s->track, &th, &travel);
    if (travel <= 0)
        return 0;
    long long off = (long long)y - s->grab - (s->track.y + SB_MARGIN);
    if (off < 0)
        off = 0;
    if (off > travel)
        off = travel;
    int max = ui_scroll_max(s);
    return ui_scroll_set(s, (int)((off * max + travel / 2) / travel));
}

int ui_scroll_hover(UiScroll *s, int x, int y)
{
    int h = s->dragging || (ui_scroll_needed(s) && x >= 0 && ui_contains(s->track, x, y));
    int changed = h != s->hover;
    s->hover = h;
    return changed;
}
