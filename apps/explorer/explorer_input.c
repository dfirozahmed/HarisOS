/* File Explorer: mouse and keyboard handlers of the window. */
#include "explorer.h"
#include "taskbar.h"
#include "dnd.h"
#include "utf8.h"

#define DND_START 6 /* pixels the pointer must move with the button held on a row before the entries are dragged out */

/* is (x, y) on the divider between the sidebar and the list (the grab zone for resizing the sidebar)? */
static int on_divider(const Layout *l, int x, int y)
{
    int edge = l->side.x + l->side.w;
    return x >= edge - SIDE_GRAB && x < edge + SIDE_GRAB && y >= l->side.y && y < l->side.y + l->side.h;
}

/* the column title at x in the header */
static int head_key(const Layout *l, int x)
{
    Cols cols = ex_columns(l);
    return x >= cols.size_x ? VFS_SORT_SIZE : (cols.show_type && x >= cols.type_x) ? VFS_SORT_TYPE : VFS_SORT_NAME;
}

/* Mark list row `row` (an index into the items) to be drawn again, if it is in view. */
static void mark_row(const Layout *l, int row)
{
    int i = row - EX_SCROLL;
    if (row < 0 || i < 0 || i > ex_visible_rows(l))
        return;
    int top = l->list.y + i * ROW_H, bottom = top + ROW_H;
    if (bottom > l->list.y + l->list.h)
        bottom = l->list.y + l->list.h;
    wm_add_damage(ui_rect(l->list.x, top, l->list.w, bottom - top));
}

/* something is selected that the toolbar's Cut / Copy / Rename / Delete work on (see ex_sync_buttons) */
static int has_selection(void)
{
    return ex_sel_actionable() > 0;
}

/* pointer shape: a left-right double arrow on the divider, and for as long as it is being dragged */
int ex_cursor(Window *w, int x, int y)
{
    if (EX_SIDE_DRAG)
        return GFX_CURSOR_SIZE_H;
    if (EX_MENU.open || EX_RENAME_ID >= 0)
        return GFX_CURSOR_ARROW;
    Layout l = ex_layout(wm_content(w));
    return on_divider(&l, x, y) ? GFX_CURSOR_SIZE_H : GFX_CURSOR_ARROW;
}

