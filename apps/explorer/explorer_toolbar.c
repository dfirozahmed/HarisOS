/* File Explorer: toolbar buttons, tooltips and the path bar. */
#include "explorer.h"

static void on_back(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_go_back();
}

static void on_forward(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_go_forward();
}

static void on_up(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_go_up();
}

static void on_new_folder(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_new_item(1);
}

static void on_sort(UiButton *b, void *user)
{
    (void)user;
    ex_menu_open(b->r.x, b->r.y + b->r.h + 2, 2); /* drops down under the button */
}

static void on_new_file(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_new_item(0);
}

static void on_delete(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_ask_delete(EX_SEL);
}

static void on_refresh(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_refresh();
}

static void on_cut(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_clip_set(1);
}

static void on_copy(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_clip_set(0);
}

static void on_paste(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_paste();
}

static void on_rename(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ex_start_rename(EX_SEL);
}

/* ---- the path bar: "This PC > Documents > Projects", each name a button ---- */
#define CRUMB_MAX 24
#define CRUMB_PAD 12 /* room around a name inside its button */
#define CRUMB_SEP 16 /* the gap with the ">" between two names */
#define CRUMB_TEXT_X 38 /* names start this far from the bar's left edge (after the folder icon) */

typedef struct
{
    int id; /* folder it leads to */
    const char *name;
    int x, w;
} Crumb;

UiRect ex_path_bar(const Layout *l)
{
    int px = l->toolbar.x + PATH_X;
    return ui_rect(px, l->toolbar.y + 9, l->toolbar.w - (px - l->toolbar.x) - 10 - NAVBTN - 4, 34); /* room for Refresh on the right */
}

/* The names that fit in the bar, left to right. When the path is too long the front is replaced by one "..." that
 * leads to the folder just before the first name shown. Returns how many. */
static int crumbs_build(const Layout *l, Crumb *out)
{
    int chain[CRUMB_MAX], depth = 0; /* EX_CUR up to the root */
    for (int n = EX_CUR; depth < CRUMB_MAX && vfs_node(n); n = vfs_node(n)->parent)
    {
        chain[depth++] = n;
        if (n == VFS_ROOT)
            break;
    }
    if (depth == 0)
        chain[depth++] = VFS_ROOT;
    Crumb all[CRUMB_MAX];
    int total = 0;
    for (int i = depth - 1; i >= 0; i--, total++)
    {
        all[total].id = chain[i];
        all[total].name = chain[i] == VFS_ROOT ? "This PC" : vfs_node(chain[i])->name;
        all[total].w = ui_text_width(all[total].name, UI_TEXT_BODY) + CRUMB_PAD * 2;
    }
    int room = ex_path_bar(l).w - CRUMB_TEXT_X - 8;
    int dots_w = ui_text_width("...", UI_TEXT_BODY) + CRUMB_PAD * 2;
    int first = total - 1, used = all[total - 1].w; /* the current folder always shows */
    while (first > 0 && used + CRUMB_SEP + all[first - 1].w + (first - 1 > 0 ? CRUMB_SEP + dots_w : 0) <= room)
        used += CRUMB_SEP + all[--first].w;
    if (first == total - 1 && used > room) /* the last name alone is too wide: it gets all the room, cut short */
        all[first].w = room > 0 ? room : 0;

    int n = 0, x = ex_path_bar(l).x + CRUMB_TEXT_X;
    if (first > 0)
    {
        out[n].id = all[first - 1].id;
        out[n].name = "...";
        out[n].x = x;
        out[n].w = dots_w;
        x += dots_w + CRUMB_SEP;
        n++;
    }
    for (int i = first; i < total; i++, n++)
    {
        out[n] = all[i];
        out[n].x = x;
        x += all[i].w + CRUMB_SEP;
    }
    return n;
}

static UiRect crumb_rect(const UiRect *bar, const Crumb *c) { return ui_rect(c->x, bar->y + 4, c->w, bar->h - 8); }

