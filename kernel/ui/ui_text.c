/* HarisUI: Text layout, alignment and style helpers. */
#include "ui_internal.h"

/* ---- text ---------------------------------------------------- */
const Font *ui_font(UiTextStyle s)
{
    switch (s)
    {
    case UI_TEXT_SMALL:
        return THEME->font_small;
    case UI_TEXT_LABEL:
        return THEME->font_label;
    case UI_TEXT_TITLE:
        return THEME->font_title;
    case UI_TEXT_HEADING:
        return THEME->font_heading;
    default:
        return THEME->font_body;
    }
}

static UiRgb style_color(UiTextStyle s)
{
    return (s == UI_TEXT_DIM || s == UI_TEXT_SMALL) ? THEME->text_dim : THEME->text;
}

int ui_text_width(const char *s, UiTextStyle st) { return gfx_text_w(ui_font(st), s); }
int ui_text_height(UiTextStyle st) { return font_line_height(ui_font(st)); }

void ui_text_color(int x, int y, const char *s, UiTextStyle st, UiRgb c)
{
    gfx_text(ui_font(st), x, y, s, ui_color(c));
}

void ui_text(int x, int y, const char *s, UiTextStyle st)
{
    ui_text_color(x, y, s, st, style_color(st));
}

void ui_text_in_color(UiRect r, const char *s, UiTextStyle st, UiAlign align, UiRgb color)
{
    const Font *f = ui_font(st);
    char buf[96];
    int w = gfx_text_w(f, s);
    if (w > r.w)
    {
        /* too long: cut it and add "..." */
        int dots = gfx_text_w(f, "...");
        int n = gfx_text_fit(f, s, r.w - dots);
        if (n > (int)sizeof(buf) - 4)
            n = (int)sizeof(buf) - 4;
        for (int i = 0; i < n; i++)
            buf[i] = s[i];
        buf[n] = '.';
        buf[n + 1] = '.';
        buf[n + 2] = '.';
        buf[n + 3] = 0;
        s = buf;
        w = gfx_text_w(f, s);
    }
    int x = r.x;
    if (align == UI_ALIGN_CENTER)
        x = r.x + (r.w - w) / 2;
    else if (align == UI_ALIGN_RIGHT)
        x = r.x + r.w - w;
    int y = r.y + (r.h - font_line_height(f)) / 2;
    gfx_text(f, x, y, s, ui_color(color));
}

void ui_text_in(UiRect r, const char *s, UiTextStyle st, UiAlign align)
{
    ui_text_in_color(r, s, st, align, style_color(st));
}

