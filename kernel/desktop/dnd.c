/* HarisOS drag and drop of files from a window to the desktop: see include/dnd.h. */
#include "dnd.h"
#include "deskfiles.h"
#include "taskbar.h"
#include "vfs.h"
#include "wm.h"

#define CARD_H 46
#define CARD_PAD 10
#define CARD_ICON 24
#define CARD_MIN_W 120
#define CARD_MAX_W 300
#define OFFSET_X 16 /* the card sits below and to the right of the pointer */
#define OFFSET_Y 14

static int ACTIVE;
static int NODES[DND_MAX];
static int N;
static char LABEL[VFS_NAME_MAX + 16];
static int X, Y;    /* the pointer */
static int OVER;    /* the pointer is where a drop does something: the bare desktop */

static char *put(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;
    return o;
}

static int on_bare_desktop(int x, int y)
{
    if (x < 0 || y < 0 || x >= (int)gfx_width() || y >= (int)gfx_height() - TASKBAR_HEIGHT)
        return 0;
    return wm_window_at(x, y) == 0;
}

static const char *hint(void)
{
    return OVER ? "Send to desktop" : "";
}

/* where the card is: below and to the right of the pointer, kept on the screen */
static UiRect card(void)
{
    int tw = ui_text_width(LABEL, UI_TEXT_LABEL);
    int hw = ui_text_width(hint(), UI_TEXT_SMALL);
    int w = CARD_PAD * 2 + CARD_ICON + 8 + (tw > hw ? tw : hw);
    if (w < CARD_MIN_W)
        w = CARD_MIN_W;
    if (w > CARD_MAX_W)
        w = CARD_MAX_W;
    int x = X + OFFSET_X, y = Y + OFFSET_Y;
    int W = (int)gfx_width(), H = (int)gfx_height();
    if (x + w > W - 4)
        x = X - OFFSET_X - w; /* near the right edge: to the left of the pointer */
    if (y + CARD_H > H - 4)
        y = Y - OFFSET_Y - CARD_H;
    return ui_rect(x, y, w, CARD_H);
}

UiRect dnd_bounds(void)
{
    UiRect c = card();
    return ui_rect(c.x - 16, c.y - 16, c.w + 32, c.h + 16 + 24);
}

static void damage(void)
{
    wm_add_screen_damage(dnd_bounds());
}

void dnd_begin(const int *nodes, int n, const char *label)
{
    if (ACTIVE)
        damage();
    if (!nodes || n <= 0)
    {
        ACTIVE = 0;
        return;
    }
    N = n < DND_MAX ? n : DND_MAX;
    for (int i = 0; i < N; i++)
        NODES[i] = nodes[i];
    char *o = LABEL;
    if (label && label[0])
        o = put(o, label);
    else if (N == 1 && vfs_node(NODES[0]))
        o = put(o, vfs_node(NODES[0])->name);
    else
    {
        char t[12];
        int k = 0, v = N;
        do
            t[k++] = (char)('0' + v % 10);
        while ((v /= 10) > 0);
        while (k)
            *o++ = t[--k];
        o = put(o, " items");
    }
    *o = 0;
    ACTIVE = 1;
    OVER = 0;
}

int dnd_active(void) { return ACTIVE; }

void dnd_move(int x, int y)
{
    if (!ACTIVE)
        return;
    damage(); /* where the card was */
    X = x;
    Y = y;
    OVER = on_bare_desktop(x, y);
    damage();
}

void dnd_cancel(void)
{
    if (!ACTIVE)
        return;
    damage();
    ACTIVE = 0;
    OVER = 0;
}

int dnd_drop(int x, int y)
{
    if (!ACTIVE)
        return 0;
    int drop = on_bare_desktop(x, y);
    int nodes[DND_MAX], n = N;
    for (int i = 0; i < n; i++)
        nodes[i] = NODES[i];
    dnd_cancel(); /* the card goes first: what the drop does draws over the place it was */
    if (!drop)
        return 0;
    return deskfiles_send_many(nodes, n) > 0 ? 1 : 0;
}

void dnd_draw(void)
{
    if (!ACTIVE || !gfx_ready())
        return;
    const UiTheme *t = ui_theme();
    UiRect c = card();
    gfx_shadow(c.x, c.y, c.w, c.h, t->radius_m, 14, 0, 5, 120);
    gfx_fill_round_rect_alpha(c.x, c.y, c.w, c.h, t->radius_m, ui_color(t->surface), 238);
    gfx_round_rect_alpha(c.x, c.y, c.w, c.h, t->radius_m, OVER ? 2 : 1, ui_color(OVER ? t->accent_hi : t->border), OVER ? 230 : 170);
    const VfsNode *first = vfs_node(NODES[0]);
    int cx = c.x + CARD_PAD + CARD_ICON / 2, cy = c.y + c.h / 2;
    ui_icon(first && first->is_dir ? UI_ICON_FILES : UI_ICON_NOTEPAD, cx, cy, CARD_ICON, t->text);
    if (N > 1) /* how many, on the icon's corner */
    {
        gfx_fill_circle(cx + 10, cy - 10, 8, ui_color(t->accent));
        char num[8];
        int k = 0, v = N > 99 ? 99 : N;
        do
            num[k++] = (char)('0' + v % 10);
        while ((v /= 10) > 0);
        char s[8];
        for (int i = 0; i < k; i++)
            s[i] = num[k - 1 - i];
        s[k] = 0;
        ui_text_in_color(ui_rect(cx + 2, cy - 18, 16, 16), s, UI_TEXT_SMALL, UI_ALIGN_CENTER, t->text_on_accent);
    }
    int tx = c.x + CARD_PAD + CARD_ICON + 8;
    int tw = c.x + c.w - CARD_PAD - tx;
    if (hint()[0])
    {
        ui_text_in(ui_rect(tx, c.y + 5, tw, 20), LABEL, UI_TEXT_LABEL, UI_ALIGN_LEFT);
        ui_text_in_color(ui_rect(tx, c.y + 24, tw, 16), hint(), UI_TEXT_SMALL, UI_ALIGN_LEFT, t->accent_hi);
    }
    else
        ui_text_in(ui_rect(tx, c.y, tw, c.h), LABEL, UI_TEXT_LABEL, UI_ALIGN_LEFT);
}
