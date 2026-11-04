/* File Explorer: the right-click menu and the sort menu. The panel itself (layout, mouse, keys, drawing) is the HarisUI UiMenu
 * (ui.h); this file fills it with the items for what was clicked and runs the one that is chosen. */
#include "explorer.h"
#include "deskfiles.h"

static void add(int item, const char *label, const char *hint, int flags)
{
    ui_menu_add(&EX_MENU, item, label, hint, flags);
}

/* Open the menu at (x, y), kept inside the window's content area. kind: 0 = folder menu, 1 = entry menu, 2 = sort menu. */
void ex_menu_open(int x, int y, int kind)
{
    ui_menu_clear(&EX_MENU);
    if (kind == 2)
    {
        add(M_SORT_NAME, "Name", 0, EX_SORT_KEY == VFS_SORT_NAME ? UI_MENU_CHECKED : 0);
        add(M_SORT_SIZE, "Size", 0, EX_SORT_KEY == VFS_SORT_SIZE ? UI_MENU_CHECKED : 0);
        add(M_SORT_TYPE, "Type", 0, EX_SORT_KEY == VFS_SORT_TYPE ? UI_MENU_CHECKED : 0);
        ui_menu_separator(&EX_MENU);
        add(M_SORT_ASC, "Ascending", 0, !EX_SORT_DESC ? UI_MENU_CHECKED : 0);
        add(M_SORT_DESC, "Descending", 0, EX_SORT_DESC ? UI_MENU_CHECKED : 0);
    }
    else if (kind == 1)
    {
        add(M_OPEN, "Open", "Enter", 0);
        add(M_SEND, "Send to desktop", 0, 0);
        ui_menu_separator(&EX_MENU);
        add(M_CUT, "Cut", "Ctrl+X", 0);
        add(M_COPY, "Copy", "Ctrl+C", 0);
        ui_menu_separator(&EX_MENU);
        add(M_RENAME, "Rename", "F2", 0);
        add(M_DELETE, "Delete", "Del", UI_MENU_DANGER);
    }
    else
    {
        add(M_NEW_FOLDER, "New folder", "Ctrl+Shift+N", 0);
        add(M_NEW_FILE, "New text file", 0, 0);
        ui_menu_separator(&EX_MENU);
        if (EX_CLIP_ID >= 0 && vfs_node(EX_CLIP_ID)) /* nothing to paste: no entry */
        {
            add(M_PASTE, "Paste", "Ctrl+V", 0);
            ui_menu_separator(&EX_MENU);
        }
        add(M_REFRESH, "Refresh", "F5", 0);
    }
    ui_menu_open(&EX_MENU, x, y, wm_content(EX_WIN));
}

void ex_menu_run(int item)
{
    EX_MENU.open = 0;
    switch (item)
    {
    case M_OPEN:
        ex_open_item(EX_SEL);
        break;
    case M_RENAME:
        ex_start_rename(EX_SEL);
        break;
    case M_DELETE:
        ex_ask_delete(EX_SEL);
        break;
    case M_NEW_FOLDER:
        ex_new_item(1);
        break;
    case M_NEW_FILE:
        ex_new_item(0);
        break;
    case M_REFRESH:
        ex_refresh();
        break;
    case M_CUT:
        ex_clip_set(1);
        break;
    case M_COPY:
        ex_clip_set(0);
        break;
    case M_PASTE:
        ex_paste();
        break;
    case M_SEND:
        if (EX_SEL >= 0 && EX_SEL < EX_NITEMS)
            deskfiles_send(EX_ITEMS[EX_SEL]); /* a copy goes into the Desktop folder; a toast says how it went */
        break;
    case M_SORT_NAME:
        ex_set_sort(VFS_SORT_NAME, EX_SORT_DESC);
        break;
    case M_SORT_SIZE:
        ex_set_sort(VFS_SORT_SIZE, EX_SORT_DESC);
        break;
    case M_SORT_TYPE:
        ex_set_sort(VFS_SORT_TYPE, EX_SORT_DESC);
        break;
    case M_SORT_ASC:
        ex_set_sort(EX_SORT_KEY, 0);
        break;
    case M_SORT_DESC:
        ex_set_sort(EX_SORT_KEY, 1);
        break;
    default:
        break;
    }
}
