/* File Explorer app: browses the in-memory volume from kernel/fs/vfs.c.
 *
 * Layout (top to bottom): toolbar (Back, Up, path bar) / sidebar "Places" + file list with a
 * column header / status bar. Click selects, double-click or Enter opens, Backspace goes up,
 * Alt+Left goes back, Up/Down/Home/End/PageUp/PageDown move, typing a letter jumps to a name.
 * An app uses only public headers (ui.h, wm.h, desktop.h, vfs.h) - never desktop_internal.h. */
#include "app.h"
#include "desktop.h"
#include "taskbar.h"
#include "timer.h"
#include "ui.h"
#include "vfs.h"
#include "wm.h"

#define NAV_ROW_H 52 /* Back / Forward / Up + path bar */
#define CMD_ROW_H 44 /* New folder, Cut, Copy, Paste, Rename */
#define TOOLBAR_H (NAV_ROW_H + CMD_ROW_H)
#define STATUS_H 28
#define SIDE_W 164
#define HEAD_H 26
#define ROW_H 32
#define PLACE_H 34
#define ICON_PX 22
#define SIZE_COL_W 92
#define TYPE_COL_W 120
#define WHEEL_ROWS 3
#define DOUBLE_CLICK_MS 450
#define HIST_MAX 16
/* toolbar button ids = the order they are added in explorer_app_open() */
enum
{
    BTN_BACK,
    BTN_FORWARD,
    BTN_UP,
    BTN_REFRESH,
    BTN_NEW_FOLDER,
    BTN_NEW_FILE,
    BTN_CUT,
    BTN_COPY,
    BTN_PASTE,
    BTN_RENAME,
    BTN_DELETE,
    BTN_SORT,
    BTN_COUNT
};
#define NAVBTN 36    /* icon-only Back / Forward / Up buttons */
#define PATH_X (10 + 3 * (NAVBTN + 2) + 6) /* path bar starts after the three navigation buttons */
#define NEWBTN_X 10 /* "New folder" in the command row */
#define NEWBTN_W 118
#define CMD_X0 (NEWBTN_X + NEWBTN_W + 2 + NAVBTN + 16) /* Cut / Copy / Paste / Rename start here, after a divider */
#define CMD_Y (NAV_ROW_H + 5)
#define SORT_X (CMD_X0 + 5 * (NAVBTN + 2) + 14) /* "Sort" after a second divider */
#define SORT_W 82
#define TIP_DELAY_MS 500

enum
{
    FK_FOLDER,
    FK_TEXT,
    FK_CODE,
    FK_IMAGE,
    FK_AUDIO,
    FK_EFI,
    FK_OTHER
};

typedef struct
{
    UiRect toolbar, side, head, list, status;
} Layout;

typedef struct
{
    const char *label;
    const char *path;
} Place;

static const Place PLACES[] = {
    {"This PC", "/"},
    {"Documents", "/Documents"},
    {"Pictures", "/Pictures"},
    {"Music", "/Music"},
    {"System", "/System"},
};
#define PLACE_N ((int)(sizeof PLACES / sizeof PLACES[0]))

static Window *EX_WIN;
static int CUR = VFS_ROOT;       /* folder being shown */
static int ITEMS[VFS_MAX_NODES]; /* its children, sorted */
static int NITEMS;
static int SEL = -1;
static int SCROLL; /* first visible row */
static int HIST[HIST_MAX];
static int HIST_N;
static int FWD[HIST_MAX]; /* folders to go forward to after Back */
static int FWD_N;
static const char *NOTE; /* one-line message for the status bar, cleared by the next action */
static int LAST_ROW = -1;
static unsigned long long LAST_CLICK_MS;
static int HOVER_ROW = -1;   /* list row under the pointer, -1 = none */
static int HOVER_PLACE = -1; /* sidebar entry under the pointer */
static int SORT_KEY = VFS_SORT_NAME; /* VFS_SORT_* */
static int SORT_DESC;
static int TIP_BTN = -1;               /* toolbar button the pointer rests on, -1 = none */
static unsigned long long TIP_SINCE;   /* when it started resting there */
static int TIP_SHOWN;                  /* the tooltip is visible */
static int CLIP_ID = -1;     /* entry on the clipboard (Cut or Copy), -1 = empty */
static int CLIP_CUT;         /* 1 = it moves on Paste, 0 = a copy is made */
static int RENAME_ID = -1;   /* node being renamed in its row (win->edits[0] holds the text), -1 = none */

/* right-click menu: a small panel drawn inside the window */
enum
{
    M_OPEN,
    M_RENAME,
    M_DELETE,
    M_NEW_FOLDER,
    M_NEW_FILE,
    M_REFRESH,
    M_CUT,
    M_COPY,
    M_PASTE,
    M_SORT_NAME,
    M_SORT_SIZE,
    M_SORT_TYPE,
    M_SORT_ASC,
    M_SORT_DESC,
    M_SEP /* a divider line, not selectable */
};
#define MENU_W 224
#define MENU_ROW_H 30
#define MENU_SEP_H 9
#define MENU_PAD 6
#define MENU_MAX 8
typedef struct
{
    int open;
    int kind; /* 0 = folder menu, 1 = entry menu, 2 = sort menu (check marks) */
    int x, y;
    int count;
    int items[MENU_MAX];
    int hover; /* index into items, -1 = none */
} Menu;
static Menu MENU;

/* delete confirmation: a modal dialog of the Explorer window */
static Window *DEL_WIN;
static int DEL_ID;
static char DEL_TITLE[VFS_NAME_MAX + 16], DEL_LINE[64];

/* ---- file kinds ------------------------------------------------ */
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ext_is(const char *name, const char *ext)
{
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.')
            dot = p + 1;
    if (!dot)
        return 0;
    while (*dot && *ext && lower((unsigned char)*dot) == *ext)
        dot++, ext++;
    return !*dot && !*ext;
}

static int kind_of(const VfsNode *n)
{
    static const char *const TEXT[] = {"txt", "md", "ini", "log", 0};
    static const char *const CODE[] = {"c", "h", "asm", 0};
    static const char *const IMAGE[] = {"bmp", "png", "jpg", 0};
    static const char *const AUDIO[] = {"wav", "mp3", 0};
    if (n->is_dir)
        return FK_FOLDER;
    for (int i = 0; TEXT[i]; i++)
        if (ext_is(n->name, TEXT[i]))
            return FK_TEXT;
    for (int i = 0; CODE[i]; i++)
        if (ext_is(n->name, CODE[i]))
            return FK_CODE;
    for (int i = 0; IMAGE[i]; i++)
        if (ext_is(n->name, IMAGE[i]))
            return FK_IMAGE;
    for (int i = 0; AUDIO[i]; i++)
        if (ext_is(n->name, AUDIO[i]))
            return FK_AUDIO;
    return ext_is(n->name, "efi") ? FK_EFI : FK_OTHER;
}

static const char *kind_name(int k)
{
    static const char *const NAMES[] = {"Folder", "Text file", "Source code", "Image", "Audio", "EFI program", "File"};
    return NAMES[k];
}

static UiRgb kind_color(int k)
{
    switch (k)
    {
    case FK_TEXT:
        return ui_rgb(110, 150, 230);
    case FK_CODE:
        return ui_rgb(80, 200, 150);
    case FK_IMAGE:
        return ui_rgb(240, 130, 170);
    case FK_AUDIO:
        return ui_rgb(170, 120, 240);
    case FK_EFI:
        return ui_rgb(250, 170, 60);
    default:
        return ui_rgb(150, 160, 185);
    }
}

