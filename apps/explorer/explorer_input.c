/* File Explorer: mouse and keyboard handlers of the window. */
#include "explorer.h"

/* is (x, y) on the divider between the sidebar and the list (the grab zone for resizing the sidebar)? */
static int on_divider(const Layout *l, int x, int y)
{
    int edge = l->side.x + l->side.w;
    return x >= edge - SIDE_GRAB && x < edge + SIDE_GRAB && y >= l->side.y && y < l->side.y + l->side.h;
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

    EX_TIP_BTN = -1; /* a click hides the tooltip until the pointer moves again */
    EX_TIP_SHOWN = 0;
    if (EX_MENU.open) /* a click on an item runs it; a click anywhere else only closes the menu */
    {
        int hit = ex_menu_hit(x, y);
        if (hit >= 0)
            ex_menu_run(EX_MENU.items[hit]);
        else
            EX_MENU.open = 0;
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
            ex_go_to(ex_place_id(row), 1);
            return 1;
        }
        return 0;
    }
    if (ui_contains(l.head, x, y)) /* a column title: sort by it, again to reverse */
    {
        Cols cols = ex_columns(&l);
        int key = x >= cols.size_x ? VFS_SORT_SIZE : (cols.show_type && x >= cols.type_x) ? VFS_SORT_TYPE : VFS_SORT_NAME;
        ex_set_sort(key, key == EX_SORT_KEY ? !EX_SORT_DESC : 0);
        return 1;
    }
    if (!ui_contains(l.list, x, y))
        return 0;

    int row = EX_SCROLL + (y - l.list.y) / ROW_H;
    if (row < 0 || row >= EX_NITEMS)
    {
        int changed = EX_SEL >= 0 || EX_NOTE;
        EX_SEL = -1;
        EX_NOTE = 0;
        EX_LAST_ROW = -1;
        return changed;
    }
    unsigned long long now = timer_ms();
    int double_click = row == EX_LAST_ROW && now - EX_LAST_CLICK_MS <= DOUBLE_CLICK_MS;
    EX_LAST_ROW = row;
    EX_LAST_CLICK_MS = now;
    ex_select_row(body, row);
    if (double_click)
    {
        EX_LAST_ROW = -1;
        ex_open_item(row);
    }
    return 1;
}

/* holding the button after a press in the rename field selects text */
int ex_drag(Window *w, int x, int y, unsigned buttons)
{
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
    if (EX_RENAME_ID < 0 || !w->edits[0].dragging)
        return 0;
    int changed = ui_edit_mouse(&w->edits[0], x, y, buttons);
    if (changed)
        ui_caret_restart();
    return changed;
}

/* right click: select the entry under the pointer and show its menu, or the folder menu on empty space */
int ex_rpress(Window *w, int x, int y)
{
    UiRect body = wm_content(w);
    Layout l = ex_layout(body);
    if (EX_RENAME_ID >= 0 && !ex_end_rename(1))
        return 1;
    EX_MENU.open = 0;
    if (!ui_contains(l.list, x, y) && !ui_contains(l.head, x, y))
        return 1; /* toolbar, sidebar, status bar: no menu */
    l = ex_layout(body);
    int row = ui_contains(l.list, x, y) ? EX_SCROLL + (y - l.list.y) / ROW_H : -1;
    if (row >= 0 && row < EX_NITEMS)
    {
        ex_select_row(body, row);
        ex_menu_open(x, y, 1);
    }
    else
    {
        EX_SEL = -1;
        EX_NOTE = 0;
        ex_menu_open(x, y, 0);
    }
    return 1;
}

/* pointer moved: hover highlight in the list / sidebar, or in the open menu */
int ex_hover(Window *w, int x, int y)
{
    int old_row = EX_HOVER_ROW, old_place = EX_HOVER_PLACE, old_menu = EX_MENU.hover;
    int old_tip = EX_TIP_BTN, was_shown = EX_TIP_SHOWN;
    EX_TIP_BTN = -1;
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
        EX_MENU.hover = x < 0 ? -1 : ex_menu_hit(x, y);
    else if (x >= 0)
    {
        Layout l = ex_layout(wm_content(w));
        if (ui_contains(l.list, x, y))
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
    return old_row != EX_HOVER_ROW || old_place != EX_HOVER_PLACE || old_menu != EX_MENU.hover || was_shown != EX_TIP_SHOWN;
}

int ex_wheel(Window *w, int steps)
{
    Layout l = ex_layout(wm_content(w));
    int old = EX_SCROLL;
    EX_SCROLL += steps * WHEEL_ROWS;
    ex_clamp_scroll(&l);
    return EX_SCROLL != old;
}

/* Jump to the next entry (after the selection, wrapping) whose name starts with `c`. */
static void type_ahead(UiRect body, int c)
{
    c = ex_lower(c);
    for (int k = 1; k <= EX_NITEMS; k++)
    {
        int row = (EX_SEL + k + EX_NITEMS) % EX_NITEMS;
        const VfsNode *n = vfs_node(EX_ITEMS[row]);
        if (n && ex_lower((unsigned char)n->name[0]) == c)
        {
            ex_select_row(body, row);
            return;
        }
    }
}

void ex_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    UiRect body = wm_content(w);
    Layout l = ex_layout(body);
    int page = ex_visible_rows(&l) - 1;
    if (page < 1)
        page = 1;

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
        if (k->key == KEY_UP)
            ex_menu_step(-1);
        else if (k->key == KEY_DOWN)
            ex_menu_step(1);
        else if (k->key == KEY_ENTER && EX_MENU.hover >= 0)
            ex_menu_run(EX_MENU.items[EX_MENU.hover]);
        else if (k->key == KEY_ESC)
            EX_MENU.open = 0;
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
        ex_start_rename(EX_SEL);
        return;
    case KEY_DELETE:
        ex_ask_delete(EX_SEL);
        return;
    case KEY_BACKSPACE:
        ex_go_up();
        return;
    case KEY_ENTER:
        ex_open_item(EX_SEL);
        return;
    default:
        break;
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
    switch (k->key)
    {
    case KEY_UP:
        ex_select_row(body, EX_SEL < 0 ? 0 : EX_SEL - 1);
        break;
    case KEY_DOWN:
        ex_select_row(body, EX_SEL < 0 ? 0 : EX_SEL + 1);
        break;
    case KEY_PAGEUP:
        ex_select_row(body, EX_SEL - page < 0 ? 0 : EX_SEL - page);
        break;
    case KEY_PAGEDOWN:
        ex_select_row(body, EX_SEL + page >= EX_NITEMS ? EX_NITEMS - 1 : EX_SEL + page);
        break;
    case KEY_HOME:
        ex_select_row(body, 0);
        break;
    case KEY_END:
        ex_select_row(body, EX_NITEMS - 1);
        break;
    default:
        if (k->ch > ' ' && k->ch < 127 && !(k->mods & (KMOD_CTRL | KMOD_ALT)))
            type_ahead(body, k->ch);
        break;
    }
}