int ex_crumb_at(const Layout *l, int x, int y)
{
    UiRect bar = ex_path_bar(l);
    Crumb c[CRUMB_MAX];
    int n = crumbs_build(l, c);
    for (int i = 0; i < n; i++)
        if (ui_contains(crumb_rect(&bar, &c[i]), x, y))
            return c[i].id;
    return -1;
}

/* what was typed -> an absolute VFS path: "\" counts as "/", a leading "This PC" is dropped, no trailing "/" */
static void norm_path(const char *s, char *out, int cap)
{
    while (*s == ' ')
        s++;
    const char *pc = "This PC";
    int k = 0;
    while (pc[k] && ex_lower((unsigned char)s[k]) == ex_lower((unsigned char)pc[k]))
        k++;
    if (!pc[k])
        s += k;
    int o = 0;
    out[o++] = '/';
    for (; *s && o < cap - 1; s++)
    {
        char ch = *s == '\\' ? '/' : *s;
        if (ch == '/' && out[o - 1] == '/')
            continue;
        out[o++] = ch;
    }
    while (o > 1 && (out[o - 1] == ' ' || out[o - 1] == '/'))
        o--;
    out[o] = 0;
}

void ex_start_addr_edit(void)
{
    if (!EX_WIN || EX_ADDR_EDIT)
        return;
    if (EX_RENAME_ID >= 0 && !ex_end_rename(1))
        return;
    EX_MENU.open = 0;
    UiEdit *e = &EX_WIN->edits[0];
    char path[UI_EDIT_MAX + 1];
    vfs_path(EX_CUR, path, sizeof path);
    ui_edit_clear(e);
    e->max = UI_EDIT_MAX;
    ui_edit_set_text(e, path);
    ui_edit_select_all(e);
    EX_ADDR_EDIT = 1;
    EX_HOVER_CRUMB = -1;
    EX_NOTE = 0;
    EX_WIN->edit_count = 1;
    EX_WIN->focus = 0;
    EX_WIN->caret = 1;
    ui_caret_restart();
}

int ex_end_addr(int commit)
{
    if (!EX_ADDR_EDIT || !EX_WIN)
        return 1;
    UiEdit *e = &EX_WIN->edits[0];
    int id = -1;
    if (commit)
    {
        char path[UI_EDIT_MAX + 2];
        norm_path(e->text, path, sizeof path);
        id = vfs_lookup(path);
        if (!vfs_node(id))
        {
            EX_NOTE = "That path does not exist."; /* the field stays open so it can be fixed */
            return 0;
        }
    }
    EX_ADDR_EDIT = 0;
    EX_WIN->edit_count = 0;
    EX_WIN->caret = 0;
    e->field.w = 0;
    if (!commit)
        return 1;
    EX_NOTE = 0;
    const VfsNode *n = vfs_node(id);
    int file = n->is_dir ? -1 : id; /* a file: open its folder and select it */
    ex_go_to(n->is_dir ? id : n->parent, 1);
    int row = ex_row_of(file);
    if (row >= 0)
        ex_select_row(wm_content(EX_WIN), row);
    return 1;
}

