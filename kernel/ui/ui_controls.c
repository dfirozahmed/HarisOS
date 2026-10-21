/* HarisUI: Buttons and common UI controls. */
#include "ui_internal.h"

/* ---- controls ------------------------------------------------ */
static UiRgb lighten(UiRgb c, int amount) { return ui_rgb_mix(c, ui_rgb(255, 255, 255), (unsigned char)amount); }
static UiRgb darken(UiRgb c, int amount) { return ui_rgb_mix(c, ui_rgb(0, 0, 0), (unsigned char)amount); }

void ui_button(UiRect r, const char *label, UiButtonKind kind, UiState state)
{
    ui_button_icon(r, label, -1, kind, state);
}

void ui_button_icon(UiRect r, const char *label, int icon, UiButtonKind kind, UiState state)
{
    const UiTheme *t = THEME;
    int rad = t->radius_m;
    if (rad > r.h / 2)
        rad = r.h / 2;
    int disabled = state == UI_STATE_DISABLED;
    UiRgb top, bottom, text;

    switch (kind)
    {
    case UI_BTN_PRIMARY:
        top = t->accent_hi;
        bottom = t->accent;
        text = t->text_on_accent;
        break;
    case UI_BTN_DANGER:
        top = lighten(t->danger, 40);
        bottom = t->danger;
        text = t->text_on_accent;
        break;
    case UI_BTN_SECONDARY:
        top = lighten(t->surface_alt, 14);
        bottom = t->surface_alt;
        text = t->text;
        break;
    default: /* ghost: only visible when hovered or pressed */
        top = bottom = t->surface_alt;
        text = t->text;
        break;
    }
    if (state == UI_STATE_HOVER)
    {
        top = lighten(top, 22);
        bottom = lighten(bottom, 22);
    }
    else if (state == UI_STATE_PRESSED)
    {
        top = darken(top, 22);
        bottom = darken(bottom, 22);
    }
    else if (disabled)
    {
        top = bottom = ui_rgb_mix(t->surface, t->surface_alt, 160);
        text = t->text_dim;
    }

    if ((kind == UI_BTN_PRIMARY || kind == UI_BTN_DANGER) && !disabled && state != UI_STATE_PRESSED)
        gfx_shadow(r.x, r.y, r.w, r.h, rad, 4, 0, 2, 30);
    if (kind == UI_BTN_GHOST && (state == UI_STATE_NORMAL || disabled))
    {
        /* nothing behind the label */
    }
    else
    {
        //for a solid color, use the line below:
        //gfx_fill_round_rect(r.x, r.y, r.w, r.h, rad, ui_color(bottom));
        //for a gradient, use the line below instead of the one above:
        gfx_fill_round_rect_gradient(r.x, r.y, r.w, r.h, rad, ui_color(top), ui_color(bottom));
    }
    if (kind == UI_BTN_SECONDARY && !disabled)
        gfx_round_rect(r.x, r.y, r.w, r.h, rad, 1, ui_color(t->border));
    if (icon < 0)
    {
        ui_text_in_color(ui_rect(r.x + 10, r.y, r.w - 20, r.h), label, UI_TEXT_LABEL, UI_ALIGN_CENTER, text);
        return;
    }
    /* icon and label side by side, centered together as one group */
    int isz = r.h >= 30 ? 18 : 14, gap = label[0] ? 7 : 0;
    int tw = label[0] ? ui_text_width(label, UI_TEXT_LABEL) : 0;
    int total = isz + gap + tw;
    if (total > r.w - 12) /* too narrow for both: the label is shortened by ui_text_in_color */
        tw = r.w - 12 - isz - gap, total = r.w - 12;
    int gx = r.x + (r.w - total) / 2;
    ui_icon((UiIcon)icon, gx + isz / 2, r.y + r.h / 2, isz, text);
    if (tw > 0)
        ui_text_in_color(ui_rect(gx + isz + gap, r.y, tw, r.h), label, UI_TEXT_LABEL, UI_ALIGN_LEFT, text);
}

/* ---- button widgets with events ------------------------------ */
void ui_buttons_init(UiButtonSet *s)
{
    s->count = 0;
    s->hover = -1;
    s->pressed = -1;
    s->last_buttons = 0;
}

void ui_buttons_clear(UiButtonSet *s) { ui_buttons_init(s); }

int ui_buttons_add(UiButtonSet *s, UiButton *b)
{
    if (!b || s->count >= UI_MAX_BUTTONS)
        return -1;
    s->items[s->count++] = b;
    return 0;
}

void ui_buttons_draw(const UiButtonSet *s)
{
    for (int i = 0; i < s->count; i++)
    {
        const UiButton *b = s->items[i];
        UiState st = UI_STATE_NORMAL;
        if (!b->enabled)
            st = UI_STATE_DISABLED;
        else if (i == s->pressed)
            st = UI_STATE_PRESSED;
        else if (i == s->hover && s->pressed < 0)
            st = UI_STATE_HOVER;
        ui_button_icon(b->r, b->label, b->icon - 1, b->kind, st);
    }
}

UiRect ui_buttons_bounds(const UiButtonSet *s)
{
    if (s->count == 0)
        return ui_rect(0, 0, 0, 0);
    int x0 = s->items[0]->r.x, y0 = s->items[0]->r.y;
    int x1 = x0 + s->items[0]->r.w, y1 = y0 + s->items[0]->r.h;
    for (int i = 1; i < s->count; i++)
    {
        UiRect r = s->items[i]->r;
        if (r.x < x0)
            x0 = r.x;
        if (r.y < y0)
            y0 = r.y;
        if (r.x + r.w > x1)
            x1 = r.x + r.w;
        if (r.y + r.h > y1)
            y1 = r.y + r.h;
    }
    return ui_rect(x0, y0, x1 - x0, y1 - y0);
}