int ex_press(Window *w, int x, int y)
{
    UiRect body = wm_content(w);
    Layout l = ex_layout(body);

    EX_BAND = EX_DND_PEND = 0; /* a new press: nothing of an earlier one is left (its release may have been missed) */
    int tip_was_shown = EX_TIP_SHOWN;
    EX_TIP_BTN = -1; /* a click hides the tooltip until the pointer moves again */
    EX_TIP_SHOWN = 0;
    int tip_marked = tip_was_shown && ex_tip_mark(w, 0); /* its box is drawn again (the whole window if not known) */
    if (EX_MENU.open) /* a click on an item shows it pressed and it runs when the button is released (ex_drag); a click anywhere else only closes the menu */
    {
        ui_menu_press(&EX_MENU, x, y);
        return 1;
    }
    if (EX_RENAME_ID >= 0)
    {
        UiRect f = w->edits[0].field;
        if (f.w > 0 && ui_contains(f, x, y))
        {
            ui_edit_mouse(&w->edits[0], x, y, 1);
            ui_caret_restart();
            return 1;
        }
        if (!ex_end_rename(1)) /* name refused: stay in the field */
            return 1;
        l = ex_layout(body);
    }
    if (EX_ADDR_EDIT) /* a click in the path field moves the caret; anywhere else gives the field up */
    {
        UiRect f = w->edits[0].field;
        if (f.w > 0 && ui_contains(f, x, y))
        {
            ui_edit_mouse(&w->edits[0], x, y, 1);
            ui_caret_restart();
            return 1;
        }
        ex_end_addr(0);
    }
    if (ui_contains(ex_path_bar(&l), x, y)) /* a name goes to that folder; the empty part types a path */
    {
        int dir = ex_crumb_at(&l, x, y);
        if (dir >= 0)
            ex_go_to(dir, 1);
        else
            ex_start_addr_edit();
        return 1;
    }

    if (on_divider(&l, x, y)) /* hold it and drag to resize the sidebar */
    {
        EX_SIDE_DRAG = 1;
        return 1;
    }

    UiRect pill;
    if (ex_lock_rect(&l, &pill) && ui_contains(pill, x, y)) /* the lock in the status bar */
    {
        ex_toggle_lock();
        return 1;
    }
    if (ui_contains(l.side, x, y))
    {
        int row = (y - (l.side.y + 32)) / (PLACE_H + 2);
        if (y >= l.side.y + 32 && row >= 0 && row < ex_place_count() && ex_place_id(row) >= 0)
        {
            EX_PRESS_PLACE = row; /* shown pressed; it opens when the button is released (ex_drag) */
            wm_add_damage(l.side);
            return 2;
        }
        return 0;
    }
    if (ui_contains(l.head, x, y)) /* a column title: sort by it, again to reverse */
    {
        EX_PRESS_HEAD = head_key(&l, x); /* shown pressed; it sorts when the button is released (ex_drag) */
        wm_add_damage(l.head);
        return 2;
    }
    if (!ui_contains(l.list, x, y))
        return 0;

    ex_sb_sync(&l); /* the scrollbar: grab the thumb (ex_drag follows it) or click the track to move a page */
    if (ui_scroll_press(&EX_SB, x, y))
    {
        EX_SCROLL = EX_SB.pos;
        wm_add_damage(l.list);
        return (tip_was_shown && !tip_marked) ? 1 : 2;
    }

    int row = EX_SCROLL + (y - l.list.y) / ROW_H;
    if (row < 0 || row >= EX_NITEMS)
    {
        /* Empty space: a click there clears the selection; holding the button and moving draws a rubber band that selects the rows it
         * touches (with Ctrl or Shift the band adds to what is selected). */
        int additive = (EX_MODS & (KMOD_CTRL | KMOD_SHIFT)) != 0;
        int changed = (!additive && EX_SEL >= 0) || EX_NOTE;
        EX_NOTE = 0;
        EX_LAST_ROW = -1;
        if (EX_NITEMS > 0)
        {
            int cy = y - l.list.y + EX_SCROLL * ROW_H;
            ex_band_start(additive);
            EX_BAND = 1;
            EX_BAND_X0 = EX_BAND_X1 = x;
            EX_BAND_Y0 = EX_BAND_Y1 = cy;
        }
        else
            ex_clear_sel();
        return changed;
    }
    if (EX_MODS & (KMOD_CTRL | KMOD_SHIFT)) /* several: Shift takes the rows from the anchor to this one, Ctrl adds or drops this one */
    {
        EX_LAST_ROW = -1; /* not the first click of a double click */
        EX_NOTE = 0;
        if (EX_MODS & KMOD_SHIFT)
            ex_select_range(row);
        else
            ex_select_toggle(row);
        return 1; /* any number of rows changed: the whole window is drawn again */
    }
    if (ex_is_selected(row) && ex_sel_count() > 1) /* one of several selected: keep them all, they may be dragged out; a click without a drag narrows the selection (ex_drag) */
    {
        EX_DND_PEND = 1;
        EX_DND_X = x;
        EX_DND_Y = y;
        EX_DND_ROW = row;
        return (tip_was_shown && !tip_marked) ? 1 : 0;
    }
    unsigned long long now = timer_ms();
    int double_click = row == EX_LAST_ROW && now - EX_LAST_CLICK_MS <= DOUBLE_CLICK_MS;
    EX_LAST_ROW = row;
    EX_LAST_CLICK_MS = now;
    int old_sel = EX_SEL, old_scroll = EX_SCROLL, had = has_selection(), had_many = ex_sel_count() > 1;
    ex_select_row(body, row);
    if (double_click)
    {
        EX_LAST_ROW = -1;
        ex_open_item(row);
        return 1;
    }
    EX_DND_PEND = 1; /* the entry may be dragged out of the window (ex_drag) */
    EX_DND_X = x;
    EX_DND_Y = y;
    EX_DND_ROW = row;
    /* A plain selection: only the row that lost it, the row that got it and the status bar (it names the selection)
     * look different; the toolbar too when Cut / Copy / Rename / Delete become usable or not. The whole window is
     * drawn again only when the list scrolled (a tooltip the click hid: only its box, marked above). */
    if (EX_SCROLL != old_scroll || had_many || (tip_was_shown && !tip_marked)) /* (a click that ends a several-row selection changes rows all over) */
        return 1;
    mark_row(&l, old_sel);
    mark_row(&l, row);
    wm_add_damage(l.status);
    if (has_selection() != had)
        wm_add_damage(l.toolbar);
    return 2;
}