void ex_draw_toolbar(const Layout *l, const UiTheme *t)
{
    UiRect tb = l->toolbar;
    gfx_fill_rect_alpha(tb.x, tb.y, tb.w, tb.h, t->surface_alt.r, t->surface_alt.g, t->surface_alt.b, 70);
    ui_separator(tb.x, tb.y + tb.h - 1, tb.w);

    /* command row: a divider between New folder and the clipboard / rename buttons */
    gfx_fill_rect_alpha(tb.x + CMD_X0 - 9, tb.y + CMD_Y + 6, 1, 22, t->border.r, t->border.g, t->border.b, 255);

    gfx_fill_rect_alpha(tb.x + SORT_X - 8, tb.y + CMD_Y + 6, 1, 22, t->border.r, t->border.g, t->border.b, 255);

    UiRect bar = ex_path_bar(l);
    if (bar.w < 40)
        return;
    gfx_fill_round_rect(bar.x, bar.y, bar.w, bar.h, t->radius_m, ui_color(t->surface));
    gfx_round_rect(bar.x, bar.y, bar.w, bar.h, t->radius_m, 1, ui_color(EX_ADDR_EDIT ? t->accent : t->border));
    if (EX_CUR == VFS_ROOT)
        ex_draw_pc_icon(bar.x + 20, bar.y + bar.h / 2, 20);
    else
        ex_draw_folder_icon(bar.x + 20, bar.y + bar.h / 2, 18);
    if (EX_ADDR_EDIT && EX_WIN)
    {
        ui_edit_draw(ui_rect(bar.x + CRUMB_TEXT_X - 6, bar.y + 4, bar.w - CRUMB_TEXT_X - 2, bar.h - 8), &EX_WIN->edits[0],
                     "Type a path, e.g. /RAM Disk", 1);
        return;
    }
    Crumb c[CRUMB_MAX];
    int n = crumbs_build(l, c);
    GfxClip saved = gfx_get_clip(); /* a name that is too long is cut at the bar's edge */
    gfx_clip_intersect(bar.x, bar.y, bar.w - 4, bar.h);
    for (int i = 0; i < n; i++)
    {
        UiRect r = crumb_rect(&bar, &c[i]);
        int last = i == n - 1;
        if (c[i].id == EX_HOVER_CRUMB)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 30);
        ui_text_in_color(r, c[i].name, UI_TEXT_BODY, UI_ALIGN_CENTER, last ? t->text : t->text_dim);
        if (!last)
            ui_text_in_color(ui_rect(r.x + r.w, r.y, CRUMB_SEP, r.h), ">", UI_TEXT_SMALL, UI_ALIGN_CENTER, t->text_dim);
    }
    gfx_restore_clip(saved);
}

/* tooltip: a small label under the toolbar button the pointer rests on (above it near the bottom) */
void ex_tip_draw(const Window *w, UiRect body, const UiTheme *t)
{
    static const char *const TIPS[BTN_COUNT] = {"Back (Alt+Left)", "Forward (Alt+Right)", "Up one level (Alt+Up)",
                                                "Refresh (F5)", "New folder (Ctrl+Shift+N)", "New text file",
                                                "Cut (Ctrl+X)", "Copy (Ctrl+C)", "Paste (Ctrl+V)", "Rename (F2)",
                                                "Delete (Del)", "Sort by"};
    if (EX_TIP_BTN < 0 || EX_TIP_BTN >= BTN_COUNT || EX_TIP_BTN >= w->btn_count)
        return;
    const char *s = TIPS[EX_TIP_BTN];
    UiRect b = w->btn[EX_TIP_BTN].r;
    int tw = ui_text_width(s, UI_TEXT_SMALL) + 22, th = 26;
    int x = b.x + b.w / 2 - tw / 2, y = b.y + b.h + 6;
    if (x + tw > body.x + body.w - 4)
        x = body.x + body.w - 4 - tw;
    if (x < body.x + 4)
        x = body.x + 4;
    if (y + th > body.y + body.h - 4)
        y = b.y - th - 6;
    gfx_shadow(x, y, tw, th, t->radius_s, 8, 0, 3, 110);
    gfx_fill_round_rect(x, y, tw, th, t->radius_s, ui_color(t->titlebar));
    gfx_round_rect(x, y, tw, th, t->radius_s, 1, ui_color(t->border));
    ui_text_in_color(ui_rect(x, y, tw, th), s, UI_TEXT_SMALL, UI_ALIGN_CENTER, t->text);
}

/* grey out the toolbar buttons that cannot do anything right now */
void ex_sync_buttons(void)
{
    if (!EX_WIN)
        return;
    int idle = EX_RENAME_ID < 0 && !EX_ADDR_EDIT;
    int has = EX_SEL >= 0 && EX_SEL < EX_NITEMS && !ex_is_drive(vfs_node(EX_ITEMS[EX_SEL])); /* a drive cannot be cut, renamed ... */
    int inside = EX_CUR != VFS_ROOT;                                                       /* nothing is created in This PC */
    EX_WIN->btn[BTN_REFRESH].enabled = idle;
    EX_WIN->btn[BTN_NEW_FOLDER].enabled = idle && inside;
    EX_WIN->btn[BTN_NEW_FILE].enabled = idle && inside;
    EX_WIN->btn[BTN_CUT].enabled = idle && has;
    EX_WIN->btn[BTN_COPY].enabled = idle && has;
    EX_WIN->btn[BTN_PASTE].enabled = idle && inside && vfs_node(EX_CLIP_ID) != 0;
    EX_WIN->btn[BTN_RENAME].enabled = idle && has;
    EX_WIN->btn[BTN_DELETE].enabled = idle && has && !ex_delete_pending() && !ex_lock_dialog_pending();
    EX_WIN->btn[BTN_SORT].enabled = idle;
    ex_update_nav_buttons();
}

