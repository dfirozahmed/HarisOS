/* File Explorer: rename, new folder / file, clipboard (cut / copy / paste) and delete with its dialog. */
#include "explorer.h"

/* delete confirmation: a modal dialog of the Explorer window */
static Window *DEL_WIN;
static int DEL_IDS[EX_SEL_MAX]; /* the entries the question is about */
static int DEL_N;
static char DEL_TITLE[VFS_NAME_MAX + 16], DEL_LINE[96];
static char NOTE[120]; /* the status bar's message after a command on several entries */

int ex_delete_pending(void) { return DEL_WIN != 0; }

void ex_start_rename(int row)
{
    if (!EX_WIN || row < 0 || row >= EX_NITEMS)
        return;
    const VfsNode *n = vfs_node(EX_ITEMS[row]);
    if (!n)
        return;
    ex_select_row(wm_content(EX_WIN), row);
    EX_MENU.open = 0;
    if (ex_is_drive(n))
    {
        EX_NOTE = "A drive cannot be renamed.";
        return;
    }
    if (!ex_check_writable())
        return;
    EX_RENAME_ID = EX_ITEMS[row];
    UiEdit *e = &EX_WIN->edits[0];
    ui_edit_clear(e);
    e->max = VFS_NAME_MAX - 1;
    ui_edit_set_text(e, n->name);
    int stem = e->len; /* select the name without its extension, like a file manager */
    for (int i = e->len - 1; i > 0 && !n->is_dir; i--)
        if (e->text[i] == '.')
        {
            stem = i;
            break;
        }
    e->anchor = 0;
    e->cursor = stem;
    EX_WIN->edit_count = 1;
    EX_WIN->focus = 0;
    EX_WIN->caret = 1;
    ui_caret_restart();
}

/* Enter (commit = 1) or Escape (commit = 0). Returns 0 only when the new name is refused; the
 * field then stays open so it can be fixed. */
int ex_end_rename(int commit)
{
    if (EX_RENAME_ID < 0 || !EX_WIN)
        return 1;
    UiEdit *e = &EX_WIN->edits[0];
    int id = EX_RENAME_ID;
    const VfsNode *n = vfs_node(id);
    if (commit && n && !ex_same_text(n->name, e->text) && vfs_rename(id, e->text) < 0)
    {
        ex_note_error(); /* the field stays open so the name can be fixed */
        return 0;
    }
    EX_RENAME_ID = -1;
    EX_WIN->edit_count = 0;
    EX_WIN->caret = 0;
    e->field.w = 0;
    EX_NOTE = 0;
    ex_reload(); /* the entry may sort to another place */
    int row = ex_row_of(id);
    if (row >= 0)
        ex_select_row(wm_content(EX_WIN), row);
    return 1;
}

/* is there already an entry with this name in the current folder (ignoring case)? */
static int name_taken(const char *name)
{
    for (int i = 0; i < EX_NITEMS; i++)
    {
        const VfsNode *n = vfs_node(EX_ITEMS[i]);
        if (!n)
            continue;
        const char *a = n->name, *b = name;
        while (*a && ex_lower((unsigned char)*a) == ex_lower((unsigned char)*b))
            a++, b++;
        if (*a == *b)
            return 1;
    }
    return 0;
}

/* "New folder", "New folder (2)" ... / "New file.txt", "New file (2).txt" ... */
static void make_name(int is_dir, char *out)
{
    const char *base = is_dir ? "New folder" : "New file";
    const char *ext = is_dir ? "" : ".txt";
    for (int n = 1; n < 100; n++)
    {
        int o = ex_put_str(out, 0, VFS_NAME_MAX, base);
        if (n > 1)
        {
            o = ex_put_str(out, o, VFS_NAME_MAX, " (");
            o = ex_put_uint(out, o, n);
            o = ex_put_str(out, o, VFS_NAME_MAX, ")");
        }
        o = ex_put_str(out, o, VFS_NAME_MAX, ext);
        out[o] = 0;
        if (!name_taken(out))
            return;
    }
}

