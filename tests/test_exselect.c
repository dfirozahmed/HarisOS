/* Host-side tests for the File Explorer's selection (apps/explorer/explorer_select.c): one or several entries selected by node, Ctrl and Shift
 * selections, what a command works on, what the clipboard remembers, and keeping the selection when the list is made again. */
#include "explorer.h"
#include "deskfiles.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- stubs: a tiny tree. Ids 10 .. 19 are files in folder 30; ids 50, 51 are drives (children of the root) ---- */
static VfsNode NODES[64];
static void node_set(int id, int parent, int is_dir)
{
    memset(&NODES[id], 0, sizeof NODES[id]);
    NODES[id].used = 1;
    NODES[id].parent = parent;
    NODES[id].is_dir = is_dir;
}
const VfsNode *vfs_node(int id) { return id >= 0 && id < 64 && NODES[id].used ? &NODES[id] : 0; }
int vfs_is_under(int id, int anc)
{
    for (int i = id; i >= 0 && i < 64 && NODES[i].used; i = NODES[i].parent)
    {
        if (i == anc)
            return 1;
        if (NODES[i].parent == i)
            break;
    }
    return 0;
}
int ex_is_drive(const VfsNode *n) { return n && n->parent == VFS_ROOT && n->is_dir; }
int ex_row_of(int id)
{
    for (int i = 0; i < EX_NITEMS; i++)
        if (EX_ITEMS[i] == id)
            return i;
    return -1;
}
static int SCROLLED_TO = -1;
void ex_scroll_to(UiRect body, int row) { (void)body; SCROLLED_TO = row; }
static int SENT_N, SENT[EX_SEL_MAX], SEND_CALLS;
int deskfiles_send_many(const int *nodes, int n)
{
    SEND_CALLS++;
    SENT_N = n;
    for (int i = 0; i < n; i++)
        SENT[i] = nodes[i];
    return n;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

/* the rule everything keeps: the cursor row exists exactly when something is selected, and it is one of the selected */
static int invariant(void)
{
    int marked_rows = 0;
    for (int r = 0; r < EX_NITEMS; r++)
        marked_rows += ex_is_selected(r);
    if (marked_rows != ex_sel_count())
        return 0;
    if ((EX_SEL >= 0) != (ex_sel_count() > 0))
        return 0;
    return EX_SEL < 0 || ex_is_selected(EX_SEL);
}

/* the list shows ids first .. first + n - 1 */
static void list(int first, int n)
{
    EX_NITEMS = n;
    for (int i = 0; i < n; i++)
        EX_ITEMS[i] = first + i;
}

static int rows_selected(int lo, int hi) /* all of lo..hi selected, and nothing else */
{
    for (int r = 0; r < EX_NITEMS; r++)
        if (ex_is_selected(r) != (r >= lo && r <= hi))
            return 0;
    return 1;
}

int main(void)
{
    node_set(29, 0, 1); /* (a folder directly under the root counts as a drive, so the files' folder is one level down) */
    node_set(30, 29, 1);
    for (int i = 10; i < 20; i++)
        node_set(i, 30, 0);
    node_set(50, 0, 1);
    node_set(51, 0, 1);

    list(10, 8);
    ex_clear_sel();
    CHECK(ex_sel_count() == 0 && EX_SEL == -1 && invariant());

    /* one entry */
    ex_select_only(2);
    CHECK(ex_sel_count() == 1 && EX_SEL == 2 && EX_ANCHOR == 2 && rows_selected(2, 2) && invariant());
    ex_select_only(5);
    CHECK(ex_sel_count() == 1 && EX_SEL == 5 && rows_selected(5, 5));
    ex_select_only(-1);
    CHECK(ex_sel_count() == 0 && EX_SEL == -1 && invariant());
    ex_select_only(2);
    ex_select_only(99);
    CHECK(ex_sel_count() == 0 && EX_SEL == -1 && invariant());
    CHECK(!ex_is_selected(-1) && !ex_is_selected(99));

    /* Ctrl+click: this one joins or leaves */
    ex_select_only(1);
    ex_select_toggle(3);
    CHECK(ex_sel_count() == 2 && EX_SEL == 3 && EX_ANCHOR == 3 && ex_is_selected(1) && ex_is_selected(3) && invariant());
    ex_select_toggle(1);
    CHECK(ex_sel_count() == 1 && EX_SEL == 3 && !ex_is_selected(1) && invariant());
    ex_select_toggle(3); /* the last one leaves: nothing is selected */
    CHECK(ex_sel_count() == 0 && EX_SEL == -1 && invariant());
    ex_select_only(1);
    ex_select_toggle(3);
    ex_select_toggle(5);
    ex_select_toggle(5); /* the cursor one leaves: the cursor goes to the first that is still selected */
    CHECK(ex_sel_count() == 2 && EX_SEL == 1 && invariant());
    ex_select_toggle(99); /* no such row: nothing happens */
    CHECK(ex_sel_count() == 2 && invariant());

    /* Shift+click: from the anchor to the row; the anchor stays, so the range can go the other way */
    ex_select_only(2);
    ex_select_range(5);
    CHECK(rows_selected(2, 5) && ex_sel_count() == 4 && EX_SEL == 5 && EX_ANCHOR == 2 && invariant());
    ex_select_range(0);
    CHECK(rows_selected(0, 2) && ex_sel_count() == 3 && EX_SEL == 0 && EX_ANCHOR == 2 && invariant());
    ex_select_range(2);
    CHECK(rows_selected(2, 2) && invariant());
    ex_clear_sel();
    ex_select_range(3); /* no anchor yet: the row itself */
    CHECK(rows_selected(3, 3) && EX_ANCHOR == 3 && invariant());
    ex_select_range(99);
    CHECK(rows_selected(3, 3));
    ex_select_toggle(6); /* Ctrl+click sets the anchor, a later Shift+click starts from it */
    ex_select_range(7);
    CHECK(rows_selected(6, 7) && EX_ANCHOR == 6 && invariant());

    /* Shift+arrow: the same, and the new end is scrolled into view */
    ex_select_only(1);
    SCROLLED_TO = -1;
    UiRect body = {0, 0, 100, 100};
    ex_extend_to(body, 3);
    CHECK(rows_selected(1, 3) && EX_SEL == 3 && SCROLLED_TO == 3);
    ex_extend_to(body, 2);
    CHECK(rows_selected(1, 2) && EX_SEL == 2 && SCROLLED_TO == 2 && invariant());
    SCROLLED_TO = -1;
    ex_extend_to(body, 99);
    CHECK(SCROLLED_TO == -1 && rows_selected(1, 2));

    /* Ctrl+A */
    ex_select_only(4);
    ex_select_all();
    CHECK(rows_selected(0, 7) && ex_sel_count() == 8 && EX_SEL == 4 && invariant());
    ex_clear_sel();
    ex_select_all();
    CHECK(ex_sel_count() == 8 && EX_SEL == 0 && invariant());
    EX_NITEMS = 0;
    ex_clear_sel();
    ex_select_all();
    CHECK(ex_sel_count() == 0 && EX_SEL == -1);
    list(10, 8);

    /* what a command works on: not the drives, in the order of the list */
    list(10, 4);
    EX_ITEMS[4] = 50;
    EX_ITEMS[5] = 51;
    EX_NITEMS = 6;
    ex_select_all();
    int ids[EX_SEL_MAX];
    CHECK(ex_selected(ids, EX_SEL_MAX) == 4 && ids[0] == 10 && ids[3] == 13 && ex_sel_actionable() == 4 && ex_sel_count() == 6);
    CHECK(ex_selected(ids, 2) == 2 && ids[1] == 11); /* never more than there is room for */
    ex_select_only(4);
    ex_select_toggle(5);
    CHECK(ex_sel_count() == 2 && ex_selected(ids, EX_SEL_MAX) == 0 && ex_sel_actionable() == 0); /* only drives */
    ex_select_toggle(2);
    CHECK(ex_selected(ids, EX_SEL_MAX) == 1 && ids[0] == 12);

    /* the list is made again: the selection stays with the entries, not the rows */
    list(10, 4);
    ex_select_only(1);
    ex_select_toggle(3); /* nodes 11 and 13, the cursor on 13 */
    EX_ITEMS[0] = 13;
    EX_ITEMS[1] = 12;
    EX_ITEMS[2] = 11;
    EX_ITEMS[3] = 10; /* sorted the other way */
    ex_fix_selection(13);
    CHECK(ex_sel_count() == 2 && EX_SEL == 0 && ex_is_selected(0) && ex_is_selected(2) && !ex_is_selected(1) && invariant());
    list(10, 4);
    ex_select_only(1);
    ex_select_toggle(3);
    list(10, 3); /* 13 is gone: 11 is still selected and it is the cursor now */
    ex_fix_selection(13);
    CHECK(ex_sel_count() == 1 && EX_SEL == 1 && ex_is_selected(1) && invariant());
    list(10, 4);
    ex_select_only(1);
    EX_ITEMS[0] = 10;
    EX_ITEMS[1] = 12;
    EX_ITEMS[2] = 13;
    EX_NITEMS = 3; /* 11, the only one selected, is gone: the entry that took its place is selected */
    ex_fix_selection(11);
    CHECK(ex_sel_count() == 1 && EX_SEL == 1 && invariant());
    EX_NITEMS = 1; /* and when that row is not there any more: the last one */
    ex_fix_selection(12);
    CHECK(ex_sel_count() == 1 && EX_SEL == 0 && invariant());
    EX_NITEMS = 0;
    ex_fix_selection(10);
    CHECK(ex_sel_count() == 0 && EX_SEL == -1 && invariant());
    list(10, 4);
    ex_clear_sel();
    EX_SEL = 2; /* code that sets only the cursor row: that row is the selection */
    ex_fix_selection(-1);
    CHECK(ex_sel_count() == 1 && EX_SEL == 2 && ex_is_selected(2) && invariant());
    EX_SEL = 9; /* a cursor past the end, with something marked: it goes to the first marked row */
    ex_fix_selection(-1);
    CHECK(EX_SEL == 2 && ex_sel_count() == 1 && invariant());
    ex_clear_sel();
    EX_SEL = 9; /* ... and with nothing marked: the last row is the selection */
    ex_fix_selection(-1);
    CHECK(EX_SEL == 3 && ex_sel_count() == 1 && ex_is_selected(3) && invariant());
    ex_clear_sel();
    ex_select_all();
    ex_fix_selection(12); /* marks of entries that are not in the list are dropped */
    EX_NITEMS = 2;
    ex_fix_selection(10);
    CHECK(ex_sel_count() == 2 && invariant());
    list(10, 4);
    ex_fix_selection(-1);
    CHECK(ex_sel_count() == 2 && rows_selected(0, 1)); /* 12 and 13 were let go: they are not selected when they come back */

    /* select_ids: exactly these (those that are in the list); the cursor on the first */
    list(10, 6);
    ex_select_all();
    int some[4] = {13, 99, 11, 12};
    ex_select_ids(some, 4);
    CHECK(ex_sel_count() == 3 && ex_is_selected(1) && ex_is_selected(2) && ex_is_selected(3) && EX_SEL == 3 && EX_ANCHOR == 3 && invariant());
    ex_select_ids(some, 0);
    CHECK(ex_sel_count() == 0 && EX_SEL == -1 && invariant());

    /* the clipboard */
    EX_CLIP[0] = 10;
    EX_CLIP[1] = 99; /* gone */
    EX_CLIP[2] = 12;
    EX_CLIP_N = 3;
    CHECK(ex_clip_count() == 2 && EX_CLIP[0] == 10 && EX_CLIP[1] == 12);
    CHECK(ex_clip_has(10) && ex_clip_has(12) && !ex_clip_has(11) && !ex_clip_has(99));
    ex_clip_drop_under(12); /* an entry goes: it leaves the clipboard */
    CHECK(ex_clip_count() == 1 && ex_clip_has(10));
    ex_clip_drop_under(30); /* its folder goes: so does everything in it */
    CHECK(ex_clip_count() == 0);
    EX_CLIP_N = 0;

    /* Send to desktop: the selected entries, not the drives, in one go */
    list(10, 4);
    EX_ITEMS[4] = 50;
    EX_NITEMS = 5;
    ex_clear_sel();
    SEND_CALLS = 0;
    EX_NOTE = 0;
    ex_send_selected();
    CHECK(SEND_CALLS == 0 && EX_NOTE == 0); /* nothing selected: nothing */
    ex_select_range(0);
    ex_select_range(2);
    ex_select_toggle(4); /* rows 0..2 and the drive */
    EX_MENU.open = 1;
    ex_send_selected();
    CHECK(SEND_CALLS == 1 && SENT_N == 3 && SENT[0] == 10 && SENT[1] == 11 && SENT[2] == 12 && !EX_MENU.open);
    ex_select_only(4); /* only a drive */
    SEND_CALLS = 0;
    ex_send_selected();
    CHECK(SEND_CALLS == 0 && EX_NOTE != 0 && strstr(EX_NOTE, "drive") != 0);

    printf("exselect tests passed\n");
    return 0;
}
