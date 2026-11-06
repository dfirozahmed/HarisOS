/* File picker: a modal dialog to choose one file (see include/filepicker.h).
 *
 * It lists the children of one folder of the virtual file system (vfs.h): at the top that is the drives. A click selects a row, a double click
 * (or Enter) opens a folder or chooses a file, Up (or Backspace) goes to the parent folder, Select chooses the selected file. A folder on a drive is
 * read from the drive when it is opened for the first time (vfs_refresh); the listing itself is vfs_list_by, folders first, then by name. */
#include "filepicker.h"
#include "taskbar.h"
#include "timer.h"
#include "ui.h"
#include "vfs.h"

#define ROW_H 30
#define HEAD_H 44
#define FOOT_H 54
#define ITEMS_MAX 256
#define DOUBLE_MS 450
#define BTN_UP 0
#define BTN_CANCEL 1
#define BTN_SELECT 2

static Window *FP;
static void (*ON_PICK)(const char *path, void *user);
static void *USER;
static int DIR = -1;             /* the folder shown (a vfs node) */
static int ITEMS[ITEMS_MAX];
static int N;
static int SEL = -1;
static int SCROLL;
static UiScroll SB;
static int LAST_ROW = -1;
static unsigned long long LAST_CLICK;

static void put_str(char *s, int cap, int *n, const char *t)
{
    while (*t && *n < cap - 1)
        s[(*n)++] = *t++;
    s[*n] = 0;
}

static void put_dec(char *s, int cap, int *n, unsigned v)
{
    char tmp[10];
    int k = 0;
    do
    {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k && *n < cap - 1)
        s[(*n)++] = tmp[--k];
    s[*n] = 0;
}

/* "812 B", "1.2 KB", "3.4 MB" */
static void size_text(unsigned bytes, char *out, int cap)
{
    int n = 0;
    out[0] = 0;
    if (bytes < 1024)
    {
        put_dec(out, cap, &n, bytes);
        put_str(out, cap, &n, " B");
        return;
    }
    unsigned long long unit = bytes < 1024u * 1024u ? 1024 : 1024ull * 1024;
    unsigned long long tenths = (unsigned long long)bytes * 10 / unit;
    put_dec(out, cap, &n, (unsigned)(tenths / 10));
    put_str(out, cap, &n, ".");
    put_dec(out, cap, &n, (unsigned)(tenths % 10));
    put_str(out, cap, &n, unit == 1024 ? " KB" : " MB");
}

static void load(int dir)
{
    const VfsNode *d = vfs_node(dir);
    if (!d || !d->is_dir)
        return;
    if (!d->loaded)
        vfs_refresh(dir);
    DIR = dir;
    int total = vfs_list_by(dir, ITEMS, ITEMS_MAX, VFS_SORT_NAME, 0);
    N = total < ITEMS_MAX ? total : ITEMS_MAX;
    SEL = -1;
    SCROLL = 0;
    LAST_ROW = -1;
}

static int is_file_row(int row)
{
    if (row < 0 || row >= N)
        return 0;
    const VfsNode *n = vfs_node(ITEMS[row]);
    return n && !n->is_dir;
}

static void update_buttons(void)
{
    if (!FP)
        return;
    const VfsNode *d = vfs_node(DIR);
    FP->btn[BTN_UP].enabled = d && d->parent >= 0;
    FP->btn[BTN_SELECT].enabled = is_file_row(SEL);
}

typedef struct
{
    UiRect head, list;
} Layout;

static Layout layout(UiRect body)
{
    Layout l;
    UiRect r = ui_inset(body, 8);
    l.head = ui_cut_top(&r, HEAD_H);
    ui_cut_bottom(&r, FOOT_H);
    ui_cut_right(&r, UI_SCROLL_W);
    l.list = r;
    return l;
}

static int visible_rows(const Layout *l)
{
    int n = l->list.h / ROW_H;
    return n < 1 ? 1 : n;
}

static void sb_sync(const Layout *l)
{
    SB.total = N;
    SB.page = visible_rows(l);
    SB.pos = SCROLL;
    SB.track = ui_rect(l->list.x + l->list.w, l->list.y, UI_SCROLL_W, l->list.h);
}

static void keep_visible(const Layout *l)
{
    int vis = visible_rows(l);
    if (SEL < SCROLL)
        SCROLL = SEL;
    if (SEL >= SCROLL + vis)
        SCROLL = SEL - vis + 1;
    if (SCROLL < 0)
        SCROLL = 0;
}