/* "812 B", "1.2 KB", "3.0 MB" (one decimal, rounded down) */
static void fmt_size(unsigned bytes, char *out)
{
    unsigned whole, tenth;
    char unit;
    if (bytes < 1024)
    {
        whole = bytes;
        tenth = 0;
        unit = 'B';
    }
    else if (bytes < 1024u * 1024u)
    {
        whole = bytes / 1024;
        tenth = (bytes % 1024) * 10 / 1024;
        unit = 'K';
    }
    else
    {
        whole = bytes / (1024u * 1024u);
        tenth = (bytes % (1024u * 1024u)) * 10 / (1024u * 1024u);
        unit = 'M';
    }
    char digits[12];
    int n = 0, o = 0;
    do
        digits[n++] = (char)('0' + whole % 10), whole /= 10;
    while (whole);
    while (n)
        out[o++] = digits[--n];
    if (unit != 'B')
    {
        out[o++] = '.';
        out[o++] = (char)('0' + tenth);
        out[o++] = ' ';
        out[o++] = unit;
        out[o++] = 'B';
    }
    else
    {
        out[o++] = ' ';
        out[o++] = 'B';
    }
    out[o] = 0;
}

static int put_uint(char *out, int o, int v)
{
    char digits[12];
    int n = 0;
    do
        digits[n++] = (char)('0' + v % 10), v /= 10;
    while (v);
    while (n)
        out[o++] = digits[--n];
    return o;
}

static int put_str(char *out, int o, int cap, const char *s)
{
    while (*s && o < cap - 1)
        out[o++] = *s++;
    return o;
}

/* ---- navigation ------------------------------------------------ */
static int end_rename(int commit);

static void update_nav_buttons(void)
{
    if (!EX_WIN)
        return;
    EX_WIN->btn[BTN_BACK].enabled = HIST_N > 0;
    EX_WIN->btn[BTN_FORWARD].enabled = FWD_N > 0;
    EX_WIN->btn[BTN_UP].enabled = CUR != VFS_ROOT;
}

static void reload(void)
{
    int total = vfs_list_by(CUR, ITEMS, VFS_MAX_NODES, SORT_KEY, SORT_DESC);
    NITEMS = total < VFS_MAX_NODES ? total : VFS_MAX_NODES;
    if (SEL >= NITEMS)
        SEL = NITEMS - 1;
    if (SCROLL > NITEMS - 1)
        SCROLL = NITEMS > 0 ? NITEMS - 1 : 0;
    update_nav_buttons();
}

static void go_to(int dir, int push)
{
    const VfsNode *n = vfs_node(dir);
    if (!n || !n->is_dir || dir == CUR)
        return;
    if (RENAME_ID >= 0 && !end_rename(1))
        end_rename(0); /* leaving the folder: keep the old name if the new one is not usable */
    if (push)
    {
        FWD_N = 0; /* a new path ends the forward list */
        if (HIST_N == HIST_MAX)
        {
            for (int i = 1; i < HIST_MAX; i++)
                HIST[i - 1] = HIST[i];
            HIST_N--;
        }
        HIST[HIST_N++] = CUR;
    }
    CUR = dir;
    SEL = -1;
    SCROLL = 0;
    LAST_ROW = -1;
    NOTE = 0;
    reload();
    if (NITEMS > 0)
        SEL = 0;
}

static void go_back(void)
{
    if (HIST_N == 0)
        return;
    int dir = HIST[--HIST_N];
    if (FWD_N < HIST_MAX)
        FWD[FWD_N++] = CUR; /* so Forward can come back here */
    if (!vfs_node(dir))
    {
        update_nav_buttons();
        return;
    }
    go_to(dir, 0);
}

static void go_up(void)
{
    const VfsNode *n = vfs_node(CUR);
    if (!n || CUR == VFS_ROOT)
        return;
    int from = CUR;
    go_to(n->parent, 1);
    for (int i = 0; i < NITEMS; i++) /* select the folder we just left */
        if (ITEMS[i] == from)
            SEL = i;
}

static void open_item(int row)
{
    if (row < 0 || row >= NITEMS)
        return;
    const VfsNode *n = vfs_node(ITEMS[row]);
    if (!n)
        return;
    if (n->is_dir)
        go_to(ITEMS[row], 1);
    else
        NOTE = "No app can open this file yet.";
}

static void on_back(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    go_back();
}

static void go_forward(void)
{
    if (FWD_N == 0)
        return;
    int dir = FWD[--FWD_N];
    int keep = FWD_N; /* a normal go_to() forgets the forward list; this one must not */
    if (vfs_node(dir))
        go_to(dir, 1);
    FWD_N = keep;
    update_nav_buttons();
}

static void on_forward(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    go_forward();
}

static void on_up(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    go_up();
}

/* ---- layout ---------------------------------------------------- */
static Layout layout(UiRect body)
{
    Layout l;
    UiRect b = body;
    l.toolbar = ui_cut_top(&b, TOOLBAR_H);
    l.status = ui_cut_bottom(&b, STATUS_H);
    l.side = ui_cut_left(&b, SIDE_W);
    l.head = ui_cut_top(&b, HEAD_H);
    l.list = b;
    return l;
}

static int visible_rows(const Layout *l)
{
    int n = l->list.h / ROW_H;
    return n < 1 ? 1 : n;
}

static void clamp_scroll(const Layout *l)
{
    int max_scroll = NITEMS - visible_rows(l);
    if (max_scroll < 0)
        max_scroll = 0;
    if (SCROLL > max_scroll)
        SCROLL = max_scroll;
    if (SCROLL < 0)
        SCROLL = 0;
}

static void select_row(UiRect body, int row)
{
    if (row < 0 || row >= NITEMS)
        return;
    Layout l = layout(body);
    int vis = visible_rows(&l);
    SEL = row;
    NOTE = 0;
    if (row < SCROLL)
        SCROLL = row;
    if (row >= SCROLL + vis)
        SCROLL = row - vis + 1;
    clamp_scroll(&l);
}