void ex_new_item(int is_dir)
{
    if (!EX_WIN || (EX_RENAME_ID >= 0 && !ex_end_rename(1)))
        return;
    if (!ex_check_writable())
        return;
    char name[VFS_NAME_MAX];
    make_name(is_dir, name);
    int id = vfs_add(EX_CUR, name, is_dir, 0);
    if (id < 0)
    {
        ex_note_error();
        return;
    }
    ex_reload();
    ex_start_rename(ex_row_of(id)); /* type the real name right away */
}

/* "3 of 5 items could not be <what>: <the drive's words>." in NOTE */
static void note_failures(int failed, int total, const char *what, int first_err)
{
    int o = ex_put_uint(NOTE, 0, failed);
    o = ex_put_str(NOTE, o, sizeof NOTE, " of ");
    o = ex_put_uint(NOTE, o, total);
    o = ex_put_str(NOTE, o, sizeof NOTE, " items could not be ");
    o = ex_put_str(NOTE, o, sizeof NOTE, what);
    o = ex_put_str(NOTE, o, sizeof NOTE, ": ");
    o = ex_put_str(NOTE, o, sizeof NOTE, vfs_error_text(first_err));
    NOTE[o] = 0;
    EX_NOTE = NOTE;
}

/* Cut (cut = 1) or Copy (cut = 0): remember the selected entries. Nothing changes until Paste. */
void ex_clip_set(int cut)
{
    if (EX_RENAME_ID >= 0)
        return;
    int n = ex_selected(EX_CLIP, EX_SEL_MAX); /* not the drives */
    if (n == 0)
    {
        if (ex_sel_count() > 0)
            EX_NOTE = "A drive cannot be cut or copied.";
        return;
    }
    EX_CLIP_N = n;
    EX_CLIP_CUT = cut;
    EX_NOTE = 0;
}

/* Paste into the current folder: Cut moves the entries here, Copy makes copies (a taken name becomes "name (2)"). Each is tried by itself:
 * one that cannot be pasted does not stop the others, and the message says how many could not. */
void ex_paste(void)
{
    int n = ex_clip_count();
    if (n == 0)
        return;
    if (EX_RENAME_ID >= 0 && !ex_end_rename(1))
        return;
    int ids[EX_SEL_MAX], done[EX_SEL_MAX];
    for (int i = 0; i < n; i++)
        ids[i] = EX_CLIP[i];
    int cut = EX_CLIP_CUT;
    int same_place = 0;
    for (int i = 0; i < n; i++) /* cut and pasted in the same place: nothing to do for that one */
        if (cut && vfs_node(ids[i]) && vfs_node(ids[i])->parent == EX_CUR)
            same_place++;
    if (same_place == n)
    {
        EX_CLIP_N = 0;
        return;
    }
    if (!ex_check_writable())
        return;
    int nd = 0, failed = 0, inside = 0, first_err = 0;
    for (int i = 0; i < n; i++)
    {
        const VfsNode *v = vfs_node(ids[i]);
        if (!v || (cut && v->parent == EX_CUR))
            continue;
        if (vfs_is_under(EX_CUR, ids[i]))
        {
            inside++;
            continue;
        }
        int id = cut ? vfs_move(ids[i], EX_CUR) : vfs_copy(ids[i], EX_CUR);
        if (id < 0)
        {
            if (!failed)
                first_err = vfs_last_error();
            failed++;
            continue;
        }
        done[nd++] = id;
    }
    if (cut && (nd > 0 || failed == 0))
        EX_CLIP_N = 0; /* moved entries leave the clipboard; copied ones can be pasted again */
    EX_NOTE = 0;
    ex_reload(); /* (also after a failure: a copy between drives may have been partly done and undone) */
    if (failed > 0 && n == 1)
        EX_NOTE = vfs_error_text(first_err);
    else if (failed > 0)
        note_failures(failed, n, cut ? "moved" : "copied", first_err);
    else if (inside > 0 && nd == 0)
        EX_NOTE = cut ? "A folder cannot be moved into itself." : "A folder cannot be copied into itself.";
    if (nd > 0)
    {
        ex_select_ids(done, nd); /* what was pasted is selected */
        if (EX_WIN && EX_SEL >= 0)
            ex_scroll_to(wm_content(EX_WIN), EX_SEL);
    }
}

