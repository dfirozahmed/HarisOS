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

/* "This PC > Documents > Projects" */
static void crumb_text(char *out, int cap)
{
    char path[160];
    vfs_path(EX_CUR, path, sizeof path);
    int o = ex_put_str(out, 0, cap, "This PC");
    for (const char *p = path; *p && o < cap - 1; p++)
    {
        if (*p == '/')
            o = ex_put_str(out, o, cap, "  >  ");
        else
            out[o++] = *p;
    }
    out[o] = 0;
}

void ex_draw_toolbar(const Layout *l, const UiTheme *t)
{
    UiRect tb = l->toolbar;
    gfx_fill_rect_alpha(tb.x, tb.y, tb.w, tb.h, t->surface_alt.r, t->surface_alt.g, t->surface_alt.b, 70);
    ui_separator(tb.x, tb.y + tb.h - 1, tb.w);

    /* command row: a divider between New folder and the clipboard / rename buttons */
    gfx_fill_rect_alpha(tb.x + CMD_X0 - 9, tb.y + CMD_Y + 6, 1, 22, t->border.r, t->border.g, t->border.b, 255);

    gfx_fill_rect_alpha(tb.x + SORT_X - 8, tb.y + CMD_Y + 6, 1, 22, t->border.r, t->border.g, t->border.b, 255);

    int px = tb.x + PATH_X;
    UiRect bar = ui_rect(px, tb.y + 9, tb.w - (px - tb.x) - 10 - NAVBTN - 4, 34); /* room for Refresh on the right */
    if (bar.w < 40)
        return;
    gfx_fill_round_rect(bar.x, bar.y, bar.w, bar.h, t->radius_m, ui_color(t->surface));
    gfx_round_rect(bar.x, bar.y, bar.w, bar.h, t->radius_m, 1, ui_color(t->border));
    ex_draw_folder_icon(bar.x + 20, bar.y + bar.h / 2, 18);
    char crumbs[200];
    crumb_text(crumbs, sizeof crumbs);
    ui_text_in(ui_rect(bar.x + 38, bar.y, bar.w - 46, bar.h), crumbs, UI_TEXT_BODY, UI_ALIGN_LEFT);
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
    int idle = EX_RENAME_ID < 0;
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
    int want = EX_TIP_BTN >= 0 && !EX_MENU.open && w == wm_top() && timer_ms() - EX_TIP_SINCE >= TIP_DELAY_MS;
    if (want == EX_TIP_SHOWN)
        return 0;
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