/* ---- rename / new / delete ------------------------------------- */
static int same_text(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static int row_of(int id)
{
    for (int i = 0; i < NITEMS; i++)
        if (ITEMS[i] == id)
            return i;
    return -1;
}

static void start_rename(int row)
{
    if (!EX_WIN || row < 0 || row >= NITEMS)
        return;
    const VfsNode *n = vfs_node(ITEMS[row]);
    if (!n)
        return;
    select_row(wm_content(EX_WIN), row);
    MENU.open = 0;
    RENAME_ID = ITEMS[row];
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
static int end_rename(int commit)
{
    if (RENAME_ID < 0 || !EX_WIN)
        return 1;
    UiEdit *e = &EX_WIN->edits[0];
    int id = RENAME_ID;
    const VfsNode *n = vfs_node(id);
    if (commit && n && !same_text(n->name, e->text) && vfs_rename(id, e->text) < 0)
    {
        NOTE = e->len == 0 ? "A name cannot be empty." : "That name is already used or not allowed.";
        return 0;
    }
    RENAME_ID = -1;
    EX_WIN->edit_count = 0;
    EX_WIN->caret = 0;
    e->field.w = 0;
    NOTE = 0;
    reload(); /* the entry may sort to another place */
    int row = row_of(id);
    if (row >= 0)
        select_row(wm_content(EX_WIN), row);
    return 1;
}

/* is there already an entry with this name in the current folder (ignoring case)? */
static int name_taken(const char *name)
{
    for (int i = 0; i < NITEMS; i++)
    {
        const VfsNode *n = vfs_node(ITEMS[i]);
        if (!n)
            continue;
        const char *a = n->name, *b = name;
        while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
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
        int o = put_str(out, 0, VFS_NAME_MAX, base);
        if (n > 1)
        {
            o = put_str(out, o, VFS_NAME_MAX, " (");
            o = put_uint(out, o, n);
            o = put_str(out, o, VFS_NAME_MAX, ")");
        }
        o = put_str(out, o, VFS_NAME_MAX, ext);
        out[o] = 0;
        if (!name_taken(out))
            return;
    }
}

static void new_item(int is_dir)
{
    if (!EX_WIN || (RENAME_ID >= 0 && !end_rename(1)))
        return;
    char name[VFS_NAME_MAX];
    make_name(is_dir, name);
    int id = vfs_add(CUR, name, is_dir, 0);
    if (id < 0)
    {
        NOTE = "The volume is full - delete something first.";
        return;
    }
    reload();
    start_rename(row_of(id)); /* type the real name right away */
}

static void on_new_folder(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    new_item(1);
}

/* Cut (cut = 1) or Copy (cut = 0): remember the selected entry. Nothing changes until Paste. */
static void clip_set(int cut)
{
    if (RENAME_ID >= 0 || SEL < 0 || SEL >= NITEMS)
        return;
    CLIP_ID = ITEMS[SEL];
    CLIP_CUT = cut;
    NOTE = 0;
}

/* Paste into the current folder: Cut moves the entry here, Copy makes a copy (a taken name becomes "name (2)"). */
static void paste(void)
{
    const VfsNode *n = vfs_node(CLIP_ID);
    if (!n)
    {
        CLIP_ID = -1;
        return;
    }
    if (RENAME_ID >= 0 && !end_rename(1))
        return;
    if (CLIP_CUT && n->parent == CUR) /* cut and pasted in the same place: nothing to do */
    {
        CLIP_ID = -1;
        return;
    }
    if (vfs_is_under(CUR, CLIP_ID))
    {
        NOTE = CLIP_CUT ? "A folder cannot be moved into itself." : "A folder cannot be copied into itself.";
        return;
    }
    int id = CLIP_CUT ? CLIP_ID : vfs_copy(CLIP_ID, CUR);
    if (id < 0 || (CLIP_CUT && vfs_move(CLIP_ID, CUR) < 0))
    {
        NOTE = "There is not enough room on the volume.";
        return;
    }
    if (CLIP_CUT)
        CLIP_ID = -1; /* a moved entry leaves the clipboard; a copied one can be pasted again */
    NOTE = 0;
    reload();
    int row = row_of(id);
    if (row >= 0 && EX_WIN)
        select_row(wm_content(EX_WIN), row);
}

/* Change the order of the list; the selected entry stays selected (and in view). */
static void set_sort(int key, int desc)
{
    if (RENAME_ID >= 0 && !end_rename(1))
        return;
    int id = (SEL >= 0 && SEL < NITEMS) ? ITEMS[SEL] : -1;
    SORT_KEY = key;
    SORT_DESC = desc;
    reload();
    int row = row_of(id);
    if (row >= 0 && EX_WIN)
        select_row(wm_content(EX_WIN), row);
    LAST_ROW = -1;
}

static void menu_open(int x, int y, int kind);

static void on_sort(UiButton *b, void *user)
{
    (void)user;
    menu_open(b->r.x, b->r.y + b->r.h + 2, 2); /* drops down under the button */
}

static void ask_delete(int row);

static void on_new_file(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    new_item(0);
}

static void on_delete(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    ask_delete(SEL);
}

static void on_refresh(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    reload();
    NOTE = 0;
}

static void on_cut(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    clip_set(1);
}

static void on_copy(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    clip_set(0);
}

static void on_paste(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    paste();
}

static void on_rename(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    start_rename(SEL);
}

static void do_delete(void)
{
    int id = DEL_ID;
    if (DEL_WIN)
        wm_close(DEL_WIN);
    if (!vfs_node(id))
        return;
    int row = row_of(id), k = 0;
    for (int i = 0; i < HIST_N; i++) /* forget history entries that are about to disappear */
        if (!vfs_is_under(HIST[i], id))
            HIST[k++] = HIST[i];
    HIST_N = k;
    k = 0;
    for (int i = 0; i < FWD_N; i++)
        if (!vfs_is_under(FWD[i], id))
            FWD[k++] = FWD[i];
    FWD_N = k;
    if (CLIP_ID >= 0 && vfs_is_under(CLIP_ID, id))
        CLIP_ID = -1; /* its id may be reused by the next new entry */
    vfs_remove(id);
    LAST_ROW = -1;
    NOTE = 0;
    reload();
    SEL = NITEMS == 0 ? -1 : (row < NITEMS ? row : NITEMS - 1);
    if (EX_WIN && SEL >= 0)
        select_row(wm_content(EX_WIN), SEL);
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

static void ask_delete(int row)
{
    if (!EX_WIN || DEL_WIN || row < 0 || row >= NITEMS)
        return;
    int id = ITEMS[row];
    if (RENAME_ID >= 0 && !end_rename(1)) /* finishing a rename may re-sort the list: keep the id, not the row */
        return;
    DEL_ID = id;
    const VfsNode *n = vfs_node(DEL_ID);
    if (!n)
        return;
    MENU.open = 0;
    int o = put_str(DEL_TITLE, 0, sizeof DEL_TITLE, n->is_dir ? "Delete folder \"" : "Delete \"");
    o = put_str(DEL_TITLE, o, sizeof DEL_TITLE, n->name);
    o = put_str(DEL_TITLE, o, sizeof DEL_TITLE, "\"?");
    DEL_TITLE[o] = 0;
    if (n->is_dir)
    {
        int inside = vfs_subtree_count(DEL_ID);
        o = put_str(DEL_LINE, 0, sizeof DEL_LINE, "The folder and ");
        o = put_uint(DEL_LINE, o, inside);
        o = put_str(DEL_LINE, o, sizeof DEL_LINE, inside == 1 ? " item inside it" : " items inside it");
        o = put_str(DEL_LINE, o, sizeof DEL_LINE, " will be removed.");
    }
    else
        o = put_str(DEL_LINE, 0, sizeof DEL_LINE, "The file will be removed.");
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
        NOTE = "Window limit reached - close one first.";
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

/* ---- right-click menu ------------------------------------------ */
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
    for (int i = 0; i < MENU.count; i++)
        h += MENU.items[i] == M_SEP ? MENU_SEP_H : MENU_ROW_H;
    return h;
}

static UiRect menu_item_rect(int index)
{
    int y = MENU.y + MENU_PAD;
    for (int i = 0; i < index; i++)
        y += MENU.items[i] == M_SEP ? MENU_SEP_H : MENU_ROW_H;
    return ui_rect(MENU.x + MENU_PAD, y, MENU_W - MENU_PAD * 2, MENU.items[index] == M_SEP ? MENU_SEP_H : MENU_ROW_H);
}

/* the selectable item under (x, y), -1 if none */
static int menu_hit(int x, int y)
{
    for (int i = 0; i < MENU.count; i++)
        if (MENU.items[i] != M_SEP && ui_contains(menu_item_rect(i), x, y))
            return i;
    return -1;
}

/* Open the menu at (x, y), kept inside the window's content area. */
static void menu_open(int x, int y, int kind)
{
    MENU.count = 0;
    MENU.kind = kind;
    if (kind == 2)
    {
        MENU.items[MENU.count++] = M_SORT_NAME;
        MENU.items[MENU.count++] = M_SORT_SIZE;
        MENU.items[MENU.count++] = M_SORT_TYPE;
        MENU.items[MENU.count++] = M_SEP;
        MENU.items[MENU.count++] = M_SORT_ASC;
        MENU.items[MENU.count++] = M_SORT_DESC;
    }
    else if (kind == 1)
    {
        MENU.items[MENU.count++] = M_OPEN;
        MENU.items[MENU.count++] = M_SEP;
        MENU.items[MENU.count++] = M_CUT;
        MENU.items[MENU.count++] = M_COPY;
        MENU.items[MENU.count++] = M_SEP;
        MENU.items[MENU.count++] = M_RENAME;
        MENU.items[MENU.count++] = M_DELETE;
    }
    else
    {
        MENU.items[MENU.count++] = M_NEW_FOLDER;
        MENU.items[MENU.count++] = M_NEW_FILE;
        MENU.items[MENU.count++] = M_SEP;
        if (CLIP_ID >= 0 && vfs_node(CLIP_ID)) /* nothing to paste: no entry */
        {
            MENU.items[MENU.count++] = M_PASTE;
            MENU.items[MENU.count++] = M_SEP;
        }
        MENU.items[MENU.count++] = M_REFRESH;
    }
    UiRect b = wm_content(EX_WIN);
    int h = menu_height();
    if (x + MENU_W > b.x + b.w - 4)
        x = b.x + b.w - 4 - MENU_W;
    if (y + h > b.y + b.h - 4)
        y = b.y + b.h - 4 - h;
    MENU.x = x < b.x + 4 ? b.x + 4 : x;
    MENU.y = y < b.y + 4 ? b.y + 4 : y;
    MENU.hover = -1;
    MENU.open = 1;
}

static void menu_run(int item)
{
    MENU.open = 0;
    switch (item)
    {
    case M_OPEN:
        open_item(SEL);
        break;
    case M_RENAME:
        start_rename(SEL);
        break;
    case M_DELETE:
        ask_delete(SEL);
        break;
    case M_NEW_FOLDER:
        new_item(1);
        break;
    case M_NEW_FILE:
        new_item(0);
        break;
    case M_REFRESH:
        reload();
        NOTE = 0;
        break;
    case M_CUT:
        clip_set(1);
        break;
    case M_COPY:
        clip_set(0);
        break;
    case M_PASTE:
        paste();
        break;
    case M_SORT_NAME:
        set_sort(VFS_SORT_NAME, SORT_DESC);
        break;
    case M_SORT_SIZE:
        set_sort(VFS_SORT_SIZE, SORT_DESC);
        break;
    case M_SORT_TYPE:
        set_sort(VFS_SORT_TYPE, SORT_DESC);
        break;
    case M_SORT_ASC:
        set_sort(SORT_KEY, 0);
        break;
    case M_SORT_DESC:
        set_sort(SORT_KEY, 1);
        break;
    default:
        break;
    }
}

/* keyboard: move the highlight to the next / previous selectable item (wraps) */
static void menu_step(int dir)
{
    int i = MENU.hover;
    for (int k = 0; k < MENU.count; k++)
    {
        i = i < 0 ? (dir > 0 ? 0 : MENU.count - 1) : (i + dir + MENU.count) % MENU.count;
        if (MENU.items[i] != M_SEP)
        {
            MENU.hover = i;
            return;
        }
    }
}

static int menu_checked(int item)
{
    switch (item)
    {
    case M_SORT_NAME:
        return SORT_KEY == VFS_SORT_NAME;
    case M_SORT_SIZE:
        return SORT_KEY == VFS_SORT_SIZE;
    case M_SORT_TYPE:
        return SORT_KEY == VFS_SORT_TYPE;
    case M_SORT_ASC:
        return !SORT_DESC;
    case M_SORT_DESC:
        return SORT_DESC;
    default:
        return 0;
    }
}

static void menu_draw(const UiTheme *t)
{
    if (!MENU.open)
        return;
    int h = menu_height();
    gfx_shadow(MENU.x, MENU.y, MENU_W, h, t->radius_m, 14, 0, 5, 120);
    gfx_fill_round_rect(MENU.x, MENU.y, MENU_W, h, t->radius_m, ui_color(t->surface));
    gfx_round_rect(MENU.x, MENU.y, MENU_W, h, t->radius_m, 1, ui_color(t->border));
    for (int i = 0; i < MENU.count; i++)
    {
        UiRect r = menu_item_rect(i);
        int item = MENU.items[i];
        if (item == M_SEP)
        {
            ui_separator(r.x + 6, r.y + r.h / 2, r.w - 12);
            continue;
        }
        if (i == MENU.hover)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h - 2, t->radius_s, ui_color(t->accent), 60);
        UiRgb c = item == M_DELETE ? t->danger : t->text;
        int tx = MENU.kind == 2 ? 34 : 12; /* the sort menu leaves room for a check mark */
        if (menu_checked(item))
            ui_icon(UI_ICON_CHECK, r.x + 18, r.y + (r.h - 2) / 2, 14, t->accent_hi);
        ui_text_in_color(ui_rect(r.x + tx, r.y, r.w - tx - 12, r.h - 2), menu_label(item), UI_TEXT_BODY, UI_ALIGN_LEFT, c);
        ui_text_in_color(ui_rect(r.x + 12, r.y, r.w - 24, r.h - 2), menu_hint(item), UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                         t->text_dim);
    }
}

/* ---- drawing --------------------------------------------------- */
static void draw_folder_icon(int cx, int cy, int s)
{
    int x = cx - s / 2, y = cy - s / 2;
    int h = s * 3 / 4;
    int top = y + (s - h) / 2;
    gfx_fill_round_rect(x, top, s * 2 / 5, h / 3 + 3, 3, gfx_rgb(224, 148, 36));
    gfx_fill_round_rect(x, top + 3, s, h - 3, 4, gfx_rgb(232, 160, 44));
    gfx_fill_round_rect(x, top + 6, s, h - 6, 4, gfx_rgb(255, 204, 92));
    gfx_fill_rect_alpha(x + 3, top + 7, s - 6, 1, 255, 255, 255, 90);
}

static void draw_file_icon(int cx, int cy, int s, int kind)
{
    UiRgb c = kind_color(kind);
    int w = s * 3 / 4, h = s;
    int x = cx - w / 2, y = cy - h / 2;
    int fold = w / 3;
    gfx_fill_round_rect(x, y, w, h, 3, gfx_rgb(236, 241, 252));
    int tri[6] = {(x + w - fold) * 16, y * 16, (x + w) * 16, (y + fold) * 16, (x + w - fold) * 16, (y + fold) * 16};
    gfx_fill_poly_q(tri, 3, gfx_rgb(178, 190, 216), 255);
    gfx_fill_round_rect(x + 3, y + h - 9, w - 6, 5, 2, ui_color(c));
    gfx_fill_rect(x + 3, y + h - 14, w - 6 - w / 4, 2, ui_color(ui_rgb_mix(c, ui_rgb(236, 241, 252), 150)));
}

static void draw_icon(int cx, int cy, int s, int kind)
{
    if (kind == FK_FOLDER)
        draw_folder_icon(cx, cy, s);
    else
        draw_file_icon(cx, cy, s, kind);
}

/* "This PC > Documents > Projects" */
static void crumb_text(char *out, int cap)
{
    char path[160];
    vfs_path(CUR, path, sizeof path);
    int o = put_str(out, 0, cap, "This PC");
    for (const char *p = path; *p && o < cap - 1; p++)
    {
        if (*p == '/')
            o = put_str(out, o, cap, "  >  ");
        else
            out[o++] = *p;
    }
    out[o] = 0;
}

static void draw_toolbar(const Layout *l, const UiTheme *t)
{
    UiRect tb = l->toolbar;
    gfx_fill_rect_alpha(tb.x, tb.y, tb.w, tb.h, t->surface_alt.r, t->surface_alt.g, t->surface_alt.b, 70);
    ui_separator(tb.x, tb.y + tb.h - 1, tb.w);

    /* command row: a divider between New folder and the clipboard / rename buttons */
    gfx_fill_rect_alpha(tb.x + CMD_X0 - 9, tb.y + CMD_Y + 6, 1, 22, t->border.r, t->border.g, t->border.b, 255);

    gfx_fill_rect_alpha(tb.x + SORT_X - 8, tb.y + CMD_Y + 6, 1, 22, t->border.r, t->border.g, t->border.b, 255);

    int px = tb.x + PATH_X;
    UiRect bar = ui_rect(px, tb.y + 9, tb.w - (px - tb.x) - 10 - NAVBTN - 4, 34); /* room for Refresh on the right */
    if (bar.w < 40)
        return;
    gfx_fill_round_rect(bar.x, bar.y, bar.w, bar.h, t->radius_m, ui_color(t->surface));
    gfx_round_rect(bar.x, bar.y, bar.w, bar.h, t->radius_m, 1, ui_color(t->border));
    draw_folder_icon(bar.x + 20, bar.y + bar.h / 2, 18);
    char crumbs[200];
    crumb_text(crumbs, sizeof crumbs);
    ui_text_in(ui_rect(bar.x + 38, bar.y, bar.w - 46, bar.h), crumbs, UI_TEXT_BODY, UI_ALIGN_LEFT);
}

static int place_id(int i) { return vfs_lookup(PLACES[i].path); }

static void draw_sidebar(const Layout *l, const UiTheme *t)
{
    UiRect s = l->side;
    gfx_fill_rect_alpha(s.x, s.y, s.w, s.h, t->surface_alt.r, t->surface_alt.g, t->surface_alt.b, 36);
    gfx_vline(s.x + s.w - 1, s.y, s.h, ui_color(t->border));
    ui_text_in_color(ui_rect(s.x + 16, s.y + 8, s.w - 24, 22), "PLACES", UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);

    int y = s.y + 32;
    for (int i = 0; i < PLACE_N && y + PLACE_H <= s.y + s.h; i++, y += PLACE_H + 2)
    {
        int id = place_id(i);
        if (id < 0)
            continue;
        /* "This PC" is current only at the root; the others while inside them */
        int here = id == VFS_ROOT ? CUR == VFS_ROOT : vfs_is_under(CUR, id);
        UiRect r = ui_rect(s.x + 8, y, s.w - 16, PLACE_H);
        if (!here && i == HOVER_PLACE)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 20);
        if (here)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 44);
            gfx_fill_round_rect(r.x, r.y + 8, 3, r.h - 16, 1, ui_color(t->accent_hi));
        }
        draw_icon(r.x + 22, r.y + r.h / 2, 20, FK_FOLDER);
        ui_text_in_color(ui_rect(r.x + 42, r.y, r.w - 46, r.h), PLACES[i].label, here ? UI_TEXT_LABEL : UI_TEXT_BODY,
                         UI_ALIGN_LEFT, here ? t->text : t->text_dim);
    }

    /* storage: how much of the node pool is in use */
    int used = vfs_count();
    UiRect st = ui_rect(s.x + 16, s.y + s.h - 62, s.w - 32, 54);
    if (st.y > y)
    {
        ui_text_in_color(ui_rect(st.x, st.y, st.w, 20), "RAM volume", UI_TEXT_LABEL, UI_ALIGN_LEFT, t->text);
        ui_progress(ui_rect(st.x, st.y + 24, st.w, 8), used * 100 / VFS_MAX_NODES);
        char line[40];
        int o = put_uint(line, 0, used);
        o = put_str(line, o, sizeof line, " of ");
        o = put_uint(line, o, VFS_MAX_NODES);
        o = put_str(line, o, sizeof line, " entries");
        line[o] = 0;
        ui_text_in_color(ui_rect(st.x, st.y + 34, st.w, 18), line, UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    }
}