/* holding the button after a press in the rename field selects text */
int ex_drag(Window *w, int x, int y, unsigned buttons)
{
    if (EX_DND) /* entries are being dragged: the desktop draws the card; letting go over the bare desktop sends them there */
    {
        if (buttons & 1)
            dnd_move(x, y);
        else
        {
            EX_DND = 0;
            dnd_drop(x, y);
        }
        return 0;
    }
    if (EX_DND_PEND)
    {
        if (buttons & 1)
        {
            int dx = x - EX_DND_X, dy = y - EX_DND_Y;
            if (dx * dx + dy * dy < DND_START * DND_START)
                return 0;
            EX_DND_PEND = 0;
            int ids[EX_SEL_MAX];
            int n = ex_selected(ids, EX_SEL_MAX);
            if (n == 0) /* only a drive: nothing to carry */
                return 0;
            dnd_begin(ids, n, 0);
            dnd_move(x, y);
            EX_DND = 1;
            return 0;
        }
        EX_DND_PEND = 0; /* released without a drag */
        if (ex_sel_count() > 1) /* a plain click on one of several: that one alone stays selected */
        {
            EX_LAST_ROW = EX_DND_ROW;
            EX_LAST_CLICK_MS = timer_ms();
            ex_select_row(wm_content(w), EX_DND_ROW);
            return 1;
        }
        return 0;
    }
    if (EX_BAND)
    {
        Layout l = ex_layout(wm_content(w));
        if (!(buttons & 1))
        {
            EX_BAND = 0;
            return 1;
        }
        if (y < l.list.y) /* above / below the list: it scrolls a row at a time while the pointer moves there */
            EX_SCROLL--;
        else if (y >= l.list.y + l.list.h)
            EX_SCROLL++;
        ex_clamp_scroll(&l);
        int cx = x < l.list.x ? l.list.x : x >= l.list.x + l.list.w ? l.list.x + l.list.w - 1 : x;
        int cy = (y < l.list.y ? l.list.y : y >= l.list.y + l.list.h ? l.list.y + l.list.h - 1 : y) - l.list.y + EX_SCROLL * ROW_H;
        if (cx == EX_BAND_X1 && cy == EX_BAND_Y1)
            return 0;
        EX_BAND_X1 = cx;
        EX_BAND_Y1 = cy;
        ex_band_update(EX_BAND_Y0, EX_BAND_Y1);
        EX_NOTE = 0;
        wm_add_damage(l.list);
        wm_add_damage(l.status);
        wm_add_damage(l.toolbar); /* Cut / Copy / Delete become usable or not */
        return 2;
    }
    if (EX_MENU.open && EX_MENU.press >= 0)
    {
        int id = -1;
        int r = ui_menu_release(&EX_MENU, x, y, buttons, &id); /* 2 = released on the item it was pressed on */
        if (r == 2)
            ex_menu_run(id);
        return r != 0;
    }
    if (EX_SB.dragging) /* the scrollbar's thumb is held: the list follows the pointer */
    {
        Layout l = ex_layout(wm_content(w));
        ex_sb_sync(&l);
        int changed = ui_scroll_drag(&EX_SB, x, y, buttons);
        EX_SCROLL = EX_SB.pos;
        if (!changed)
            return 0;
        wm_add_damage(l.list);
        return 2;
    }
    if (EX_PRESS_PLACE >= 0 || EX_PRESS_HEAD >= 0)
    {
        Layout l = ex_layout(wm_content(w));
        int place = EX_PRESS_PLACE, head = EX_PRESS_HEAD;
        int over_place = place >= 0 && ui_contains(l.side, x, y) && y >= l.side.y + 32 && (y - (l.side.y + 32)) / (PLACE_H + 2) == place;
        int over_head = head >= 0 && ui_contains(l.head, x, y) && head_key(&l, x) == head;
        if (buttons & 1) /* still held: pressed look only while the pointer is on the same entry */
        {
            int was_p = EX_PRESS_PLACE, was_h = EX_PRESS_HEAD;
            EX_PRESS_PLACE = over_place ? place : -1;
            EX_PRESS_HEAD = over_head ? head : -1;
            if (was_p == EX_PRESS_PLACE && was_h == EX_PRESS_HEAD)
                return 0;
            wm_add_damage(place >= 0 ? l.side : l.head);
            return 2;
        }
        /* released: act only if the pointer is still on the entry that was pressed */
        EX_PRESS_PLACE = EX_PRESS_HEAD = -1;
        if (over_place)
            ex_go_to(ex_place_id(place), 1);
        else if (over_head)
            ex_set_sort(head, head == EX_SORT_KEY ? !EX_SORT_DESC : 0);
        else
        {
            wm_add_damage(place >= 0 ? l.side : l.head);
            return 2;
        }
        return 1;
    }
    if (EX_SIDE_DRAG)
    {
        if (!(buttons & 1))
        {
            EX_SIDE_DRAG = 0;
            return 0;
        }
        UiRect body = wm_content(w);
        int side = x - body.x;
        if (side > SIDE_W_MAX)
            side = SIDE_W_MAX;
        if (side > body.w - LIST_W_MIN)
            side = body.w - LIST_W_MIN;
        if (side < SIDE_W_MIN)
            side = SIDE_W_MIN;
        int changed = side != EX_SIDE_W;
        EX_SIDE_W = side;
        return changed;
    }
    if ((EX_RENAME_ID < 0 && !EX_ADDR_EDIT) || !w->edits[0].dragging)
        return 0;
    int changed = ui_edit_mouse(&w->edits[0], x, y, buttons);
    if (!changed)
        return 0;
    ui_caret_restart();
    wm_add_damage(ui_inset(w->edits[0].field, -3)); /* only the selection in the rename / address box changes */
    return 2;
}

