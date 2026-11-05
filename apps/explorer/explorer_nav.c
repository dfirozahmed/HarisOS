/* File Explorer: current folder, history, sorting, window layout and selection. */
#include "explorer.h"
#include "console.h"
#include "exec.h"

void ex_update_nav_buttons(void)
{
    if (!EX_WIN)
        return;
    EX_WIN->btn[BTN_BACK].enabled = EX_HIST_N > 0;
    EX_WIN->btn[BTN_FORWARD].enabled = EX_FWD_N > 0;
    EX_WIN->btn[BTN_UP].enabled = EX_CUR != VFS_ROOT;
}

static void note_for_folder(void)
{
    const VfsNode *cur = vfs_node(EX_CUR);
    if (cur && !cur->loaded)
        EX_NOTE = "This folder could not be read.";
    else if (cur && cur->truncated)
        EX_NOTE = "This folder has more items than can be shown here.";
}

void ex_reload(void)
{
    int focus = (EX_SEL >= 0 && EX_SEL < EX_NITEMS) ? EX_ITEMS[EX_SEL] : -1; /* by node: the rows may be in another order now */
    int total = vfs_list_by(EX_CUR, EX_ITEMS, VFS_MAX_NODES, EX_SORT_KEY, EX_SORT_DESC);
    EX_NITEMS = total < VFS_MAX_NODES ? total : VFS_MAX_NODES;
    ex_fix_selection(focus); /* the selected entries stay selected, those that are gone are not */
    if (EX_SCROLL > EX_NITEMS - 1)
        EX_SCROLL = EX_NITEMS > 0 ? EX_NITEMS - 1 : 0;
    ex_update_nav_buttons();
}

/* ask the drive again (changed entries show up), then list */
void ex_refresh(void)
{
    if (EX_RENAME_ID >= 0 && !ex_end_rename(1))
        return;
    EX_NOTE = 0;
    vfs_refresh(EX_CUR);
    ex_reload(); /* keeps the selection */
    note_for_folder();
}

void ex_go_to(int dir, int push)
{
    const VfsNode *n = vfs_node(dir);
    if (!n || !n->is_dir || dir == EX_CUR)
        return;
    ex_end_addr(0);
    if (EX_RENAME_ID >= 0 && !ex_end_rename(1))
        ex_end_rename(0); /* leaving the folder: keep the old name if the new one is not usable */
    if (push)
    {
        EX_FWD_N = 0; /* a new path ends the forward list */
        if (EX_HIST_N == HIST_MAX)
        {
            for (int i = 1; i < HIST_MAX; i++)
                EX_HIST[i - 1] = EX_HIST[i];
            EX_HIST_N--;
        }
        EX_HIST[EX_HIST_N++] = EX_CUR;
    }
    EX_CUR = dir;
    ex_clear_sel();
    EX_SCROLL = 0;
    EX_LAST_ROW = -1;
    EX_NOTE = 0;
    ex_reload();
    note_for_folder();
    if (EX_NITEMS > 0)
        ex_select_only(0);
}

void ex_go_back(void)
{
    if (EX_HIST_N == 0)
        return;
    int dir = EX_HIST[--EX_HIST_N];
    if (EX_FWD_N < HIST_MAX)
        EX_FWD[EX_FWD_N++] = EX_CUR; /* so Forward can come back here */
    if (!vfs_node(dir))
    {
        ex_update_nav_buttons();
        return;
    }
    ex_go_to(dir, 0);
}

void ex_go_up(void)
{
    const VfsNode *n = vfs_node(EX_CUR);
    if (!n || EX_CUR == VFS_ROOT)
        return;
    int from = EX_CUR;
    ex_go_to(n->parent, 1);
    for (int i = 0; i < EX_NITEMS; i++) /* select the folder we just left */
        if (EX_ITEMS[i] == from)
            ex_select_only(i);
}

/* A program (an ELF file): open a console window for it and start it. The program's thread runs by itself from here;
 * what it prints appears in the window, what the user types there is what it reads. Returns a message for the status
 * bar if it could not be started, else NULL. */
static const char *run_program(int node, const VfsNode *n)
{
    static char note[100];
    int con = console_window_open_for(n->name, exec_node_subsystem(node));
    if (con < 0)
        return "Cannot run: no free console or window (close one first).";
    int err = 0;
    int id = exec_run_node(node, n->name, con, &err);
    if (id >= 0)
        return 0;

    const char *why = exec_error_text(id, err);
    const char *head = "Cannot run: ";
    int len = 0;
    for (; head[len]; len++)
        note[len] = head[len];
    for (int i = 0; why[i] && len < (int)sizeof(note) - 2; i++)
        note[len++] = why[i];
    note[len++] = '.';
    note[len] = 0;
    console_write(con, note, (unsigned)len); /* the window stays, so the reason is not lost */
    console_write(con, "\n", 1);
    console_finish(con, -1);
    return note;
}

void ex_open_item(int row)
{
    if (row < 0 || row >= EX_NITEMS)
        return;
    const VfsNode *n = vfs_node(EX_ITEMS[row]);
    if (!n)
        return;
    int kind = ex_kind_of(n);
    if (n->is_dir)
        ex_go_to(EX_ITEMS[row], 1);
    else if (kind == FK_TEXT || kind == FK_CODE) /* plain text: Notepad */
    {
        char path[UI_EDIT_MAX + 1];
        vfs_path(EX_ITEMS[row], path, (int)sizeof path);
        EX_NOTE = notepad_open_file(path); /* NULL: opened (or the question about unsaved text was asked) */
    }
    else if (kind == FK_IMAGE)
    {
        char path[UI_EDIT_MAX + 1];
        vfs_path(EX_ITEMS[row], path, (int)sizeof path);
        EX_NOTE = imageviewer_open_file(path); /* NULL: opened */
    }
    else if (kind == FK_PROGRAM)
        EX_NOTE = run_program(EX_ITEMS[row], n);
    else
        EX_NOTE = "No app can open this file yet.";
}