/* where the Type and Size columns are (shared by drawing and header clicks) */
typedef struct
{
    int show_type, type_x, size_x;
} Cols;

static Cols columns(const Layout *l)
{
    Cols c;
    c.show_type = l->list.w >= 360;
    int right = l->list.x + l->list.w - 14; /* leave room for the scrollbar */
    c.size_x = right - SIZE_COL_W;
    c.type_x = c.show_type ? c.size_x - TYPE_COL_W : c.size_x;
    return c;
}

/* a column title; the sorted column is brighter and has a small up / down triangle */
static void head_cell(UiRect cell, const char *label, UiAlign align, int key, const UiTheme *t)
{
    int active = SORT_KEY == key;
    ui_text_in_color(cell, label, UI_TEXT_SMALL, align, active ? t->text : t->text_dim);
    if (!active)
        return;
    int tw = ui_text_width(label, UI_TEXT_SMALL);
    int ax = align == UI_ALIGN_RIGHT ? cell.x + cell.w - tw - 10 : cell.x + tw + 9;
    int ay = cell.y + cell.h / 2;
    int up[6] = {(ax - 4) * 16, (ay + 2) * 16, (ax + 4) * 16, (ay + 2) * 16, ax * 16, (ay - 3) * 16};
    int down[6] = {(ax - 4) * 16, (ay - 2) * 16, (ax + 4) * 16, (ay - 2) * 16, ax * 16, (ay + 3) * 16};
    gfx_fill_poly_q(SORT_DESC ? down : up, 3, ui_color(t->accent_hi), 255);
}