/* right click: select the entry under the pointer and show its menu, or the folder menu on empty space */
int ex_rpress(Window *w, int x, int y)
{
    UiRect body = wm_content(w);
    Layout l = ex_layout(body);
    if (EX_RENAME_ID >= 0 && !ex_end_rename(1))
        return 1;
    ex_end_addr(0);
    EX_MENU.open = 0;
    if (!ui_contains(l.list, x, y) && !ui_contains(l.head, x, y))
        return 1; /* toolbar, sidebar, status bar: no menu */
    l = ex_layout(body);
    int row = ui_contains(l.list, x, y) ? EX_SCROLL + (y - l.list.y) / ROW_H : -1;
    if (row >= 0 && row < EX_NITEMS)
    {
        if (!ex_is_selected(row)) /* an entry that is not selected becomes the selection; one that is keeps the selection it is in */
            ex_select_row(body, row);
        else
            EX_NOTE = 0;
        ex_menu_open(x, y, 1);
    }
    else
    {
        ex_clear_sel();
        EX_NOTE = 0;
        ex_menu_open(x, y, 0);
    }
    return 1;
}

/* pointer moved: hover highlight in the list / sidebar, or in the open menu */
int ex_hover(Window *w, int x, int y)
{
    int old_row = EX_HOVER_ROW, old_place = EX_HOVER_PLACE, old_menu = EX_MENU.hover;
    int old_tip = EX_TIP_BTN, was_shown = EX_TIP_SHOWN, old_crumb = EX_HOVER_CRUMB;
    EX_TIP_BTN = -1;
    Layout lay = ex_layout(wm_content(w));
    EX_HOVER_CRUMB = (x >= 0 && !EX_MENU.open && !EX_ADDR_EDIT) ? ex_crumb_at(&lay, x, y) : -1;
    if (x >= 0 && !EX_MENU.open)
        for (int i = 0; i < w->btn_count; i++)
            if (ui_contains(w->btn[i].r, x, y))
                EX_TIP_BTN = i;
    if (EX_TIP_BTN != old_tip) /* a new button: wait again before showing its tooltip */
    {
        EX_TIP_SINCE = timer_ms();
        EX_TIP_SHOWN = 0;
    }
    EX_HOVER_ROW = EX_HOVER_PLACE = -1;
    if (EX_MENU.open)
        ui_menu_hover(&EX_MENU, x, y);
    else if (x >= 0)
    {
        Layout l = ex_layout(wm_content(w));
        if (ui_contains(l.list, x, y) && x < l.list.x + l.list.w - UI_SCROLL_W) /* the scrollbar column is not a row */
        {
            int row = EX_SCROLL + (y - l.list.y) / ROW_H;
            EX_HOVER_ROW = row < EX_NITEMS ? row : -1;
        }
        else if (ui_contains(l.side, x, y) && y >= l.side.y + 32)
        {
            int p = (y - (l.side.y + 32)) / (PLACE_H + 2);
            EX_HOVER_PLACE = p < ex_place_count() ? p : -1;
        }
    }
    if (old_menu != EX_MENU.hover || old_crumb != EX_HOVER_CRUMB)
        return 1; /* menu and path bar: rare, redraw the whole window */
    ex_sb_sync(&lay);
    int sb_changed = ui_scroll_hover(&EX_SB, EX_MENU.open ? -1 : x, y); /* the bar widens under the pointer */
    if (sb_changed)
        wm_add_damage(EX_SB.track);
    int tip_gone = was_shown && !EX_TIP_SHOWN;
    if (tip_gone && !ex_tip_mark(w, 0)) /* the pointer left the button: only the tooltip's box is drawn again */
        return 1;
    if (old_row == EX_HOVER_ROW && old_place == EX_HOVER_PLACE)
        return (tip_gone || sb_changed) ? 2 : 0;

    /* Only the highlight of a row or sidebar entry moved: redraw just those. A row's pixels are where the row is
     * now, also after the list scrolled (they moved with it). */
    Layout l = ex_layout(wm_content(w));
    if (old_row != EX_HOVER_ROW)
    {
        mark_row(&l, old_row);
        mark_row(&l, EX_HOVER_ROW);
    }
    if (old_place != EX_HOVER_PLACE)
        wm_add_damage(l.side);
    return 2;
}

