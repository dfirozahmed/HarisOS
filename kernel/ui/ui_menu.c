/* HarisUI pop-up menu: layout, mouse and keyboard handling, drawing. The state is a UiMenu the app owns (see ui.h);
 * no allocation, nothing global. The look is the File Explorer's right-click menu, which now uses this. */
#include "ui_internal.h"

#define MENU_ROW_H 30
#define MENU_SEP_H 9
#define MENU_PAD 6
#define MENU_MIN_W 180
#define MENU_EDGE 4 /* gap kept to the edge of the area it opens in */

static int is_sep(const UiMenuItem *it) { return it->label == 0; }
static int selectable(const UiMenuItem *it) { return !is_sep(it) && !(it->flags & UI_MENU_DISABLED); }
static int item_h(const UiMenuItem *it) { return is_sep(it) ? MENU_SEP_H : MENU_ROW_H; }

void ui_menu_clear(UiMenu *m)
{
    m->open = 0;
    m->count = 0;
    m->hover = m->press = -1;
    m->press_on = 0;
    m->checks = 0;
    m->animated = 0;
    m->x = m->y = m->w = m->h = 0;
    for (int i = 0; i < UI_MENU_MAX; i++)
    {
        ui_fade_reset(&m->fade[i]);
        m->shown[i] = 0;
    }
}

int ui_menu_add(UiMenu *m, int id, const char *label, const char *hint, int flags)
{
    if (m->count >= UI_MENU_MAX || !label)
        return -1;
    UiMenuItem *it = &m->items[m->count++];
    it->id = id;
    it->label = label;
    it->hint = hint;
    it->flags = flags;
    if (flags & UI_MENU_CHECKED)
        m->checks = 1;
    return 0;
}

int ui_menu_separator(UiMenu *m)
{
    if (m->count == 0 || is_sep(&m->items[m->count - 1])) /* nothing above it, or one already */
        return 0;
    if (m->count >= UI_MENU_MAX)
        return -1;
    UiMenuItem *it = &m->items[m->count++];
    it->id = -1;
    it->label = 0;
    it->hint = 0;
    it->flags = 0;
    return 0;
}

void ui_menu_close(UiMenu *m)
{
    m->open = 0;
    m->hover = m->press = -1;
    m->press_on = 0;
}

void ui_menu_open(UiMenu *m, int x, int y, UiRect bounds)
{
    while (m->count > 0 && is_sep(&m->items[m->count - 1])) /* a separator at the end divides nothing */
        m->count--;
    ui_menu_close(m);
    if (m->count == 0)
        return;
    for (int i = 0; i < UI_MENU_MAX; i++) /* opens with no highlight */
    {
        ui_fade_reset(&m->fade[i]);
        m->shown[i] = 0;
    }

    int indent = m->checks ? 34 : 12;
    int wl = 0, wh = 0, h = MENU_PAD * 2;
    for (int i = 0; i < m->count; i++)
    {
        const UiMenuItem *it = &m->items[i];
        h += item_h(it);
        if (is_sep(it))
            continue;
        int a = ui_text_width(it->label, UI_TEXT_BODY);
        int b = it->hint && it->hint[0] ? ui_text_width(it->hint, UI_TEXT_SMALL) : 0;
        if (a > wl)
            wl = a;
        if (b > wh)
            wh = b;
    }
    int w = MENU_PAD * 2 + indent + wl + (wh ? 24 + wh : 0) + 12;
    if (w < MENU_MIN_W)
        w = MENU_MIN_W;
    if (w > bounds.w - 2 * MENU_EDGE)
        w = bounds.w - 2 * MENU_EDGE;
    if (h > bounds.h - 2 * MENU_EDGE)
        h = bounds.h - 2 * MENU_EDGE;

    if (x + w > bounds.x + bounds.w - MENU_EDGE)
        x = bounds.x + bounds.w - MENU_EDGE - w;
    if (y + h > bounds.y + bounds.h - MENU_EDGE)
        y = bounds.y + bounds.h - MENU_EDGE - h;
    m->x = x < bounds.x + MENU_EDGE ? bounds.x + MENU_EDGE : x;
    m->y = y < bounds.y + MENU_EDGE ? bounds.y + MENU_EDGE : y;
    m->w = w;
    m->h = h;
    m->open = 1;
}

UiRect ui_menu_rect(const UiMenu *m)
{
    return ui_rect(m->x, m->y, m->w, m->h);
}

/* the shadow (blur 14, moved 5 down) reaches 14 px around and 19 px below */
UiRect ui_menu_bounds(const UiMenu *m)
{
    return ui_rect(m->x - 16, m->y - 16, m->w + 32, m->h + 16 + 24);
}

UiRect ui_menu_item_rect(const UiMenu *m, int index)
{
    int y = m->y + MENU_PAD;
    for (int i = 0; i < index && i < m->count; i++)
        y += item_h(&m->items[i]);
    int h = index >= 0 && index < m->count ? item_h(&m->items[index]) : 0;
    return ui_rect(m->x + MENU_PAD, y, m->w - MENU_PAD * 2, h);
}

int ui_menu_hit(const UiMenu *m, int x, int y)
{
    if (!m->open)
        return -1;
    for (int i = 0; i < m->count; i++)
        if (selectable(&m->items[i]) && ui_contains(ui_menu_item_rect(m, i), x, y))
            return i;
    return -1;
}