static int buttons_hit(const UiButtonSet *s, int x, int y)
{
    for (int i = 0; i < s->count; i++)
        if (s->items[i]->enabled && s->items[i]->r.w && ui_contains(s->items[i]->r, x, y))
            return i;
    return -1;
}

static void buttons_fire(UiButton *b, UiButtonFn fn)
{
    if (fn)
        fn(b, b->user);
}

int ui_buttons_pointer(UiButtonSet *s, int x, int y, unsigned buttons)
{
    int changed = 0;
    int over = buttons_hit(s, x, y);
    int down = (buttons & 1) != 0, was_down = (s->last_buttons & 1) != 0;
    s->last_buttons = buttons;

    if (over != s->hover)
    {
        int old = s->hover;
        s->hover = over;
        changed = 1;
        if (old >= 0 && old < s->count)
            buttons_fire(s->items[old], s->items[old]->on_leave);
        if (over >= 0 && s->hover == over) /* a callback may have changed things */
            buttons_fire(s->items[over], s->items[over]->on_hover);
    }

    if (down && !was_down && over >= 0)
    {
        s->pressed = over;
        changed = 1;
        buttons_fire(s->items[over], s->items[over]->on_press);
    }
    else if (!down && was_down && s->pressed >= 0)
    {
        int p = s->pressed;
        s->pressed = -1;
        changed = 1;
        if (p < s->count && p == over) /* released on the same button = a click */
            buttons_fire(s->items[p], s->items[p]->on_click);
    }
    return changed;
}

void ui_progress(UiRect r, int percent)
{
    const UiTheme *t = THEME;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    int rad = r.h / 2;
    gfx_fill_round_rect(r.x, r.y, r.w, r.h, rad, ui_color(t->surface_alt));
    int fw = r.w * percent / 100;
    if (percent > 0 && fw < r.h)
        fw = r.h; /* keep the rounded end visible */
    if (percent > 0)
        gfx_fill_round_rect_gradient(r.x, r.y, fw, r.h, rad, ui_color(t->accent_hi), ui_color(t->accent));
}

/* The knob is a circle of diameter r.h; its center runs from r.x + r.h/2 to r.x + r.w - r.h/2. */
void ui_slider(UiRect r, int percent)
{
    const UiTheme *t = THEME;
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    int kr = r.h / 2;
    int th = 6; /* track thickness */
    int ty = r.y + (r.h - th) / 2;
    int cx = r.x + kr + (r.w - r.h) * percent / 100;
    gfx_fill_round_rect(r.x + kr, ty, r.w - r.h, th, th / 2, ui_color(t->surface_alt));
    if (percent > 0)
        gfx_fill_round_rect_gradient(r.x + kr, ty, cx - r.x - kr, th, th / 2, ui_color(t->accent_hi), ui_color(t->accent));
    gfx_fill_circle(cx, r.y + kr, kr, ui_color(t->accent));
    gfx_fill_circle(cx, r.y + kr, kr - 3, ui_color(t->surface));
}

void ui_toggle(UiRect r, int on)
{
    const UiTheme *t = THEME;
    int rad = r.h / 2;
    if (on)
        gfx_fill_round_rect_gradient(r.x, r.y, r.w, r.h, rad, ui_color(t->accent_hi), ui_color(t->accent));
    else
    {
        gfx_fill_round_rect(r.x, r.y, r.w, r.h, rad, ui_color(t->surface_alt));
        gfx_round_rect(r.x, r.y, r.w, r.h, rad, 1, ui_color(t->border));
    }
    int kr = rad - 3;
    int kx = on ? r.x + r.w - rad : r.x + rad;
    gfx_fill_circle_q(kx * 16, (r.y + rad) * 16, kr * 16, gfx_rgb(255, 255, 255), 255);
}

int ui_badge(int x, int y, const char *s, UiRgb color)
{
    const UiTheme *t = THEME;
    const Font *f = t->font_small;
    int w = gfx_text_w(f, s) + 20, h = font_line_height(f) + 6;
    gfx_fill_round_rect_alpha(x, y, w, h, h / 2, ui_color(color), 46);
    gfx_round_rect_alpha(x, y, w, h, h / 2, 1, ui_color(color), 110);
    gfx_fill_circle_q((x + 9) * 16, (y + h / 2) * 16, 3 * 16, ui_color(color), 255);
    gfx_text(f, x + 16, y + 3, s, ui_color(color));
    return w;
}

void ui_text_field(UiRect r, const char *text, const char *placeholder, int focused)
{
    const UiTheme *t = THEME;
    gfx_fill_round_rect(r.x, r.y, r.w, r.h, t->radius_m, ui_color(t->surface_alt));
    gfx_round_rect(r.x, r.y, r.w, r.h, t->radius_m, focused ? 2 : 1, ui_color(focused ? t->accent : t->border));
    UiRect in = ui_rect(r.x + 12, r.y, r.w - 24, r.h);
    if (text && *text)
        ui_text_in_color(in, text, UI_TEXT_BODY, UI_ALIGN_LEFT, t->text);
    else if (placeholder)
        ui_text_in_color(in, placeholder, UI_TEXT_BODY, UI_ALIGN_LEFT, t->text_dim);
}
