/* HarisOS desktop files: see include/deskfiles.h. */
#include "deskfiles.h"
#include "console.h"
#include "deskicons.h"
#include "desktop.h"
#include "exec.h"
#include "fileexplorer.h"
#include "look.h"
#include "notepad.h"
#include "taskbar.h"
#include "timer.h"
#include "toast.h"
#include "vfs.h"
#include "wm.h"

#define FOLDER_NAME "Desktop"
#define TICK_MS 1000

static int FOLDER = -1;       /* the Desktop folder at the last look */
static unsigned SIG;          /* what was in it, as a number: names, kinds, nodes */
static int KNOWN;             /* there has been a look */
static unsigned long long NEXT_MS;
static DeskFile FILES[DESK_FILES_MAX];
static int NFILES;

/* ---- small string helpers (no libc) ---- */
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int same_ci(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b))
        a++, b++;
    return !*a && !*b;
}

static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

/* does the name end in ".ext" (ignoring case)? */
static int ext_is(const char *name, const char *ext)
{
    int n = slen(name), e = slen(ext);
    if (n <= e + 1 || name[n - e - 1] != '.')
        return 0;
    return same_ci(name + n - e, ext);
}

static char *put(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;
    return o;
}

/* ---- the folder ---- */
int deskfiles_folder(void)
{
    int dir = look_data_folder();
    if (dir < 0)
        return -1;
    int list[256];
    int k = vfs_list(dir, list, 256);
    if (k > 256)
        k = 256;
    for (int i = 0; i < k; i++)
    {
        const VfsNode *n = vfs_node(list[i]);
        if (n && n->is_dir && same_ci(n->name, FOLDER_NAME))
            return list[i];
    }
    return -1;
}

/* look at the folder: what is in it now, and a number that changes when that does */
static unsigned look(int folder, DeskFile *out, int *count)
{
    int n = 0;
    unsigned h = 5381u + (unsigned)folder;
    if (folder >= 0)
    {
        int ids[DESK_FILES_MAX];
        int total = vfs_list(folder, ids, DESK_FILES_MAX); /* folders first, then by name */
        for (int i = 0; i < total && i < DESK_FILES_MAX; i++)
        {
            const VfsNode *v = vfs_node(ids[i]);
            if (!v)
                continue;
            DeskFile *f = &out[n++];
            f->node = ids[i];
            f->is_dir = v->is_dir;
            int k = 0;
            for (; v->name[k] && k < DESK_FILE_NAME - 1; k++)
                f->name[k] = v->name[k];
            f->name[k] = 0;
            h = h * 33u + (unsigned)ids[i] + (unsigned)v->is_dir;
            for (k = 0; f->name[k]; k++)
                h = h * 33u + (unsigned char)f->name[k];
        }
    }
    *count = n;
    return h;
}

/* look again; when it is different (or `force`) the icons are told. 1 = told. */
static int rescan(int from_disk, int force)
{
    int folder = deskfiles_folder();
    if (folder >= 0 && from_disk)
        vfs_refresh(folder);
    static DeskFile now[DESK_FILES_MAX]; /* too big for the stack */
    int n;
    unsigned sig = look(folder, now, &n);
    if (!force && KNOWN && folder == FOLDER && sig == SIG)
        return 0;
    KNOWN = 1;
    FOLDER = folder;
    SIG = sig;
    NFILES = n;
    for (int i = 0; i < n; i++)
        FILES[i] = now[i];
    deskicons_set_files(FILES, NFILES, folder >= 0);
    return 1;
}

void deskfiles_scan(int from_disk)
{
    rescan(from_disk, 1);
}

int deskfiles_tick(void)
{
    unsigned long long now = timer_ms();
    if (now < NEXT_MS)
        return 0;
    NEXT_MS = now + TICK_MS;
    return rescan(0, 0);
}