static void draw_list(const Layout *l, const UiTheme *t)
{
    Cols cols = columns(l);
    int show_type = cols.show_type, size_x = cols.size_x, type_x = cols.type_x;

    /* column header (click a title to sort by it) */
    UiRect h = l->head;
    head_cell(ui_rect(h.x + 44, h.y, 200, h.h), "Name", UI_ALIGN_LEFT, VFS_SORT_NAME, t);
    if (show_type)
        head_cell(ui_rect(type_x, h.y, TYPE_COL_W, h.h), "Type", UI_ALIGN_LEFT, VFS_SORT_TYPE, t);
    head_cell(ui_rect(size_x, h.y, SIZE_COL_W - 6, h.h), "Size", UI_ALIGN_RIGHT, VFS_SORT_SIZE, t);
    ui_separator(h.x + 8, h.y + h.h - 1, h.w - 16);

    if (NITEMS == 0)
    {
        draw_folder_icon(l->list.x + l->list.w / 2, l->list.y + l->list.h / 2 - 22, 40);
        ui_text_in_color(ui_rect(l->list.x, l->list.y + l->list.h / 2, l->list.w, 24), "This folder is empty",
                         UI_TEXT_BODY, UI_ALIGN_CENTER, t->text_dim);
        return;
    }

    GfxClip saved = gfx_get_clip();
    ui_clip(l->list);
    int vis = visible_rows(l);
    for (int i = 0; i <= vis && SCROLL + i < NITEMS; i++)
    {
        int ri = SCROLL + i;
        const VfsNode *n = vfs_node(ITEMS[ri]);
        if (!n)
            continue;
        UiRect r = ui_rect(l->list.x + 6, l->list.y + i * ROW_H, l->list.w - 12 - 8, ROW_H - 2);
        if (ri == SEL)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 46);
            gfx_fill_round_rect(r.x, r.y + 7, 3, r.h - 14, 1, ui_color(t->accent_hi));
        }
        else if (ri == HOVER_ROW)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 20);
        }
        else if (ri & 1)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->surface_alt), 40);
        }
        int kind = kind_of(n);
        draw_icon(r.x + 24, r.y + r.h / 2, ICON_PX, kind);
        if (CLIP_CUT && ITEMS[ri] == CLIP_ID) /* waiting to be moved: faded */
            gfx_fill_round_rect_alpha(r.x + 12, r.y + 4, ICON_PX + 2, ICON_PX + 2, 4, ui_color(t->surface), 140);
        if (ITEMS[ri] == RENAME_ID && EX_WIN)
            ui_edit_draw(ui_rect(r.x + 40, r.y + 2, type_x - r.x - 46, r.h - 4), &EX_WIN->edits[0], "", 1);
        else
            ui_text_in(ui_rect(r.x + 42, r.y, type_x - r.x - 46, r.h), n->name, ri == SEL ? UI_TEXT_LABEL : UI_TEXT_BODY,
                       UI_ALIGN_LEFT);
        if (show_type)
            ui_text_in_color(ui_rect(type_x, r.y, TYPE_COL_W, r.h), kind_name(kind), UI_TEXT_SMALL, UI_ALIGN_LEFT,
                             t->text_dim);
        if (!n->is_dir)
        {
            char sz[20];
            fmt_size(n->size, sz);
            ui_text_in_color(ui_rect(size_x, r.y, SIZE_COL_W - 6, r.h), sz, UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
        }
    }
    gfx_restore_clip(saved);

    /* scrollbar */
    if (NITEMS > vis)
    {
        int track_h = l->list.h - 8;
        int thumb_h = track_h * vis / NITEMS;
        if (thumb_h < 24)
            thumb_h = 24;
        int span = NITEMS - vis;
        int thumb_y = l->list.y + 4 + (track_h - thumb_h) * SCROLL / span;
        int tx = l->list.x + l->list.w - 9;
        gfx_fill_round_rect_alpha(tx, l->list.y + 4, 4, track_h, 2, ui_color(t->surface_alt), 120);
        gfx_fill_round_rect(tx, thumb_y, 4, thumb_h, 2, ui_color(t->text_dim));
    }
}