/* time passes: show the tooltip once the pointer has rested on a button long enough */
int ex_tick(Window *w)
{
    int drives = ex_sync_drives();
    int want = EX_TIP_BTN >= 0 && !EX_MENU.open && w == wm_top() && timer_ms() - EX_TIP_SINCE >= TIP_DELAY_MS;
    if (want == EX_TIP_SHOWN)
        return drives;
    EX_TIP_SHOWN = want;
    return 1;
}

/* Add the toolbar buttons to the new window, in the order of the BTN_* ids (ex_sync_buttons and
 * ex_update_nav_buttons rely on it). content_w = width of the window's content area.
 * Navigation row: Back, Forward, Up (left), Refresh (right end). Command row: New folder, New file,
 * then Cut, Copy, Paste, Rename, Delete after a divider, then Sort. All ghost style: only the icon
 * shows until the button is hovered. */
void ex_toolbar_create(Window *win, int cw)
{
    int step = NAVBTN + 2;
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(10, 8, NAVBTN, NAVBTN), 0, on_back, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(10 + step, 8, NAVBTN, NAVBTN), 0, on_forward, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(10 + 2 * step, 8, NAVBTN, NAVBTN), 0, on_up, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(cw - 10 - NAVBTN, 8, NAVBTN, NAVBTN), WM_ANCHOR_RIGHT, on_refresh, 0);
    wm_add_button_ex(win, "New folder", UI_BTN_GHOST, ui_rect(NEWBTN_X, CMD_Y, NEWBTN_W, 34), 0, on_new_folder, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(NEWBTN_X + NEWBTN_W + 2, CMD_Y, NAVBTN, 34), 0, on_new_file, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(CMD_X0, CMD_Y, NAVBTN, 34), 0, on_cut, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(CMD_X0 + step, CMD_Y, NAVBTN, 34), 0, on_copy, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(CMD_X0 + 2 * step, CMD_Y, NAVBTN, 34), 0, on_paste, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(CMD_X0 + 3 * step, CMD_Y, NAVBTN, 34), 0, on_rename, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(CMD_X0 + 4 * step, CMD_Y, NAVBTN, 34), 0, on_delete, 0);
    wm_add_button_ex(win, "Sort", UI_BTN_GHOST, ui_rect(SORT_X, CMD_Y, SORT_W, 34), 0, on_sort, 0);

    wm_set_button_icon(win, BTN_BACK, UI_ICON_ARROW_LEFT);
    wm_set_button_icon(win, BTN_FORWARD, UI_ICON_ARROW_RIGHT);
    wm_set_button_icon(win, BTN_UP, UI_ICON_ARROW_UP);
    wm_set_button_icon(win, BTN_REFRESH, UI_ICON_REFRESH);
    wm_set_button_icon(win, BTN_NEW_FOLDER, UI_ICON_FOLDER_PLUS);
    wm_set_button_icon(win, BTN_NEW_FILE, UI_ICON_FILE_PLUS);
    wm_set_button_icon(win, BTN_CUT, UI_ICON_CUT);
    wm_set_button_icon(win, BTN_COPY, UI_ICON_COPY);
    wm_set_button_icon(win, BTN_PASTE, UI_ICON_PASTE);
    wm_set_button_icon(win, BTN_RENAME, UI_ICON_RENAME);
    wm_set_button_icon(win, BTN_DELETE, UI_ICON_TRASH);
    wm_set_button_icon(win, BTN_SORT, UI_ICON_SORT);
}