static void select_row(int row)
{
    if (row < 0 || row >= N)
        return;
    SEL = row;
    Layout l = layout(wm_content(FP));
    keep_visible(&l);
    update_buttons();
}

static void go_up(void)
{
    const VfsNode *d = vfs_node(DIR);
    if (!d || d->parent < 0)
        return;
    int from = DIR;
    load(d->parent);
    for (int i = 0; i < N; i++) /* the folder we came out of is selected */
        if (ITEMS[i] == from)
            SEL = i;
    if (SEL >= 0)
    {
        Layout l = layout(wm_content(FP));
        keep_visible(&l);
    }
    update_buttons();
    wm_invalidate();
}

static void choose(int row)
{
    char path[300];
    if (!is_file_row(row))
        return;
    vfs_path(ITEMS[row], path, sizeof path);
    void (*cb)(const char *, void *) = ON_PICK;
    void *u = USER;
    wm_close(FP);
    if (cb)
        cb(path, u);
}

/* a folder is opened, a file is chosen */
static void activate(int row)
{
    if (row < 0 || row >= N)
        return;
    const VfsNode *n = vfs_node(ITEMS[row]);
    if (!n)
        return;
    if (n->is_dir)
    {
        load(ITEMS[row]);
        update_buttons();
        wm_invalidate();
    }
    else
        choose(row);
}

static void fp_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    Layout l = layout(body);
    char path[300];
    vfs_path(DIR, path, sizeof path);
    ui_text_in_color(ui_rect(l.head.x, l.head.y + 2, l.head.w - 84, 26), path[1] ? path : "This computer", UI_TEXT_TITLE, UI_ALIGN_LEFT, t->text);
    ui_separator(l.head.x, l.head.y + l.head.h - 6, l.head.w);
    int vis = visible_rows(&l);
    sb_sync(&l);
    ui_scroll_draw(SB.track, &SB);
    if (N == 0)
        ui_text_in(l.list, "This folder is empty.", UI_TEXT_DIM, UI_ALIGN_CENTER);
    for (int i = 0; i < vis && SCROLL + i < N; i++)
    {
        int ri = SCROLL + i;
        const VfsNode *n = vfs_node(ITEMS[ri]);
        if (!n)
            continue;
        UiRect r = ui_rect(l.list.x, l.list.y + i * ROW_H, l.list.w, ROW_H);
        if (ri == SEL)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h - 2, t->radius_s, ui_color(t->accent), 40);
        UiIcon ic = vfs_is_volume_root(ITEMS[ri]) ? UI_ICON_DISK : n->is_dir ? UI_ICON_FILES : UI_ICON_NOTEPAD;
        ui_icon(ic, r.x + 18, r.y + r.h / 2 - 1, 20, n->is_dir ? t->accent : t->text_dim);
        int size_w = n->is_dir ? 0 : 90;
        ui_text_in_color(ui_rect(r.x + 36, r.y, r.w - 36 - size_w - 6, r.h - 2), n->name, UI_TEXT_BODY, UI_ALIGN_LEFT, t->text);
        if (!n->is_dir)
        {
            char sz[24];
            size_text(n->size, sz, sizeof sz);
            ui_text_in_color(ui_rect(r.x + r.w - size_w - 6, r.y, size_w, r.h - 2), sz, UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
        }
    }
}

static int fp_press(Window *w, int x, int y)
{
    Layout l = layout(wm_content(w));
    sb_sync(&l);
    if (ui_scroll_press(&SB, x, y))
    {
        SCROLL = SB.pos;
        return 1;
    }
    if (!ui_contains(l.list, x, y))
        return 0;
    int row = SCROLL + (y - l.list.y) / ROW_H;
    if ((y - l.list.y) / ROW_H >= visible_rows(&l) || row >= N)
        return 0;
    unsigned long long now = timer_ms();
    int twice = row == LAST_ROW && now - LAST_CLICK < DOUBLE_MS;
    select_row(row);
    if (twice)
    {
        LAST_ROW = -1;
        activate(row);
    }
    else
    {
        LAST_ROW = row;
        LAST_CLICK = now;
    }
    return 1;
}

static int fp_drag(Window *w, int x, int y, unsigned buttons)
{
    if (!SB.dragging)
        return 0;
    Layout l = layout(wm_content(w));
    sb_sync(&l);
    int changed = ui_scroll_drag(&SB, x, y, buttons);
    SCROLL = SB.pos;
    return changed;
}