static void draw_status(const Layout *l, const UiTheme *t)
{
    UiRect s = l->status;
    gfx_fill_rect_alpha(s.x, s.y, s.w, s.h, t->surface_alt.r, t->surface_alt.g, t->surface_alt.b, 70);
    ui_separator(s.x, s.y, s.w);

    char line[96];
    int o = 0;
    UiRgb color = t->text_dim;
    if (NOTE)
    {
        o = put_str(line, 0, sizeof line, NOTE);
        color = t->warning;
    }
    else if (SEL >= 0 && SEL < NITEMS && vfs_node(ITEMS[SEL]))
    {
        const VfsNode *n = vfs_node(ITEMS[SEL]);
        o = put_str(line, 0, sizeof line, n->name);
        if (!n->is_dir)
        {
            char sz[20];
            fmt_size(n->size, sz);
            o = put_str(line, o, sizeof line, "   -   ");
            o = put_str(line, o, sizeof line, sz);
        }
        color = t->text;
    }
    else
    {
        o = put_uint(line, 0, NITEMS);
        o = put_str(line, o, sizeof line, NITEMS == 1 ? " item" : " items");
    }
    line[o] = 0;
    int right_w = s.w >= 420 ? 190 : 0;
    ui_text_in_color(ui_rect(s.x + 14, s.y, s.w - 28 - right_w, s.h), line, UI_TEXT_SMALL, UI_ALIGN_LEFT, color);
    if (right_w)
    {
        char cnt[40];
        int c = put_uint(cnt, 0, NITEMS);
        c = put_str(cnt, c, sizeof cnt, NITEMS == 1 ? " item" : " items");
        cnt[c] = 0;
        ui_text_in_color(ui_rect(s.x + s.w - right_w - 14, s.y, right_w, s.h), cnt, UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                         t->text_dim);
    }
}

/* tooltip: a small label under the toolbar button the pointer rests on (above it near the bottom) */
static void tip_draw(const Window *w, UiRect body, const UiTheme *t)
{
    static const char *const TIPS[BTN_COUNT] = {"Back (Alt+Left)", "Forward (Alt+Right)", "Up one level (Alt+Up)",
                                                "Refresh (F5)", "New folder (Ctrl+Shift+N)", "New text file",
                                                "Cut (Ctrl+X)", "Copy (Ctrl+C)", "Paste (Ctrl+V)", "Rename (F2)",
                                                "Delete (Del)", "Sort by"};
    if (TIP_BTN < 0 || TIP_BTN >= BTN_COUNT || TIP_BTN >= w->btn_count)
        return;
    const char *s = TIPS[TIP_BTN];
    UiRect b = w->btn[TIP_BTN].r;
    int tw = ui_text_width(s, UI_TEXT_SMALL) + 22, th = 26;
    int x = b.x + b.w / 2 - tw / 2, y = b.y + b.h + 6;
    if (x + tw > body.x + body.w - 4)
        x = body.x + body.w - 4 - tw;
    if (x < body.x + 4)
        x = body.x + 4;
    if (y + th > body.y + body.h - 4)
        y = b.y - th - 6;
    gfx_shadow(x, y, tw, th, t->radius_s, 8, 0, 3, 110);
    gfx_fill_round_rect(x, y, tw, th, t->radius_s, ui_color(t->titlebar));
    gfx_round_rect(x, y, tw, th, t->radius_s, 1, ui_color(t->border));
    ui_text_in_color(ui_rect(x, y, tw, th), s, UI_TEXT_SMALL, UI_ALIGN_CENTER, t->text);
}

/* grey out the toolbar buttons that cannot do anything right now */
static void sync_buttons(void)
{
    if (!EX_WIN)
        return;
    int idle = RENAME_ID < 0;
    int has = SEL >= 0 && SEL < NITEMS;
    EX_WIN->btn[BTN_REFRESH].enabled = idle;
    EX_WIN->btn[BTN_NEW_FOLDER].enabled = idle;
    EX_WIN->btn[BTN_NEW_FILE].enabled = idle;
    EX_WIN->btn[BTN_CUT].enabled = idle && has;
    EX_WIN->btn[BTN_COPY].enabled = idle && has;
    EX_WIN->btn[BTN_PASTE].enabled = idle && vfs_node(CLIP_ID) != 0;
    EX_WIN->btn[BTN_RENAME].enabled = idle && has;
    EX_WIN->btn[BTN_DELETE].enabled = idle && has && !DEL_WIN;
    EX_WIN->btn[BTN_SORT].enabled = idle;
    update_nav_buttons();
}

