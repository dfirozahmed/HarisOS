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
    ui_button_glow(r, label, icon, kind, state, state == UI_STATE_HOVER ? 255 : 0);
}

/* The hover highlight is a lightening of the button's colours by `glow`/255 of the full amount, so it can fade;
 * a ghost button has no fill at rest, so its highlight fades by alpha instead. */
void ui_button_glow(UiRect r, const char *label, int icon, UiButtonKind kind, UiState state, int glow)
{
    const UiTheme *t = THEME;
    if (state == UI_STATE_HOVER)
        state = UI_STATE_NORMAL; /* the glow carries the hover look from here on */
    if (glow < 0)
        glow = 0;
    if (glow > 255)
        glow = 255;
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
    int part = 0; /* a ghost button part-way to its hover fill: drawn with alpha, it has no fill to lighten */
    if (state == UI_STATE_PRESSED)
    {
        top = darken(top, 22);
        bottom = darken(bottom, 22);
    }
    else if (state == UI_STATE_NORMAL && glow > 0)
    {
        top = lighten(top, 22 * glow / 255);
        bottom = lighten(bottom, 22 * glow / 255);
        part = kind == UI_BTN_GHOST && glow < 255;
    }
    else if (disabled)
    {
        top = bottom = ui_rgb_mix(t->surface, t->surface_alt, 160);
        text = t->text_dim;
    }

    if ((kind == UI_BTN_PRIMARY || kind == UI_BTN_DANGER) && !disabled && state != UI_STATE_PRESSED)
        gfx_shadow(r.x, r.y, r.w, r.h, rad, 4, 0, 2, 30);
    if (kind == UI_BTN_GHOST && ((state == UI_STATE_NORMAL && glow == 0) || disabled))
    {
        /* nothing behind the label */
    }
    else if (part)
        gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, rad, ui_color(bottom), (unsigned char)glow);
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
    s->animated = 0;
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
        int glow = 0;
        if (!b->enabled)
            st = UI_STATE_DISABLED;
        else if (i == s->pressed)
            st = UI_STATE_PRESSED;
        else if (s->animated)
            glow = b->glow_shown; /* fading in or out (ui_buttons_tick) */
        else if (i == s->hover && s->pressed < 0)
            glow = 255;
        ui_button_glow(b->r, b->label, b->icon - 1, b->kind, st, glow);
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

/* Grow the dirty area by a button, with room for its shadow (blur 4, offset 2) and outline. */
static void dirty_add(UiRect *d, int *have, const UiButton *b)
{
    int x0 = b->r.x - 8, y0 = b->r.y - 8, x1 = b->r.x + b->r.w + 8, y1 = b->r.y + b->r.h + 8;
    if (*have)
    {
        if (d->x < x0) x0 = d->x;
        if (d->y < y0) y0 = d->y;
        if (d->x + d->w > x1) x1 = d->x + d->w;
        if (d->y + d->h > y1) y1 = d->y + d->h;
    }
    *d = ui_rect(x0, y0, x1 - x0, y1 - y0);
    *have = 1;
}

int ui_buttons_pointer_dirty(UiButtonSet *s, int x, int y, unsigned buttons, UiRect *dirty)
{
    int changed = 0, precise = 1, have = 0;
    UiRect d = ui_rect(0, 0, 0, 0);
    int over = buttons_hit(s, x, y);
    int down = (buttons & 1) != 0, was_down = (s->last_buttons & 1) != 0;
    s->last_buttons = buttons;

    if (over != s->hover)
    {
        int old = s->hover;
        s->hover = over;
        changed = 1;
        if (old >= 0 && old < s->count)
        {
            dirty_add(&d, &have, s->items[old]);
            if (s->items[old]->on_leave)
                precise = 0; /* a callback may change anything: redraw it all */
            buttons_fire(s->items[old], s->items[old]->on_leave);
        }
        if (over >= 0 && s->hover == over) /* a callback may have changed things */
        {
            dirty_add(&d, &have, s->items[over]);
            if (s->items[over]->on_hover)
                precise = 0;
            buttons_fire(s->items[over], s->items[over]->on_hover);
        }
    }

    if (down && !was_down && over >= 0)
    {
        s->pressed = over;
        changed = 1;
        dirty_add(&d, &have, s->items[over]);
        if (s->items[over]->on_press)
            precise = 0;
        buttons_fire(s->items[over], s->items[over]->on_press);
    }
    else if (!down && was_down && s->pressed >= 0)
    {
        int p = s->pressed;
        s->pressed = -1;
        changed = 1;
        if (p < s->count)
        {
            dirty_add(&d, &have, s->items[p]);
            if (p == over) /* released on the same button = a click: the app reacts, redraw it all */
                precise = 0;
        }
        else
            precise = 0;
        if (p < s->count && p == over)
            buttons_fire(s->items[p], s->items[p]->on_click);
    }
    if (!changed)
        return 0;
    if (!precise || !have || !dirty)
        return 1;
    *dirty = d;
    return 2;
}

int ui_buttons_tick(UiButtonSet *s, unsigned long long now_ms, UiRect *dirty)
{
    int have = 0;
    UiRect d = ui_rect(0, 0, 0, 0);
    s->animated = 1;
    for (int i = 0; i < s->count; i++)
    {
        UiButton *b = s->items[i];
        int want = b->enabled && i == s->hover && s->pressed < 0 ? 255 : 0;
        if (s->pressed >= 0 && i == s->pressed)
            want = 255; /* held: the pressed look has taken over; keep the glow so a release on the button does not dip */
        ui_fade_to(&b->glow, want, UI_FADE_MS, now_ms);
        int level = ui_fade_level(&b->glow, now_ms);
        if (level != b->glow_shown)
        {
            b->glow_shown = level;
            dirty_add(&d, &have, b);
        }
    }
    if (have && dirty)
        *dirty = d;
    return have;
}

int ui_buttons_pointer(UiButtonSet *s, int x, int y, unsigned buttons)
{
    return ui_buttons_pointer_dirty(s, x, y, buttons, 0) != 0;
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
