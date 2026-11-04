/* File Explorer: the selection (one entry or several), the clipboard's list, and what a command works on.
 *
 * The selected entries are marked by node (EX_MARK), not by row, so they stay selected when the list is sorted again or an entry is renamed.
 * EX_SEL is the row of the one that counts as "the" selected entry (the cursor: the keys move from it, the status bar names it, Open and
 * Rename work on it); with several selected it is one of them. The rule everything keeps: EX_SEL >= 0 exactly when something is marked, and
 * then EX_SEL's entry is marked. EX_ANCHOR is the row a Shift+click or Shift+arrow range starts from. */
#include "explorer.h"
#include "deskfiles.h"

static void mark(int id, int on)
{
    if (id < 0 || id >= VFS_MAX_NODES)
        return;
    if (on && !EX_MARK[id])
    {
        EX_MARK[id] = 1;
        EX_MARK_N++;
    }
    else if (!on && EX_MARK[id])
    {
        EX_MARK[id] = 0;
        EX_MARK_N--;
    }
}

static void clear_marks(void)
{
    for (int i = 0; i < VFS_MAX_NODES; i++)
        EX_MARK[i] = 0;
    EX_MARK_N = 0;
}

static int row_ok(int row) { return row >= 0 && row < EX_NITEMS; }

int ex_is_selected(int row)
{
    return row_ok(row) && EX_MARK[EX_ITEMS[row]];
}

int ex_sel_count(void)
{
    return EX_MARK_N;
}

void ex_clear_sel(void)
{
    clear_marks();
    EX_SEL = -1;
    EX_ANCHOR = -1;
}

void ex_select_only(int row)
{
    clear_marks();
    if (!row_ok(row))
    {
        EX_SEL = EX_ANCHOR = -1;
        return;
    }
    mark(EX_ITEMS[row], 1);
    EX_SEL = EX_ANCHOR = row;
}

/* the first selected row, -1 if none */
static int first_marked(void)
{
    for (int r = 0; r < EX_NITEMS; r++)
        if (EX_MARK[EX_ITEMS[r]])
            return r;
    return -1;
}

void ex_select_toggle(int row)
{
    if (!row_ok(row))
        return;
    int id = EX_ITEMS[row];
    if (EX_MARK[id])
    {
        mark(id, 0);
        if (EX_SEL == row) /* the cursor goes to another one that is still selected */
            EX_SEL = first_marked();
    }
    else
    {
        mark(id, 1);
        EX_SEL = row;
    }
    EX_ANCHOR = row;
    if (EX_MARK_N == 0)
        EX_SEL = -1;
}

void ex_select_range(int to)
{
    if (!row_ok(to))
        return;
    if (!row_ok(EX_ANCHOR))
        EX_ANCHOR = row_ok(EX_SEL) ? EX_SEL : to;
    int lo = EX_ANCHOR < to ? EX_ANCHOR : to, hi = EX_ANCHOR < to ? to : EX_ANCHOR;
    clear_marks();
    for (int r = lo; r <= hi; r++)
        mark(EX_ITEMS[r], 1);
    EX_SEL = to;
}

void ex_extend_to(UiRect body, int row)
{
    if (!row_ok(row))
        return;
    ex_select_range(row);
    EX_NOTE = 0;
    ex_scroll_to(body, row); /* the new end of the range is in view */
}

void ex_select_all(void)
{
    if (EX_NITEMS == 0)
        return;
    int keep = row_ok(EX_SEL) ? EX_SEL : 0;
    clear_marks();
    for (int r = 0; r < EX_NITEMS; r++)
        mark(EX_ITEMS[r], 1);
    EX_SEL = keep;
    if (!row_ok(EX_ANCHOR))
        EX_ANCHOR = keep;
}

void ex_select_ids(const int *ids, int n)
{
    clear_marks();
    int first = -1;
    for (int i = 0; i < n; i++)
    {
        int row = ex_row_of(ids[i]);
        if (row < 0)
            continue;
        mark(ids[i], 1);
        if (first < 0)
            first = row;
    }
    EX_SEL = EX_ANCHOR = first;
}

/* the list was made again (sorted, a folder entered, an entry came or went): keep what is selected, by node */
void ex_fix_selection(int focus_id)
{
    static unsigned char keep[VFS_MAX_NODES];
    for (int i = 0; i < VFS_MAX_NODES; i++)
        keep[i] = 0;
    int n = 0;
    for (int r = 0; r < EX_NITEMS; r++)
        if (EX_MARK[EX_ITEMS[r]])
        {
            keep[EX_ITEMS[r]] = 1;
            n++;
        }
    for (int i = 0; i < VFS_MAX_NODES; i++)
        EX_MARK[i] = keep[i];
    EX_MARK_N = n;
    if (n > 0)
    {
        int row = focus_id >= 0 ? ex_row_of(focus_id) : -1;
        EX_SEL = (row >= 0 && EX_MARK[focus_id]) ? row : first_marked();
    }
    else /* nothing marked is left: what was selected went (the neighbour takes over, as before) or only EX_SEL says what is selected */
    {
        if (EX_SEL >= EX_NITEMS)
            EX_SEL = EX_NITEMS - 1;
        if (row_ok(EX_SEL))
            mark(EX_ITEMS[EX_SEL], 1);
        else
            EX_SEL = -1;
    }
    if (!row_ok(EX_ANCHOR))
        EX_ANCHOR = EX_SEL;
}

int ex_selected(int *ids, int max)
{
    int n = 0;
    for (int r = 0; r < EX_NITEMS && n < max; r++)
    {
        int id = EX_ITEMS[r];
        if (!EX_MARK[id])
            continue;
        const VfsNode *v = vfs_node(id);
        if (v && !ex_is_drive(v)) /* a drive can be selected, but no command works on it */
            ids[n++] = id;
    }
    return n;
}

int ex_sel_actionable(void)
{
    int n = 0;
    for (int r = 0; r < EX_NITEMS; r++)
        if (EX_MARK[EX_ITEMS[r]])
        {
            const VfsNode *v = vfs_node(EX_ITEMS[r]);
            if (v && !ex_is_drive(v))
                n++;
        }
    return n;
}

/* ---- the clipboard: the entries Cut or Copy was done on ---- */
int ex_clip_count(void)
{
    int k = 0;
    for (int i = 0; i < EX_CLIP_N; i++) /* entries that are gone are forgotten (their ids may be used again) */
        if (vfs_node(EX_CLIP[i]))
            EX_CLIP[k++] = EX_CLIP[i];
    EX_CLIP_N = k;
    return k;
}

int ex_clip_has(int id)
{
    for (int i = 0; i < EX_CLIP_N; i++)
        if (EX_CLIP[i] == id)
            return 1;
    return 0;
}

void ex_clip_drop_under(int id)
{
    int k = 0;
    for (int i = 0; i < EX_CLIP_N; i++)
        if (!vfs_is_under(EX_CLIP[i], id))
            EX_CLIP[k++] = EX_CLIP[i];
    EX_CLIP_N = k;
}

/* ---- Send to desktop: the selected entries ---- */
void ex_send_selected(void)
{
    int ids[EX_SEL_MAX];
    int n = ex_selected(ids, EX_SEL_MAX);
    EX_MENU.open = 0;
    if (n == 0)
    {
        if (EX_MARK_N > 0)
            EX_NOTE = "A drive cannot be sent to the desktop.";
        return;
    }
    deskfiles_send_many(ids, n); /* a toast says how it went */
}