static int fp_hover(Window *w, int x, int y)
{
    Layout l = layout(wm_content(w));
    sb_sync(&l);
    return ui_scroll_hover(&SB, x, y);
}

static int fp_wheel(Window *w, int steps)
{
    Layout l = layout(wm_content(w));
    int max = N - visible_rows(&l);
    if (max < 0)
        max = 0;
    int old = SCROLL;
    SCROLL += steps * 2;
    if (SCROLL > max)
        SCROLL = max;
    if (SCROLL < 0)
        SCROLL = 0;
    return SCROLL != old;
}

static void fp_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    Layout l = layout(wm_content(w));
    int page = visible_rows(&l);
    switch (k->key)
    {
    case KEY_ESC:
        wm_close(w);
        return;
    case KEY_UP:
        select_row(SEL < 0 ? N - 1 : SEL - 1);
        break;
    case KEY_DOWN:
        select_row(SEL + 1);
        break;
    case KEY_PAGEUP:
        select_row(SEL < 0 ? 0 : (SEL - page < 0 ? 0 : SEL - page));
        break;
    case KEY_PAGEDOWN:
        select_row(SEL < 0 ? 0 : (SEL + page >= N ? N - 1 : SEL + page));
        break;
    case KEY_HOME:
        select_row(0);
        break;
    case KEY_ENTER:
        activate(SEL);
        return;
    case KEY_BACKSPACE:
        go_up();
        return;
    default:
        return;
    }
    wm_invalidate();
}

static void fp_close(Window *w)
{
    (void)w;
    FP = 0;
}

static void on_up(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    go_up();
}

static void on_cancel(UiButton *b, void *user)
{
    (void)b;
    wm_close((Window *)user);
}

static void on_select(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    choose(SEL);
}

void filepicker_open(Window *owner, const char *title, const char *start_path, void (*on_pick)(const char *path, void *user), void *user)
{
    if (FP)
    {
        wm_activate(FP);
        return;
    }
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 480, h = 440;
    if (w > W - 20)
        w = W - 20;
    if (h > H - TASKBAR_HEIGHT - 8)
        h = H - TASKBAR_HEIGHT - 8;
    int x = owner ? owner->r.x + (owner->r.w - w) / 2 : (W - w) / 2;
    int y = owner ? owner->r.y + (owner->r.h - h) / 2 : (H - TASKBAR_HEIGHT - h) / 2;
    if (x + w > W)
        x = W - w;
    if (y + h > H - TASKBAR_HEIGHT)
        y = H - TASKBAR_HEIGHT - h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    Window *win = wm_create_dialog(owner, title && *title ? title : "Choose a file", x, y, w, h, fp_draw, 0);
    if (!win)
        return;
    FP = win;
    ON_PICK = on_pick;
    USER = user;
    win->on_key = fp_key;
    win->on_press = fp_press;
    win->on_drag = fp_drag;
    win->on_hover = fp_hover;
    win->on_wheel = fp_wheel;
    win->on_close = fp_close;
    SB.dragging = SB.hover = 0;

    /* where to start: the folder of the file given, with the file selected; or the folder given; else the top (the drives) */
    int node = start_path && *start_path ? vfs_lookup(start_path) : -1;
    const VfsNode *sn = node >= 0 ? vfs_node(node) : 0;
    int root = vfs_lookup("/");
    if (sn && sn->is_dir)
        load(node);
    else if (sn && sn->parent >= 0)
    {
        load(sn->parent);
        for (int i = 0; i < N; i++)
            if (ITEMS[i] == node)
                SEL = i;
    }
    else
        load(root);

    int tb = ui_theme()->titlebar_h;
    int cw = w - 2, ch = h - tb - 2;
    wm_add_button_ex(win, "Up", UI_BTN_SECONDARY, ui_rect(cw - 8 - 72, 8 + 4, 72, 32), WM_ANCHOR_RIGHT, on_up, win);
    wm_add_button_ex(win, "Cancel", UI_BTN_SECONDARY, ui_rect(cw - 8 - 100 - 8 - 100, ch - 8 - 38, 100, 36), WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_cancel, win);
    wm_add_button_ex(win, "Select", UI_BTN_PRIMARY, ui_rect(cw - 8 - 100, ch - 8 - 38, 100, 36), WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_select, win);
    if (SEL >= 0)
    {
        Layout l = layout(wm_content(win));
        keep_visible(&l);
    }
    update_buttons();
}