void ex_go_forward(void)
{
    if (EX_FWD_N == 0)
        return;
    int dir = EX_FWD[--EX_FWD_N];
    int keep = EX_FWD_N; /* a normal ex_go_to() forgets the forward list; this one must not */
    if (vfs_node(dir))
        ex_go_to(dir, 1);
    EX_FWD_N = keep;
    ex_update_nav_buttons();
}

Layout ex_layout(UiRect body)
{
    Layout l;
    UiRect b = body;
    l.toolbar = ui_cut_top(&b, TOOLBAR_H);
    l.status = ui_cut_bottom(&b, STATUS_H);
    int side = EX_SIDE_W;
    if (side > b.w - LIST_W_MIN) /* a narrow window: the list wins */
        side = b.w - LIST_W_MIN;
    if (side < SIDE_W_MIN)
        side = SIDE_W_MIN;
    l.side = ui_cut_left(&b, side);
    l.head = ui_cut_top(&b, HEAD_H);
    l.list = b;
    return l;
}

int ex_visible_rows(const Layout *l)
{
    int n = l->list.h / ROW_H;
    return n < 1 ? 1 : n;
}

void ex_clamp_scroll(const Layout *l)
{
    int max_scroll = EX_NITEMS - ex_visible_rows(l);
    if (max_scroll < 0)
        max_scroll = 0;
    if (EX_SCROLL > max_scroll)
        EX_SCROLL = max_scroll;
    if (EX_SCROLL < 0)
        EX_SCROLL = 0;
}

/* The wheel, the keys and selecting a row move EX_SCROLL directly, so the scrollbar is brought in step before every use. */
void ex_sb_sync(const Layout *l)
{
    EX_SB.total = EX_NITEMS;
    EX_SB.page = ex_visible_rows(l);
    EX_SB.pos = EX_SCROLL;
    EX_SB.track = ui_rect(l->list.x + l->list.w - UI_SCROLL_W, l->list.y, UI_SCROLL_W, l->list.h);
}

void ex_select_row(UiRect body, int row)
{
    if (row < 0 || row >= EX_NITEMS)
        return;
    ex_select_only(row);
    EX_NOTE = 0;
    ex_scroll_to(body, row);
}

void ex_scroll_to(UiRect body, int row)
{
    if (row < 0 || row >= EX_NITEMS)
        return;
    Layout l = ex_layout(body);
    int vis = ex_visible_rows(&l);
    if (row < EX_SCROLL)
        EX_SCROLL = row;
    if (row >= EX_SCROLL + vis)
        EX_SCROLL = row - vis + 1;
    ex_clamp_scroll(&l);
}

int ex_row_of(int id)
{
    for (int i = 0; i < EX_NITEMS; i++)
        if (EX_ITEMS[i] == id)
            return i;
    return -1;
}

/* Change the order of the list; the selected entries stay selected (the cursor one in view). */
void ex_set_sort(int key, int desc)
{
    if (EX_RENAME_ID >= 0 && !ex_end_rename(1))
        return;
    EX_SORT_KEY = key;
    EX_SORT_DESC = desc;
    ex_reload();
    if (EX_SEL >= 0 && EX_WIN)
        ex_scroll_to(wm_content(EX_WIN), EX_SEL);
    EX_LAST_ROW = -1;
}

int ex_place_count(void)
{
    int n = 1 + vfs_volume_count();
    return n < PLACE_MAX ? n : PLACE_MAX;
}

int ex_place_id(int i) { return i == 0 ? VFS_ROOT : vfs_volume_node(i - 1); }


/* A drive was plugged in or pulled out (a USB stick): list the folder again, and if the folder we were in lived on
 * a drive that is gone, go back to "This PC". Remembered folders that no longer exist are forgotten. Returns 1 if
 * the window needs a redraw. */
int ex_sync_drives(void)
{
    static unsigned seen = ~0u;
    unsigned gen = vfs_generation();
    if (gen == seen)
        return 0;
    seen = gen;

    int n = 0;
    for (int i = 0; i < EX_HIST_N; i++)
        if (vfs_node(EX_HIST[i]))
            EX_HIST[n++] = EX_HIST[i];
    EX_HIST_N = n;
    n = 0;
    for (int i = 0; i < EX_FWD_N; i++)
        if (vfs_node(EX_FWD[i]))
            EX_FWD[n++] = EX_FWD[i];
    EX_FWD_N = n;
    ex_clip_count(); /* forgets clipboard entries that are gone */
    if (EX_RENAME_ID >= 0 && !vfs_node(EX_RENAME_ID))
        ex_end_rename(0);

    if (!vfs_node(EX_CUR))
    {
        EX_CUR = VFS_ROOT;
        ex_clear_sel();
        EX_SCROLL = 0;
        EX_NOTE = "The drive was removed.";
    }
    EX_LAST_ROW = EX_HOVER_ROW = EX_HOVER_PLACE = -1;
    ex_reload();
    ex_update_nav_buttons();
    return 1;
}