/* ---- opening ---- */
static void run_program(int node, const VfsNode *n)
{
    int con = console_window_open(n->name);
    if (con < 0)
    {
        toast_show(TOAST_ERROR, "Cannot run: no free console or window (close one first).");
        return;
    }
    int err = 0;
    int id = exec_run_node(node, n->name, con, &err);
    if (id >= 0)
        return;
    char msg[TOAST_TEXT_MAX + 1], *o = msg;
    o = put(o, "Cannot run: ");
    const char *why = exec_error_text(id, err);
    for (int i = 0; why[i] && o - msg < (int)sizeof msg - 3; i++)
        *o++ = why[i];
    *o++ = '.';
    *o = 0;
    console_write(con, msg, (unsigned)(o - msg)); /* the window stays, so the reason is not lost */
    console_write(con, "\n", 1);
    console_finish(con, -1);
    toast_show(TOAST_ERROR, msg);
}

void deskfiles_open(int node)
{
    const VfsNode *n = vfs_node(node);
    if (!n)
    {
        toast_show(TOAST_WARNING, "That item is not there any more.");
        rescan(0, 1);
        return;
    }
    if (n->is_dir)
    {
        fileexplorer_open_folder(node);
        return;
    }
    static const char *const TEXT[] = {"txt", "md", "ini", "log", "c", "h", "asm", 0};
    for (int i = 0; TEXT[i]; i++)
        if (ext_is(n->name, TEXT[i]))
        {
            char path[256];
            vfs_path(node, path, (int)sizeof path);
            const char *why = notepad_open_file(path); /* NULL: opened (or the question about unsaved text was asked) */
            if (why)
                toast_show(TOAST_WARNING, why);
            return;
        }
    if (ext_is(n->name, "elf"))
    {
        run_program(node, n);
        return;
    }
    toast_show(TOAST_INFO, "No app can open this file yet.");
}

/* ---- sending ---- */
/* the Desktop folder, made when there is none */
static int folder_or_make(void)
{
    int dest = deskfiles_folder();
    if (dest >= 0)
        return dest;
    int parent = look_data_folder();
    if (parent < 0)
    {
        toast_show(TOAST_WARNING, "There is no drive to make a Desktop folder on.");
        return -1;
    }
    dest = vfs_add(parent, FOLDER_NAME, 1, 0);
    if (dest < 0) /* a locked drive, a file called Desktop ... */
    {
        char msg[TOAST_TEXT_MAX + 1], *o = msg;
        o = put(o, "Cannot make the Desktop folder: ");
        const char *why = vfs_error_text(vfs_last_error());
        for (int i = 0; why[i] && o - msg < (int)sizeof msg - 1; i++)
            *o++ = why[i];
        *o = 0;
        toast_show(TOAST_ERROR, msg);
        return -1;
    }
    return dest;
}

int deskfiles_send(int node)
{
    const VfsNode *n = vfs_node(node);
    if (!n)
    {
        toast_show(TOAST_WARNING, "That item is not there any more.");
        return -1;
    }
    if (node == VFS_ROOT || vfs_is_volume_root(node))
    {
        toast_show(TOAST_WARNING, "A drive cannot be sent to the desktop.");
        return -1;
    }
    char name[DESK_FILE_NAME];
    int k = 0;
    for (; n->name[k] && k < DESK_FILE_NAME - 1; k++)
        name[k] = n->name[k];
    name[k] = 0;
    char msg[TOAST_TEXT_MAX + 1], *o = msg;
    int existing = deskfiles_folder();
    if (existing >= 0 && vfs_is_under(node, existing)) /* it is in the Desktop folder already (or is it) */
    {
        o = put(o, name);
        o = put(o, " is on the desktop already.");
        *o = 0;
        toast_show(TOAST_INFO, msg);
        return -1;
    }
    int dest = folder_or_make();
    if (dest < 0)
        return -1;
    int copy = vfs_copy(node, dest);
    if (copy < 0)
    {
        o = put(o, "Cannot send ");
        o = put(o, name);
        o = put(o, ": ");
        const char *why = vfs_error_text(vfs_last_error());
        for (int i = 0; why[i] && o - msg < (int)sizeof msg - 1; i++)
            *o++ = why[i];
        *o = 0;
        toast_show(TOAST_ERROR, msg);
        rescan(0, 1); /* the folder may have been made even so */
        return -1;
    }
    o = put(o, "Sent ");
    o = put(o, name);
    o = put(o, " to the desktop.");
    *o = 0;
    toast_show(TOAST_SUCCESS, msg);
    rescan(0, 1);
    return copy;
}