/* what the history and the clipboard remember of an entry that is about to disappear */
static void forget(int id)
{
    int k = 0;
    for (int i = 0; i < EX_HIST_N; i++) /* history entries that are about to disappear */
        if (!vfs_is_under(EX_HIST[i], id))
            EX_HIST[k++] = EX_HIST[i];
    EX_HIST_N = k;
    k = 0;
    for (int i = 0; i < EX_FWD_N; i++)
        if (!vfs_is_under(EX_FWD[i], id))
            EX_FWD[k++] = EX_FWD[i];
    EX_FWD_N = k;
    ex_clip_drop_under(id); /* its id may be reused by the next new entry */
}

static void do_delete(void)
{
    int ids[EX_SEL_MAX];
    int n = DEL_N;
    for (int i = 0; i < n; i++)
        ids[i] = DEL_IDS[i];
    if (DEL_WIN)
        wm_close(DEL_WIN);
    int first_row = -1, failed = 0, first_err = 0, tried = 0;
    for (int i = 0; i < n; i++)
    {
        if (!vfs_node(ids[i]))
            continue;
        int row = ex_row_of(ids[i]);
        if (row >= 0 && (first_row < 0 || row < first_row))
            first_row = row;
        forget(ids[i]);
        tried++;
        if (vfs_remove(ids[i]) < 0)
        {
            if (!failed)
                first_err = vfs_last_error();
            failed++; /* some of it may be gone */
        }
    }
    EX_LAST_ROW = -1;
    EX_NOTE = 0;
    ex_reload(); /* the removed ones are not selected any more; those that could not be removed still are */
    if (failed > 0 && tried == 1)
        EX_NOTE = vfs_error_text(first_err);
    else if (failed > 0)
        note_failures(failed, tried, "deleted", first_err);
    if (failed == 0) /* the entry next to where the first one was takes over the selection */
    {
        int row = EX_NITEMS == 0 ? -1 : (first_row < EX_NITEMS ? first_row : EX_NITEMS - 1);
        ex_select_only(row);
        if (EX_WIN && row >= 0)
            ex_scroll_to(wm_content(EX_WIN), row);
    }
}

static void on_del_yes(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    do_delete();
}

static void on_del_no(UiButton *b, void *user)
{
    (void)b;
    wm_close((Window *)user);
}

static void del_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    body = ui_inset(body, t->pad);
    ui_text_in(ui_rect(body.x, body.y, body.w, 26), DEL_TITLE, UI_TEXT_TITLE, UI_ALIGN_LEFT);
    ui_text_in_color(ui_rect(body.x, body.y + 34, body.w, 22), DEL_LINE, UI_TEXT_BODY, UI_ALIGN_LEFT, t->text);
    ui_text_in_color(ui_rect(body.x, body.y + 58, body.w, 20), "This cannot be undone (there is no Recycle Bin yet).",
                     UI_TEXT_SMALL, UI_ALIGN_LEFT, t->warning);
}

static void del_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    if (k->key == KEY_ENTER)
        do_delete();
    else if (k->key == KEY_ESC)
        wm_close(w);
}

static void del_close(Window *w)
{
    (void)w;
    DEL_WIN = 0;
}