static void ex_draw(Window *w, UiRect body)
{
    const UiTheme *t = ui_theme();
    Layout l = layout(body);
    clamp_scroll(&l);
    sync_buttons(); /* before the window draws its buttons */
    draw_sidebar(&l, t);
    draw_list(&l, t);
    draw_toolbar(&l, t);
    draw_status(&l, t);
    if (MENU.open && w != wm_top())
        MENU.open = 0; /* focus moved elsewhere (another window or the delete dialog) */
    menu_draw(t);
    if (TIP_SHOWN && !MENU.open)
        tip_draw(w, body, t);
}

/* ---- input ----------------------------------------------------- */
static int ex_press(Window *w, int x, int y)
{
    UiRect body = wm_content(w);
    Layout l = layout(body);

    TIP_BTN = -1; /* a click hides the tooltip until the pointer moves again */
    TIP_SHOWN = 0;
    if (MENU.open) /* a click on an item runs it; a click anywhere else only closes the menu */
    {
        int hit = menu_hit(x, y);
        if (hit >= 0)
            menu_run(MENU.items[hit]);
        else
            MENU.open = 0;
        return 1;
    }
    if (RENAME_ID >= 0)
    {
        UiRect f = w->edits[0].field;
        if (f.w > 0 && ui_contains(f, x, y))
        {
            ui_edit_mouse(&w->edits[0], x, y, 1);
            ui_caret_restart();
            return 1;
        }
        if (!end_rename(1)) /* name refused: stay in the field */
            return 1;
        l = layout(body);
    }

    if (ui_contains(l.side, x, y))
    {
        int row = (y - (l.side.y + 32)) / (PLACE_H + 2);
        if (y >= l.side.y + 32 && row >= 0 && row < PLACE_N && place_id(row) >= 0)
        {
            go_to(place_id(row), 1);
            return 1;
        }
        return 0;
    }
    if (ui_contains(l.head, x, y)) /* a column title: sort by it, again to reverse */
    {
        Cols cols = columns(&l);
        int key = x >= cols.size_x ? VFS_SORT_SIZE : (cols.show_type && x >= cols.type_x) ? VFS_SORT_TYPE : VFS_SORT_NAME;
        set_sort(key, key == SORT_KEY ? !SORT_DESC : 0);
        return 1;
    }
    if (!ui_contains(l.list, x, y))
        return 0;

    int row = SCROLL + (y - l.list.y) / ROW_H;
    if (row < 0 || row >= NITEMS)
    {
        int changed = SEL >= 0 || NOTE;
        SEL = -1;
        NOTE = 0;
        LAST_ROW = -1;
        return changed;
    }
    unsigned long long now = timer_ms();
    int double_click = row == LAST_ROW && now - LAST_CLICK_MS <= DOUBLE_CLICK_MS;
    LAST_ROW = row;
    LAST_CLICK_MS = now;
    select_row(body, row);
    if (double_click)
    {
        LAST_ROW = -1;
        open_item(row);
    }
    return 1;
}

/* holding the button after a press in the rename field selects text */
static int ex_drag(Window *w, int x, int y, unsigned buttons)
{
    if (RENAME_ID < 0 || !w->edits[0].dragging)
        return 0;
    int changed = ui_edit_mouse(&w->edits[0], x, y, buttons);
    if (changed)
        ui_caret_restart();
    return changed;
}

/* right click: select the entry under the pointer and show its menu, or the folder menu on empty space */
static int ex_rpress(Window *w, int x, int y)
{
    UiRect body = wm_content(w);
    Layout l = layout(body);
    if (RENAME_ID >= 0 && !end_rename(1))
        return 1;
    MENU.open = 0;
    if (!ui_contains(l.list, x, y) && !ui_contains(l.head, x, y))
        return 1; /* toolbar, sidebar, status bar: no menu */
    l = layout(body);
    int row = ui_contains(l.list, x, y) ? SCROLL + (y - l.list.y) / ROW_H : -1;
    if (row >= 0 && row < NITEMS)
    {
        select_row(body, row);
        menu_open(x, y, 1);
    }
    else
    {
        SEL = -1;
        NOTE = 0;
        menu_open(x, y, 0);
    }
    return 1;
}

/* pointer moved: hover highlight in the list / sidebar, or in the open menu */
static int ex_hover(Window *w, int x, int y)
{
    int old_row = HOVER_ROW, old_place = HOVER_PLACE, old_menu = MENU.hover;
    int old_tip = TIP_BTN, was_shown = TIP_SHOWN;
    TIP_BTN = -1;
    if (x >= 0 && !MENU.open)
        for (int i = 0; i < w->btn_count; i++)
            if (ui_contains(w->btn[i].r, x, y))
                TIP_BTN = i;
    if (TIP_BTN != old_tip) /* a new button: wait again before showing its tooltip */
    {
        TIP_SINCE = timer_ms();
        TIP_SHOWN = 0;
    }
    HOVER_ROW = HOVER_PLACE = -1;
    if (MENU.open)
        MENU.hover = x < 0 ? -1 : menu_hit(x, y);
    else if (x >= 0)
    {
        Layout l = layout(wm_content(w));
        if (ui_contains(l.list, x, y))
        {
            int row = SCROLL + (y - l.list.y) / ROW_H;
            HOVER_ROW = row < NITEMS ? row : -1;
        }
        else if (ui_contains(l.side, x, y) && y >= l.side.y + 32)
        {
            int p = (y - (l.side.y + 32)) / (PLACE_H + 2);
            HOVER_PLACE = p < PLACE_N ? p : -1;
        }
    }
    return old_row != HOVER_ROW || old_place != HOVER_PLACE || old_menu != MENU.hover || was_shown != TIP_SHOWN;
}

/* time passes: show the tooltip once the pointer has rested on a button long enough */
static int ex_tick(Window *w)
{
    int want = TIP_BTN >= 0 && !MENU.open && w == wm_top() && timer_ms() - TIP_SINCE >= TIP_DELAY_MS;
    if (want == TIP_SHOWN)
        return 0;
    TIP_SHOWN = want;
    return 1;
}

static int ex_wheel(Window *w, int steps)
{
    Layout l = layout(wm_content(w));
    int old = SCROLL;
    SCROLL += steps * WHEEL_ROWS;
    clamp_scroll(&l);
    return SCROLL != old;
}

/* Jump to the next entry (after the selection, wrapping) whose name starts with `c`. */
static void type_ahead(UiRect body, int c)
{
    c = lower(c);
    for (int k = 1; k <= NITEMS; k++)
    {
        int row = (SEL + k + NITEMS) % NITEMS;
        const VfsNode *n = vfs_node(ITEMS[row]);
        if (n && lower((unsigned char)n->name[0]) == c)
        {
            select_row(body, row);
            return;
        }
    }
}