static char *put_uint(char *o, int v)
{
    char t[12];
    int k = 0;
    if (v < 0)
        v = 0;
    do
        t[k++] = (char)('0' + v % 10);
    while ((v /= 10) > 0);
    while (k)
        *o++ = t[--k];
    return o;
}

int deskfiles_send_many(const int *nodes, int n)
{
    if (n <= 0)
        return 0;
    if (n == 1)
        return deskfiles_send(nodes[0]) >= 0;
    int existing = deskfiles_folder();
    int todo = 0, already = 0;
    for (int i = 0; i < n; i++) /* what is there to copy? (nothing: the Desktop folder is not made for it) */
    {
        const VfsNode *v = vfs_node(nodes[i]);
        if (!v || nodes[i] == VFS_ROOT || vfs_is_volume_root(nodes[i]))
            continue;
        if (existing >= 0 && vfs_is_under(nodes[i], existing))
            already++;
        else
            todo++;
    }
    char msg[TOAST_TEXT_MAX + 1], *o = msg;
    if (todo == 0)
    {
        if (already > 0)
            toast_show(TOAST_INFO, "They are on the desktop already.");
        else
            toast_show(TOAST_WARNING, "Nothing to send: a drive cannot be sent to the desktop.");
        return 0;
    }
    int dest = folder_or_make();
    if (dest < 0)
        return 0; /* the toast says why */
    int ok = 0, failed = 0, first_err = 0;
    for (int i = 0; i < n; i++)
    {
        const VfsNode *v = vfs_node(nodes[i]);
        if (!v || nodes[i] == VFS_ROOT || vfs_is_volume_root(nodes[i]) || (existing >= 0 && vfs_is_under(nodes[i], existing)))
            continue;
        if (vfs_copy(nodes[i], dest) < 0)
        {
            if (!failed)
                first_err = vfs_last_error();
            failed++;
        }
        else
            ok++;
    }
    if (failed == 0)
    {
        o = put(o, "Sent ");
        o = put_uint(o, ok);
        o = put(o, ok == 1 ? " item to the desktop" : " items to the desktop");
        if (already > 0)
        {
            o = put(o, " (");
            o = put_uint(o, already);
            o = put(o, already == 1 ? " was" : " were");
            o = put(o, " there already)");
        }
        o = put(o, ".");
        *o = 0;
        toast_show(TOAST_SUCCESS, msg);
    }
    else
    {
        if (ok == 0)
        {
            o = put(o, "Cannot send ");
            o = put_uint(o, failed);
            o = put(o, failed == 1 ? " item: " : " items: ");
        }
        else
        {
            o = put(o, "Sent ");
            o = put_uint(o, ok);
            o = put(o, " of ");
            o = put_uint(o, ok + failed);
            o = put(o, " items; the rest: ");
        }
        const char *why = vfs_error_text(first_err);
        for (int i = 0; why[i] && o - msg < (int)sizeof msg - 1; i++)
            *o++ = why[i];
        *o = 0;
        toast_show(ok == 0 ? TOAST_ERROR : TOAST_WARNING, msg);
    }
    rescan(0, 1); /* some copies may have been made even when others failed */
    return ok;
}