int ui_menu_press(UiMenu *m, int x, int y)
{
    if (!m->open)
        return 0;
    if (!ui_contains(ui_menu_rect(m), x, y)) /* a click away from the menu closes it */
    {
        ui_menu_close(m);
        return 0;
    }
    int hit = ui_menu_hit(m, x, y);
    if (hit >= 0)
    {
        m->press = hit;
        m->press_on = 1;
    }
    return 1;
}

int ui_menu_release(UiMenu *m, int x, int y, unsigned buttons, int *id)
{
    if (!m->open || m->press < 0)
        return 0;
    int on = ui_menu_hit(m, x, y) == m->press; /* still on the item that was pressed */
    if (buttons & 1) /* held: the pressed look follows the pointer on and off the item */
    {
        int changed = on != m->press_on;
        m->press_on = on;
        return changed;
    }
    int p = m->press;
    m->press = -1;
    m->press_on = 0;
    if (!on)
        return 1;
    if (id)
        *id = m->items[p].id;
    m->open = 0;
    m->hover = -1;
    return 2;
}

int ui_menu_hover(UiMenu *m, int x, int y)
{
    if (!m->open)
        return 0;
    int h = x < 0 ? -1 : ui_menu_hit(m, x, y);
    int changed = h != m->hover;
    m->hover = h;
    return changed;
}

/* move the highlight to the next / previous item that can be chosen (wraps); from == -1 starts at an end */
static int step_to(const UiMenu *m, int from, int dir)
{
    int i = from;
    for (int k = 0; k < m->count; k++)
    {
        i = i < 0 ? (dir > 0 ? 0 : m->count - 1) : (i + dir + m->count) % m->count;
        if (selectable(&m->items[i]))
            return i;
    }
    return from;
}

int ui_menu_key(UiMenu *m, const KeyEvent *k, int *id)
{
    if (!m->open || !k->pressed)
        return 0;
    int old = m->hover;
    switch (k->key)
    {
    case KEY_UP:
        m->hover = step_to(m, m->hover, -1);
        return m->hover != old;
    case KEY_DOWN:
        m->hover = step_to(m, m->hover, 1);
        return m->hover != old;
    case KEY_HOME:
        m->hover = step_to(m, -1, 1);
        return m->hover != old;
    case KEY_END:
        m->hover = step_to(m, -1, -1);
        return m->hover != old;
    case KEY_ENTER:
        if (m->hover < 0 || !selectable(&m->items[m->hover]))
            return 0;
        if (id)
            *id = m->items[m->hover].id;
        ui_menu_close(m);
        return 2;
    case KEY_ESC:
        ui_menu_close(m);
        return 1;
    default:
        return 0;
    }
}

int ui_menu_tick(UiMenu *m, unsigned long long now_ms, UiRect *dirty)
{
    m->animated = 1;
    if (!m->open)
        return 0;
    int have = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = 0; i < m->count; i++)
    {
        ui_fade_to(&m->fade[i], i == m->hover && selectable(&m->items[i]) ? 255 : 0, UI_FADE_MS, now_ms);
        int lv = ui_fade_level(&m->fade[i], now_ms);
        if (lv == m->shown[i])
            continue;
        m->shown[i] = lv;
        UiRect r = ui_menu_item_rect(m, i);
        if (!have)
        {
            x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
            have = 1;
        }
        else
        {
            if (r.x < x0) x0 = r.x;
            if (r.y < y0) y0 = r.y;
            if (r.x + r.w > x1) x1 = r.x + r.w;
            if (r.y + r.h > y1) y1 = r.y + r.h;
        }
    }
    if (have && dirty)
        *dirty = ui_rect(x0, y0, x1 - x0, y1 - y0);
    return have;
}

void ui_menu_draw(const UiMenu *m)
{
    if (!m->open)
        return;
    const UiTheme *t = THEME;
    gfx_shadow(m->x, m->y, m->w, m->h, t->radius_m, 14, 0, 5, 120);
    gfx_fill_round_rect(m->x, m->y, m->w, m->h, t->radius_m, ui_color(t->surface));
    gfx_round_rect(m->x, m->y, m->w, m->h, t->radius_m, 1, ui_color(t->border));
    int indent = m->checks ? 34 : 12;
    for (int i = 0; i < m->count; i++)
    {
        const UiMenuItem *it = &m->items[i];
        UiRect r = ui_menu_item_rect(m, i);
        if (is_sep(it))
        {
            ui_separator(r.x + 6, r.y + r.h / 2, r.w - 12);
            continue;
        }
        int off = it->flags & UI_MENU_DISABLED;
        if (!off && i == m->press && m->press_on) /* held down: darker than hover */
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h - 2, t->radius_s, ui_color(t->accent), 110);
        else if (!off && m->animated && m->shown[i] > 0) /* fading in or out (ui_menu_tick) */
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h - 2, t->radius_s, ui_color(t->accent), (unsigned char)(60 * m->shown[i] / 255));
        else if (!off && !m->animated && i == m->hover)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h - 2, t->radius_s, ui_color(t->accent), 60);
        UiRgb c = off ? t->text_dim : (it->flags & UI_MENU_DANGER) ? t->danger : t->text;
        if (it->flags & UI_MENU_CHECKED)
            ui_icon(UI_ICON_CHECK, r.x + 18, r.y + (r.h - 2) / 2, 14, t->accent_hi);
        ui_text_in_color(ui_rect(r.x + indent, r.y, r.w - indent - 12, r.h - 2), it->label, UI_TEXT_BODY, UI_ALIGN_LEFT, c);
        if (it->hint && it->hint[0])
            ui_text_in_color(ui_rect(r.x + 12, r.y, r.w - 24, r.h - 2), it->hint, UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
    }
}