static void ex_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    UiRect body = wm_content(w);
    Layout l = layout(body);
    int page = visible_rows(&l) - 1;
    if (page < 1)
        page = 1;

    if (RENAME_ID >= 0) /* the name field has the keyboard */
    {
        if (k->key == KEY_ENTER)
            end_rename(1);
        else if (k->key == KEY_ESC)
            end_rename(0);
        else if (ui_edit_key(&w->edits[0], k))
        {
            NOTE = 0;
            ui_caret_restart();
        }
        return;
    }
    if (MENU.open)
    {
        if (k->key == KEY_UP)
            menu_step(-1);
        else if (k->key == KEY_DOWN)
            menu_step(1);
        else if (k->key == KEY_ENTER && MENU.hover >= 0)
            menu_run(MENU.items[MENU.hover]);
        else if (k->key == KEY_ESC)
            MENU.open = 0;
        return;
    }

    if ((k->mods & KMOD_ALT) && k->key == KEY_LEFT)
    {
        go_back();
        return;
    }
    if ((k->mods & KMOD_ALT) && k->key == KEY_RIGHT)
    {
        go_forward();
        return;
    }
    if ((k->mods & KMOD_ALT) && k->key == KEY_UP)
    {
        go_up();
        return;
    }
    switch (k->key)
    {
    case KEY_F5:
        reload();
        NOTE = 0;
        return;
    case KEY_F2:
        start_rename(SEL);
        return;
    case KEY_DELETE:
        ask_delete(SEL);
        return;
    case KEY_BACKSPACE:
        go_up();
        return;
    case KEY_ENTER:
        open_item(SEL);
        return;
    default:
        break;
    }
    if ((k->mods & KMOD_CTRL) && !(k->mods & (KMOD_SHIFT | KMOD_ALT)) && (k->key == 'x' || k->key == 'c' || k->key == 'v'))
    {
        if (k->key == 'v')
            paste();
        else
            clip_set(k->key == 'x');
        return;
    }
    if ((k->mods & KMOD_CTRL) && (k->mods & KMOD_SHIFT) && k->key == 'n')
    {
        new_item(1);
        return;
    }
    if (NITEMS == 0)
        return;
    switch (k->key)
    {
    case KEY_UP:
        select_row(body, SEL < 0 ? 0 : SEL - 1);
        break;
    case KEY_DOWN:
        select_row(body, SEL < 0 ? 0 : SEL + 1);
        break;
    case KEY_PAGEUP:
        select_row(body, SEL - page < 0 ? 0 : SEL - page);
        break;
    case KEY_PAGEDOWN:
        select_row(body, SEL + page >= NITEMS ? NITEMS - 1 : SEL + page);
        break;
    case KEY_HOME:
        select_row(body, 0);
        break;
    case KEY_END:
        select_row(body, NITEMS - 1);
        break;
    default:
        if (k->ch > ' ' && k->ch < 127 && !(k->mods & (KMOD_CTRL | KMOD_ALT)))
            type_ahead(body, k->ch);
        break;
    }
}

static void ex_close(Window *w)
{
    (void)w;
    EX_WIN = 0;
    RENAME_ID = -1;
    MENU.open = 0;
    TIP_BTN = -1;
    TIP_SHOWN = 0;
}

static void explorer_app_open(void)
{
    if (EX_WIN)
    {
        wm_activate(EX_WIN);
        return;
    }
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 760, h = 540;
    if (w > W - 20)
        w = W - 20;
    if (h > H - TASKBAR_HEIGHT - 8)
        h = H - TASKBAR_HEIGHT - 8;
    int x = (W - w) / 2, y = (H - TASKBAR_HEIGHT - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;

    Window *win = wm_create("File Explorer", x, y, w, h, ex_draw, 0);
    if (!win)
    {
        desktop_set_status("Window limit reached - close one first.");
        wm_invalidate();
        return;
    }
    win->on_key = ex_key;
    win->on_press = ex_press;
    win->on_wheel = ex_wheel;
    win->on_drag = ex_drag;
    win->on_rpress = ex_rpress;
    win->on_hover = ex_hover;
    win->on_tick = ex_tick;
    win->on_close = ex_close;
    win->icon = TASKBAR_ICON_FILES;
    wm_set_min_size(win, 480, 320);
    EX_WIN = win;
    desktop_set_status(0);

    /* Toolbar buttons, in the order of the BTN_* ids (sync_buttons / update_nav_buttons rely on it).
     * Navigation row: Back, Forward, Up (left), Refresh (right end). Command row: New folder, New file,
     * then Cut, Copy, Paste, Rename, Delete after a divider. All ghost style: only the icon shows until hovered. */
    int cw = w - 2;
    int step = NAVBTN + 2;
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(10, 8, NAVBTN, NAVBTN), 0, on_back, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(10 + step, 8, NAVBTN, NAVBTN), 0, on_forward, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(10 + 2 * step, 8, NAVBTN, NAVBTN), 0, on_up, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(cw - 10 - NAVBTN, 8, NAVBTN, NAVBTN), WM_ANCHOR_RIGHT, on_refresh, 0);
    wm_add_button_ex(win, "New folder", UI_BTN_GHOST, ui_rect(NEWBTN_X, CMD_Y, NEWBTN_W, 34), 0, on_new_folder, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(NEWBTN_X + NEWBTN_W + 2, CMD_Y, NAVBTN, 34), 0, on_new_file, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(CMD_X0, CMD_Y, NAVBTN, 34), 0, on_cut, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(CMD_X0 + step, CMD_Y, NAVBTN, 34), 0, on_copy, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(CMD_X0 + 2 * step, CMD_Y, NAVBTN, 34), 0, on_paste, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(CMD_X0 + 3 * step, CMD_Y, NAVBTN, 34), 0, on_rename, 0);
    wm_add_button_ex(win, "", UI_BTN_GHOST, ui_rect(CMD_X0 + 4 * step, CMD_Y, NAVBTN, 34), 0, on_delete, 0);
    wm_add_button_ex(win, "Sort", UI_BTN_GHOST, ui_rect(SORT_X, CMD_Y, SORT_W, 34), 0, on_sort, 0);

    wm_set_button_icon(win, BTN_BACK, UI_ICON_ARROW_LEFT);
    wm_set_button_icon(win, BTN_FORWARD, UI_ICON_ARROW_RIGHT);
    wm_set_button_icon(win, BTN_UP, UI_ICON_ARROW_UP);
    wm_set_button_icon(win, BTN_REFRESH, UI_ICON_REFRESH);
    wm_set_button_icon(win, BTN_NEW_FOLDER, UI_ICON_FOLDER_PLUS);
    wm_set_button_icon(win, BTN_NEW_FILE, UI_ICON_FILE_PLUS);
    wm_set_button_icon(win, BTN_CUT, UI_ICON_CUT);
    wm_set_button_icon(win, BTN_COPY, UI_ICON_COPY);
    wm_set_button_icon(win, BTN_PASTE, UI_ICON_PASTE);
    wm_set_button_icon(win, BTN_RENAME, UI_ICON_RENAME);
    wm_set_button_icon(win, BTN_DELETE, UI_ICON_TRASH);
    wm_set_button_icon(win, BTN_SORT, UI_ICON_SORT);

    CUR = VFS_ROOT;
    HIST_N = 0;
    FWD_N = 0;
    CLIP_ID = -1;
    SEL = -1;
    SCROLL = 0;
    NOTE = 0;
    LAST_ROW = -1;
    HOVER_ROW = HOVER_PLACE = -1;
    RENAME_ID = -1;
    MENU.open = 0;
    reload();
    if (NITEMS > 0)
        SEL = 0;
}

const AppInfo APP_EXPLORER = {
    .name = "File Explorer",
    .desc = "Browse files and folders",
    .icon = UI_ICON_FILES,
    .open = explorer_app_open,
};
