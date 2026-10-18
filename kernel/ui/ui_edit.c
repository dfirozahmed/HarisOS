/* HarisUI: Single-line text editor and blinking caret. */
#include "ui_internal.h"
#include "timer.h"

/* ---- single-line text input ---------------------------------- */
void ui_edit_clear(UiEdit *e)
{
    e->text[0] = 0;
    e->len = 0;
    e->cursor = 0;
}

int ui_edit_key(UiEdit *e, const KeyEvent *k)
{
    if (!k->pressed)
        return 0;
    if (e->cursor > e->len)
        e->cursor = e->len;
    switch (k->key)
    {
    case KEY_LEFT:
        if (e->cursor == 0)
            return 0;
        e->cursor--;
        return 1;
    case KEY_RIGHT:
        if (e->cursor == e->len)
            return 0;
        e->cursor++;
        return 1;
    case KEY_HOME:
        if (e->cursor == 0)
            return 0;
        e->cursor = 0;
        return 1;
    case KEY_END:
        if (e->cursor == e->len)
            return 0;
        e->cursor = e->len;
        return 1;
    case KEY_BACKSPACE:
        if (e->cursor == 0)
            return 0;
        for (int i = e->cursor - 1; i < e->len; i++)
            e->text[i] = e->text[i + 1];
        e->cursor--;
        e->len--;
        return 1;
    case KEY_DELETE:
        if (e->cursor == e->len)
            return 0;
        for (int i = e->cursor; i < e->len; i++)
            e->text[i] = e->text[i + 1];
        e->len--;
        return 1;
    default:
        break;
    }
    if (k->ch >= 32 && k->ch < 127 && e->len < UI_EDIT_MAX)
    {
        for (int i = e->len; i >= e->cursor; i--) /* includes the 0 at the end */
            e->text[i + 1] = e->text[i];
        e->text[e->cursor++] = (char)k->ch;
        e->len++;
        return 1;
    }
    return 0;
}

/* ---- blinking caret ---- */
#define CARET_BLINK_MS 530
static int CARET_ON = 1;
static unsigned long long CARET_EPOCH;

void ui_caret_restart(void)
{
    CARET_ON = 1;
    CARET_EPOCH = timer_ms();
}

int ui_caret_visible(void) { return CARET_ON; }

int ui_caret_update(void)
{
    if (!timer_ready())
        return 0; /* no clock: the caret just stays visible */
    int on = ((timer_ms() - CARET_EPOCH) / CARET_BLINK_MS) % 2 == 0;
    if (on == CARET_ON)
        return 0;
    CARET_ON = on;
    return 1;
}

/* pixel width of text[from .. to) */
static int edit_width(const UiEdit *e, int from, int to)
{
    char tmp[UI_EDIT_MAX + 1];
    int n = 0;
    for (int i = from; i < to && i < e->len; i++)
        tmp[n++] = e->text[i];
    tmp[n] = 0;
    return gfx_text_w(THEME->font_body, tmp);
}

void ui_edit_draw(UiRect r, UiEdit *e, const char *placeholder, int focused)
{
    e->field = r;
    const UiTheme *t = THEME;
    ui_text_field(r, "", e->len ? 0 : placeholder, focused);
    UiRect in = ui_rect(r.x + 12, r.y, r.w - 24, r.h);
    if (in.w <= 4)
        return;
    int cur = e->cursor > e->len ? e->len : e->cursor;

    /* scroll: drop characters from the left until the cursor fits */
    int start = 0;
    while (start < cur && edit_width(e, start, cur) > in.w - 3)
        start++;

    int x = in.x;
    if (e->len)
    {
        GfxClip saved = gfx_get_clip();
        gfx_clip_intersect(in.x, in.y, in.w, in.h);
        ui_text_color(x, r.y + (r.h - ui_text_height(UI_TEXT_BODY)) / 2, e->text + start, UI_TEXT_BODY, t->text);
        gfx_restore_clip(saved);
    }
    if (focused && CARET_ON)
    {
        int cx = x + edit_width(e, start, cur);
        if (cx > in.x + in.w - 2)
            cx = in.x + in.w - 2;
        gfx_fill_rect(cx, r.y + 7, 2, r.h - 14, ui_color(t->accent_hi));
    }
}