/* ---- deleting ---- */
void deskfiles_delete(int node)
{
    const VfsNode *n = vfs_node(node);
    if (!n)
    {
        rescan(0, 1);
        return;
    }
    char name[DESK_FILE_NAME];
    int k = 0;
    for (; n->name[k] && k < DESK_FILE_NAME - 1; k++)
        name[k] = n->name[k];
    name[k] = 0;
    int r = vfs_remove(node);
    if (r < 0)
        toast_show(TOAST_ERROR, vfs_error_text(vfs_last_error())); /* the drive is locked, ... */
    else
    {
        char msg[TOAST_TEXT_MAX + 1], *o = msg;
        o = put(o, "Deleted ");
        o = put(o, name);
        *o = 0;
        toast_show(TOAST_SUCCESS, msg);
    }
    rescan(0, 1); /* some of it may be gone even when it failed */
}

/* The question: a small window with Delete and Cancel. One at a time: asking again replaces the question. */
static Window *CONFIRM;
static int ASK_NODE;
static char ASK_NAME[DESK_FILE_NAME];
static int ASK_DIR;

static void confirm_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    char q[DESK_FILE_NAME + 16], *o = q;
    o = put(o, "Delete \"");
    o = put(o, ASK_NAME);
    o = put(o, "\"?");
    *o = 0;
    ui_text_in(ui_rect(body.x + 22, body.y + 20, body.w - 44, 26), q, UI_TEXT_TITLE, UI_ALIGN_LEFT);
    ui_text_in_color(ui_rect(body.x + 22, body.y + 52, body.w - 44, 22),
                     ASK_DIR ? "The folder and everything in it will be deleted." : "The file will be deleted.", UI_TEXT_BODY,
                     UI_ALIGN_LEFT, t->text);
    ui_text_in_color(ui_rect(body.x + 22, body.y + 76, body.w - 44, 22), "This cannot be undone: there is no Recycle Bin.", UI_TEXT_SMALL,
                     UI_ALIGN_LEFT, t->text_dim);
}

static void confirm_closed(Window *w)
{
    (void)w;
    CONFIRM = 0;
}

static void confirm_yes(UiButton *b, void *user)
{
    (void)b;
    Window *w = (Window *)user;
    int node = ASK_NODE;
    wm_close(w);
    deskfiles_delete(node);
}

static void confirm_no(UiButton *b, void *user)
{
    (void)b;
    wm_close((Window *)user);
}

static void confirm_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    if (k->key == KEY_ESC)
        wm_close(w);
    else if (k->key == KEY_ENTER)
    {
        int node = ASK_NODE;
        wm_close(w);
        deskfiles_delete(node);
    }
}

void deskfiles_ask_delete(int node)
{
    const VfsNode *n = vfs_node(node);
    if (!n)
    {
        rescan(0, 1);
        return;
    }
    if (CONFIRM)
        wm_close(CONFIRM); /* the old question goes; this one replaces it */
    ASK_NODE = node;
    ASK_DIR = n->is_dir;
    int k = 0;
    for (; n->name[k] && k < DESK_FILE_NAME - 1; k++)
        ASK_NAME[k] = n->name[k];
    ASK_NAME[k] = 0;

    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 460, h = 220;
    int x = (W - w) / 2, y = (H - TASKBAR_HEIGHT - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    Window *win = wm_create("Delete", x, y, w, h, confirm_draw, 0);
    if (!win)
    {
        toast_show(TOAST_ERROR, "Cannot ask: no free window (close one first).");
        return;
    }
    wm_set_resizable(win, 0);
    win->on_close = confirm_closed;
    win->on_key = confirm_key;
    CONFIRM = win;
    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    int anchor = WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM;
    wm_add_button_ex(win, "Cancel", UI_BTN_SECONDARY, ui_rect(cw - 20 - 100 - 10 - 100, ch - 20 - 34, 100, 34), anchor, confirm_no, win);
    wm_add_button_ex(win, "Delete", UI_BTN_DANGER, ui_rect(cw - 20 - 100, ch - 20 - 34, 100, 34), anchor, confirm_yes, win);
}
