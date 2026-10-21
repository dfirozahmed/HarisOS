/* File Explorer: drives - the write lock, information about the drive the current folder is on, and the
 * messages for errors the VFS reports. */
#include "explorer.h"

int ex_is_drive(const VfsNode *n) { return n && n->is_dir && n->parent == VFS_ROOT; }

int ex_cur_drive(VfsVolInfo *vi)
{
    int v = vfs_volume_of(EX_CUR);
    if (EX_CUR == VFS_ROOT || v < 0)
        return 0;
    return vfs_volume_info(v, vi) == 0;
}

/* Can the current folder be changed? If not, say why in the status bar (at This PC nothing can be created, on a
 * locked drive nothing can be written). */
int ex_check_writable(void)
{
    if (EX_CUR == VFS_ROOT)
    {
        EX_NOTE = "Open a drive first: nothing can be created in This PC.";
        return 0;
    }
    VfsVolInfo vi;
    if (ex_cur_drive(&vi) && !vi.writable)
    {
        EX_NOTE = vfs_error_text(VFS_ERR_NOWRITE); /* a disk without a write driver: read only for good */
        return 0;
    }
    if (ex_cur_drive(&vi) && vi.lockable && vi.locked)
    {
        EX_NOTE = vfs_error_text(VFS_ERR_RO);
        return 0;
    }
    return 1;
}

/* show why the last VFS call failed in the status bar */
void ex_note_error(void) { EX_NOTE = vfs_error_text(vfs_last_error()); }

int ex_lock_rect(const Layout *l, UiRect *r)
{
    VfsVolInfo vi;
    if (!ex_cur_drive(&vi) || !EX_VOL_SHOWS_LOCK(&vi) || l->status.w < 300)
        return 0;
    int w = 108;
    *r = ui_rect(l->status.x + l->status.w - 12 - w, l->status.y + 3, w, l->status.h - 6);
    return 1;
}

/* ---- the confirmation before a drive is unlocked ---------------- */
static Window *LOCK_WIN;
static int LOCK_VOL;
static char LOCK_TITLE[VFS_NAME_MAX + 32];

int ex_lock_dialog_pending(void) { return LOCK_WIN != 0; }

static void unlock_now(void)
{
    int vol = LOCK_VOL;
    if (LOCK_WIN)
        wm_close(LOCK_WIN);
    vfs_set_locked(vol, 0);
    EX_NOTE = 0;
}

static void on_unlock(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    unlock_now();
}

static void on_cancel(UiButton *b, void *user)
{
    (void)b;
    wm_close((Window *)user);
}

static void lock_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    body = ui_inset(body, t->pad);
    ui_text_in(ui_rect(body.x, body.y, body.w, 26), LOCK_TITLE, UI_TEXT_TITLE, UI_ALIGN_LEFT);
    ui_text_in_color(ui_rect(body.x, body.y + 34, body.w, 22), "Files and folders on it can then be created,", UI_TEXT_BODY,
                     UI_ALIGN_LEFT, t->text);
    ui_text_in_color(ui_rect(body.x, body.y + 54, body.w, 22), "renamed, copied, moved and deleted.", UI_TEXT_BODY,
                     UI_ALIGN_LEFT, t->text);
    ui_text_in_color(ui_rect(body.x, body.y + 82, body.w, 20), "Writing to disks is new in HarisOS: keep a backup.",
                     UI_TEXT_SMALL, UI_ALIGN_LEFT, t->warning);
}

static void lock_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    if (k->key == KEY_ENTER)
        unlock_now();
    else if (k->key == KEY_ESC)
        wm_close(w);
}

static void lock_close(Window *w)
{
    (void)w;
    LOCK_WIN = 0;
}

/* Lock the current drive at once; unlocking asks first. */
void ex_toggle_lock(void)
{
    VfsVolInfo vi;
    if (!EX_WIN || LOCK_WIN || !ex_cur_drive(&vi))
        return;
    if (!vi.lockable) /* a read-only file system: the lock cannot be opened, say why */
    {
        if (!vi.writable)
            EX_NOTE = vfs_error_text(VFS_ERR_NOWRITE);
        return;
    }
    if (EX_RENAME_ID >= 0 && !ex_end_rename(1))
        return;
    LOCK_VOL = vfs_volume_of(EX_CUR);
    EX_MENU.open = 0;
    if (!vi.locked)
    {
        vfs_set_locked(LOCK_VOL, 1);
        EX_NOTE = 0;
        return;
    }
    int o = ex_put_str(LOCK_TITLE, 0, sizeof LOCK_TITLE, "Allow changes to \"");
    o = ex_put_str(LOCK_TITLE, o, sizeof LOCK_TITLE, vi.name);
    o = ex_put_str(LOCK_TITLE, o, sizeof LOCK_TITLE, "\"?");
    LOCK_TITLE[o] = 0;

    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 480, h = 220;
    int x = EX_WIN->r.x + (EX_WIN->r.w - w) / 2, y = EX_WIN->r.y + (EX_WIN->r.h - h) / 2;
    if (x + w > W)
        x = W - w;
    if (y + h > H - TASKBAR_HEIGHT)
        y = H - TASKBAR_HEIGHT - h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    Window *win = wm_create_dialog(EX_WIN, "Unlock drive", x, y, w, h, lock_draw, 0);
    if (!win)
    {
        EX_NOTE = "Window limit reached - close one first.";
        return;
    }
    LOCK_WIN = win;
    win->on_key = lock_key;
    win->on_close = lock_close;
    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    int anchor = WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM;
    wm_add_button_ex(win, "Cancel", UI_BTN_SECONDARY, ui_rect(cw - 20 - 110 - 10 - 100, ch - 20 - 34, 100, 34), anchor,
                     on_cancel, win);
    wm_add_button_ex(win, "Unlock", UI_BTN_DANGER, ui_rect(cw - 20 - 110, ch - 20 - 34, 110, 34), anchor, on_unlock, 0);
}