/* Ask about deleting the selected entries (not the drives among them): "Delete "name"?" for one, "Delete 5 items?" for several. */
void ex_ask_delete(void)
{
    if (!EX_WIN || DEL_WIN)
        return;
    if (EX_RENAME_ID >= 0 && !ex_end_rename(1)) /* finishing a rename may re-sort the list: the selection is by node, so it holds */
        return;
    EX_MENU.open = 0;
    DEL_N = ex_selected(DEL_IDS, EX_SEL_MAX);
    if (DEL_N == 0)
    {
        if (ex_sel_count() > 0)
            EX_NOTE = "A drive cannot be deleted.";
        return;
    }
    if (!ex_check_writable())
        return;
    const VfsNode *n = vfs_node(DEL_IDS[0]);
    if (!n)
        return;
    int o;
    if (DEL_N == 1)
    {
        o = ex_put_str(DEL_TITLE, 0, sizeof DEL_TITLE, n->is_dir ? "Delete folder \"" : "Delete \"");
        o = ex_put_str(DEL_TITLE, o, sizeof DEL_TITLE, n->name);
        o = ex_put_str(DEL_TITLE, o, sizeof DEL_TITLE, "\"?");
        DEL_TITLE[o] = 0;
        if (n->is_dir)
        {
            int inside = vfs_subtree_count(DEL_IDS[0]);
            o = ex_put_str(DEL_LINE, 0, sizeof DEL_LINE, "The folder and ");
            o = ex_put_uint(DEL_LINE, o, inside);
            o = ex_put_str(DEL_LINE, o, sizeof DEL_LINE, inside == 1 ? " item inside it" : " items inside it");
            o = ex_put_str(DEL_LINE, o, sizeof DEL_LINE, " will be removed.");
        }
        else
            o = ex_put_str(DEL_LINE, 0, sizeof DEL_LINE, "The file will be removed.");
    }
    else
    {
        o = ex_put_str(DEL_TITLE, 0, sizeof DEL_TITLE, "Delete ");
        o = ex_put_uint(DEL_TITLE, o, DEL_N);
        o = ex_put_str(DEL_TITLE, o, sizeof DEL_TITLE, " items?");
        DEL_TITLE[o] = 0;
        int files = 0, folders = 0;
        for (int i = 0; i < DEL_N; i++)
        {
            const VfsNode *v = vfs_node(DEL_IDS[i]);
            if (v && v->is_dir)
                folders++;
            else
                files++;
        }
        o = 0;
        if (files > 0)
        {
            o = ex_put_uint(DEL_LINE, o, files);
            o = ex_put_str(DEL_LINE, o, sizeof DEL_LINE, files == 1 ? " file" : " files");
        }
        if (files > 0 && folders > 0)
            o = ex_put_str(DEL_LINE, o, sizeof DEL_LINE, " and ");
        if (folders > 0)
        {
            o = ex_put_uint(DEL_LINE, o, folders);
            o = ex_put_str(DEL_LINE, o, sizeof DEL_LINE, folders == 1 ? " folder (with all that is in it)" : " folders (with all that is in them)");
        }
        o = ex_put_str(DEL_LINE, o, sizeof DEL_LINE, " will be removed.");
    }
    DEL_LINE[o] = 0;

    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 420, h = 200;
    int x = EX_WIN->r.x + (EX_WIN->r.w - w) / 2, y = EX_WIN->r.y + (EX_WIN->r.h - h) / 2;
    if (x + w > W)
        x = W - w;
    if (y + h > H - TASKBAR_HEIGHT)
        y = H - TASKBAR_HEIGHT - h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    Window *win = wm_create_dialog(EX_WIN, "Delete", x, y, w, h, del_draw, 0);
    if (!win)
    {
        EX_NOTE = "Window limit reached - close one first.";
        return;
    }
    DEL_WIN = win;
    win->on_key = del_key;
    win->on_close = del_close;
    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    int anchor = WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM;
    wm_add_button_ex(win, "Cancel", UI_BTN_SECONDARY, ui_rect(cw - 20 - 100 - 10 - 100, ch - 20 - 34, 100, 34), anchor,
                     on_del_no, win);
    wm_add_button_ex(win, "Delete", UI_BTN_DANGER, ui_rect(cw - 20 - 100, ch - 20 - 34, 100, 34), anchor, on_del_yes, 0);
}
