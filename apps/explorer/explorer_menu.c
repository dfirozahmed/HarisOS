/* File Explorer: the right-click menu and the sort menu. */
#include "explorer.h"

#define MENU_W 224
#define MENU_ROW_H 30
#define MENU_SEP_H 9
#define MENU_PAD 6

static const char *menu_label(int item)
{
    static const char *const L[] = {"Open", "Rename", "Delete", "New folder", "New text file", "Refresh", "Cut", "Copy", "Paste", "Name", "Size", "Type", "Ascending", "Descending"};
    return item < M_SEP ? L[item] : "";
}

static const char *menu_hint(int item)
{
    static const char *const H[] = {"Enter", "F2", "Del", "Ctrl+Shift+N", "", "F5", "Ctrl+X", "Ctrl+C", "Ctrl+V", "", "", "", "", ""};
    return item < M_SEP ? H[item] : "";
}

static int menu_height(void)
{
    int h = MENU_PAD * 2;
    for (int i = 0; i < EX_MENU.count; i++)
        h += EX_MENU.items[i] == M_SEP ? MENU_SEP_H : MENU_ROW_H;
    return h;
}

static UiRect menu_item_rect(int index)
{
    int y = EX_MENU.y + MENU_PAD;
    for (int i = 0; i < index; i++)
        y += EX_MENU.items[i] == M_SEP ? MENU_SEP_H : MENU_ROW_H;
    return ui_rect(EX_MENU.x + MENU_PAD, y, MENU_W - MENU_PAD * 2, EX_MENU.items[index] == M_SEP ? MENU_SEP_H : MENU_ROW_H);
}

/* the selectable item under (x, y), -1 if none */
int ex_menu_hit(int x, int y)
{
    for (int i = 0; i < EX_MENU.count; i++)
        if (EX_MENU.items[i] != M_SEP && ui_contains(menu_item_rect(i), x, y))
            return i;
    return -1;
}

/* Open the menu at (x, y), kept inside the window's content area. */
void ex_menu_open(int x, int y, int kind)
{
    EX_MENU.count = 0;
    EX_MENU.kind = kind;
    if (kind == 2)
    {
        EX_MENU.items[EX_MENU.count++] = M_SORT_NAME;
        EX_MENU.items[EX_MENU.count++] = M_SORT_SIZE;
        EX_MENU.items[EX_MENU.count++] = M_SORT_TYPE;
        EX_MENU.items[EX_MENU.count++] = M_SEP;
        EX_MENU.items[EX_MENU.count++] = M_SORT_ASC;
        EX_MENU.items[EX_MENU.count++] = M_SORT_DESC;
    }
    else if (kind == 1)
    {
        EX_MENU.items[EX_MENU.count++] = M_OPEN;
        EX_MENU.items[EX_MENU.count++] = M_SEP;
        EX_MENU.items[EX_MENU.count++] = M_CUT;
        EX_MENU.items[EX_MENU.count++] = M_COPY;
        EX_MENU.items[EX_MENU.count++] = M_SEP;
        EX_MENU.items[EX_MENU.count++] = M_RENAME;
        EX_MENU.items[EX_MENU.count++] = M_DELETE;
    }
    else
    {
        EX_MENU.items[EX_MENU.count++] = M_NEW_FOLDER;
        EX_MENU.items[EX_MENU.count++] = M_NEW_FILE;
        EX_MENU.items[EX_MENU.count++] = M_SEP;
        if (EX_CLIP_ID >= 0 && vfs_node(EX_CLIP_ID)) /* nothing to paste: no entry */
        {
            EX_MENU.items[EX_MENU.count++] = M_PASTE;
            EX_MENU.items[EX_MENU.count++] = M_SEP;
        }
        EX_MENU.items[EX_MENU.count++] = M_REFRESH;
    }
    UiRect b = wm_content(EX_WIN);
    int h = menu_height();
    if (x + MENU_W > b.x + b.w - 4)
        x = b.x + b.w - 4 - MENU_W;
    if (y + h > b.y + b.h - 4)
        y = b.y + b.h - 4 - h;
    EX_MENU.x = x < b.x + 4 ? b.x + 4 : x;
    EX_MENU.y = y < b.y + 4 ? b.y + 4 : y;
    EX_MENU.hover = -1;
    EX_MENU.open = 1;
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
        ex_reload();
        EX_NOTE = 0;
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

/* keyboard: move the highlight to the next / previous selectable item (wraps) */
void ex_menu_step(int dir)
{
    int i = EX_MENU.hover;
    for (int k = 0; k < EX_MENU.count; k++)
    {
        i = i < 0 ? (dir > 0 ? 0 : EX_MENU.count - 1) : (i + dir + EX_MENU.count) % EX_MENU.count;
        if (EX_MENU.items[i] != M_SEP)
        {
            EX_MENU.hover = i;
            return;
        }
    }
}

static int menu_checked(int item)
{
    switch (item)
    {
    case M_SORT_NAME:
        return EX_SORT_KEY == VFS_SORT_NAME;
    case M_SORT_SIZE:
        return EX_SORT_KEY == VFS_SORT_SIZE;
    case M_SORT_TYPE:
        return EX_SORT_KEY == VFS_SORT_TYPE;
    case M_SORT_ASC:
        return !EX_SORT_DESC;
    case M_SORT_DESC:
        return EX_SORT_DESC;
    default:
        return 0;
    }
}

void ex_menu_draw(const UiTheme *t)
{
    if (!EX_MENU.open)
        return;
    int h = menu_height();
    gfx_shadow(EX_MENU.x, EX_MENU.y, MENU_W, h, t->radius_m, 14, 0, 5, 120);
    gfx_fill_round_rect(EX_MENU.x, EX_MENU.y, MENU_W, h, t->radius_m, ui_color(t->surface));
    gfx_round_rect(EX_MENU.x, EX_MENU.y, MENU_W, h, t->radius_m, 1, ui_color(t->border));
    for (int i = 0; i < EX_MENU.count; i++)
    {
        UiRect r = menu_item_rect(i);
        int item = EX_MENU.items[i];
        if (item == M_SEP)
        {
            ui_separator(r.x + 6, r.y + r.h / 2, r.w - 12);
            continue;
        }
        if (i == EX_MENU.hover)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h - 2, t->radius_s, ui_color(t->accent), 60);
        UiRgb c = item == M_DELETE ? t->danger : t->text;
        int tx = EX_MENU.kind == 2 ? 34 : 12; /* the sort menu leaves room for a check mark */
        if (menu_checked(item))
            ui_icon(UI_ICON_CHECK, r.x + 18, r.y + (r.h - 2) / 2, 14, t->accent_hi);
        ui_text_in_color(ui_rect(r.x + tx, r.y, r.w - tx - 12, r.h - 2), menu_label(item), UI_TEXT_BODY, UI_ALIGN_LEFT, c);
        ui_text_in_color(ui_rect(r.x + 12, r.y, r.w - 24, r.h - 2), menu_hint(item), UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                         t->text_dim);
    }
}