int ex_wheel(Window *w, int steps)
{
    Layout l = ex_layout(wm_content(w));
    int old = EX_SCROLL;
    EX_SCROLL += steps * WHEEL_ROWS;
    ex_clamp_scroll(&l);
    int rows = EX_SCROLL - old;
    if (rows == 0)
        return 0;

    /* Only the rows and the scrollbar (inside l.list) change. The rows that stay visible just moved, so move
     * their pixels and draw only the rows that came into view. That is valid while this window is on top
     * (nothing over the list to be moved with it) and the list does not reach the taskbar. With a GPU compositor (the back
     * buffer detached) the rows move inside this window's own picture, where there is no taskbar. */
    UiRect li = l.list;
    int dy = -rows * ROW_H, moved_w = li.w - 12; /* the scrollbar column is redrawn, not moved */
    if (wm_top() == w && (gfx_backbuffer_detached() || li.y + li.h <= (int)gfx_height() - TASKBAR_HEIGHT) &&
        gfx_scroll_rect(li.x, li.y, moved_w, li.h, dy) == 0)
    {
        if (dy < 0)
            wm_add_damage(ui_rect(li.x, li.y + li.h + dy, moved_w, -dy)); /* new rows at the bottom */
        else
            wm_add_damage(ui_rect(li.x, li.y, moved_w, dy)); /* new rows at the top */
        wm_add_damage(ui_rect(li.x + moved_w, li.y, li.w - moved_w, li.h)); /* scrollbar */
    }
    else
        wm_add_damage(li);
    return 2;
}

/* Jump to the next entry (after the selection, wrapping) whose name starts with the character `c` (a Unicode code
 * point; names are UTF-8, so the first character of a name is decoded, not just its first byte). Letters of other
 * scripts must match exactly; only ASCII ignores case. */
static void type_ahead(UiRect body, int c)
{
    c = ex_lower(c);
    for (int k = 1; k <= EX_NITEMS; k++)
    {
        int row = (EX_SEL + k + EX_NITEMS) % EX_NITEMS;
        const VfsNode *n = vfs_node(EX_ITEMS[row]);
        const char *p = n ? n->name : 0;
        if (p && *p && ex_lower((int)utf8_decode(&p)) == c)
        {
            ex_select_row(body, row);
            return;
        }
    }
}

void ex_key(Window *w, const KeyEvent *k)
{
    EX_MODS = k->mods & (KMOD_SHIFT | KMOD_CTRL); /* the mouse clicks that follow are Shift / Ctrl clicks */
    if (k->key == KEY_SHIFT)
        EX_MODS = k->pressed ? (EX_MODS | KMOD_SHIFT) : (EX_MODS & ~(unsigned)KMOD_SHIFT);
    else if (k->key == KEY_CTRL)
        EX_MODS = k->pressed ? (EX_MODS | KMOD_CTRL) : (EX_MODS & ~(unsigned)KMOD_CTRL);
    if (!k->pressed)
        return;
    if ((EX_DND || EX_DND_PEND) && k->key == KEY_ESC) /* Esc while dragging: never mind */
    {
        dnd_cancel();
        EX_DND = EX_DND_PEND = 0;
        return;
    }
    UiRect body = wm_content(w);
    Layout l = ex_layout(body);
    int page = ex_visible_rows(&l) - 1;
    if (page < 1)
        page = 1;

    if (EX_ADDR_EDIT) /* the path field has the keyboard */
    {
        if (k->key == KEY_ENTER)
            ex_end_addr(1);
        else if (k->key == KEY_ESC)
            ex_end_addr(0);
        else if (ui_edit_key(&w->edits[0], k))
        {
            EX_NOTE = 0;
            ui_caret_restart();
        }
        return;
    }
    if (EX_RENAME_ID >= 0) /* the name field has the keyboard */
    {
        if (k->key == KEY_ENTER)
            ex_end_rename(1);
        else if (k->key == KEY_ESC)
            ex_end_rename(0);
        else if (ui_edit_key(&w->edits[0], k))
        {
            EX_NOTE = 0;
            ui_caret_restart();
        }
        return;
    }
    if (EX_MENU.open)
    {
        int id = -1;
        if (ui_menu_key(&EX_MENU, k, &id) == 2) /* Enter on an item */
            ex_menu_run(id);
        return;
    }

    if (k->key == KEY_F4 || ((k->mods & KMOD_ALT) && k->key == 'd')) /* type a path */
    {
        ex_start_addr_edit();
        return;
    }
    if ((k->mods & KMOD_ALT) && k->key == KEY_LEFT)
    {
        ex_go_back();
        return;
    }
    if ((k->mods & KMOD_ALT) && k->key == KEY_RIGHT)
    {
        ex_go_forward();
        return;
    }
    if ((k->mods & KMOD_ALT) && k->key == KEY_UP)
    {
        ex_go_up();
        return;
    }
    switch (k->key)
    {
    case KEY_F5:
        ex_refresh();
        return;
    case KEY_F2:
        if (ex_sel_count() > 1)
            EX_NOTE = "Select one item to rename it.";
        else
            ex_start_rename(EX_SEL);
        return;
    case KEY_DELETE:
        ex_ask_delete();
        return;
    case KEY_BACKSPACE:
        ex_go_up();
        return;
    case KEY_ENTER:
        if (ex_sel_count() <= 1) /* Open works on one entry */
            ex_open_item(EX_SEL);
        return;
    default:
        break;
    }
    if ((k->mods & KMOD_CTRL) && !(k->mods & (KMOD_SHIFT | KMOD_ALT)) && k->key == 'a') /* select all */
    {
        ex_select_all();
        return;
    }
    if ((k->mods & KMOD_CTRL) && !(k->mods & (KMOD_SHIFT | KMOD_ALT)) && (k->key == 'x' || k->key == 'c' || k->key == 'v'))
    {
        if (k->key == 'v')
            ex_paste();
        else
            ex_clip_set(k->key == 'x');
        return;
    }
    if ((k->mods & KMOD_CTRL) && !(k->mods & (KMOD_SHIFT | KMOD_ALT)) && k->key == 'l') /* the drive's lock */
    {
        ex_toggle_lock();
        return;
    }
    if ((k->mods & KMOD_CTRL) && (k->mods & KMOD_SHIFT) && k->key == 'n')
    {
        ex_new_item(1);
        return;
    }
    if (EX_NITEMS == 0)
        return;
    int target = -1; /* the row a movement key goes to */
    switch (k->key)
    {
    case KEY_UP:
        target = EX_SEL < 0 ? 0 : EX_SEL - 1;
        break;
    case KEY_DOWN:
        target = EX_SEL < 0 ? 0 : EX_SEL + 1;
        break;
    case KEY_PAGEUP:
        target = EX_SEL - page < 0 ? 0 : EX_SEL - page;
        break;
    case KEY_PAGEDOWN:
        target = EX_SEL + page >= EX_NITEMS ? EX_NITEMS - 1 : EX_SEL + page;
        break;
    case KEY_HOME:
        target = 0;
        break;
    case KEY_END:
        target = EX_NITEMS - 1;
        break;
    default:
        if (((k->ch > ' ' && k->ch < 127) ||
             (k->ch >= 0xA0 && k->ch <= 0x10FFFF && !(k->ch >= 0xD800 && k->ch <= 0xDFFF))) && /* ASCII or a Unicode character */
            !(k->mods & (KMOD_CTRL | KMOD_ALT)))
            type_ahead(body, k->ch);
        break;
    }
    if (target >= 0 && target < EX_NITEMS)
    {
        if (k->mods & KMOD_SHIFT) /* Shift: the selection grows from its anchor to the new row */
            ex_extend_to(body, target);
        else
            ex_select_row(body, target);
    }
}

