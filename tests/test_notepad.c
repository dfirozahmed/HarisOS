/* Host-side tests for the Notepad text buffer, selection and undo / redo (apps/notepad/notepad_app.c).
 * The app file is included so its static functions can be called; drawing and the window manager are stubbed. */
#include "../apps/notepad/notepad_app.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- stubs (defined after the include, so the compiler checks them against the real prototypes) ---- */
static UiTheme THEME_STORE;
static char CLIP_STORE[NP_MAX + 1];
static int CLIP_N;
void desktop_set_status(const char *s) { (void)s; }
void gfx_clip_intersect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void gfx_fill_rect(int x, int y, int w, int h, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void gfx_fill_rect_alpha(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    (void)x; (void)y; (void)w; (void)h; (void)r; (void)g; (void)b; (void)a;
}
void gfx_fill_round_rect(int x, int y, int w, int h, int r, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)c; }
GfxClip gfx_get_clip(void) { GfxClip c = {0, 0, 0, 0}; return c; }
unsigned gfx_height(void) { return 768; }
unsigned gfx_width(void) { return 1024; }
void gfx_restore_clip(GfxClip c) { (void)c; }
void gfx_round_rect(int x, int y, int w, int h, int r, int t, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)t; (void)c; }
void gfx_text(const Font *f, int x, int y, const char *s, GfxColor c) { (void)f; (void)x; (void)y; (void)s; (void)c; }
int gfx_text_fit(const Font *f, const char *s, int max_w)
{
    (void)f;
    int n = (int)strlen(s), fit = max_w / 8; /* 8 px per character */
    return fit < n ? (fit < 0 ? 0 : fit) : n;
}
int gfx_text_w(const Font *f, const char *s) { (void)f; return (int)strlen(s) * 8; }
void ui_caret_restart(void) {}
int ui_caret_visible(void) { return 1; }
void ui_clipboard_set(const char *s, int len)
{
    memcpy(CLIP_STORE, s, (size_t)len);
    CLIP_N = len;
}
int ui_clipboard_get(char *out, int cap)
{
    int n = CLIP_N < cap - 1 ? CLIP_N : cap - 1;
    memcpy(out, CLIP_STORE, (size_t)n);
    out[n] = 0;
    return n;
}
GfxColor ui_color(UiRgb c) { (void)c; return 0; }
int ui_contains(UiRect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
const Font *ui_font(UiTextStyle s) { (void)s; return 0; }
UiRect ui_rect(int x, int y, int w, int h) { UiRect r = {x, y, w, h}; return r; }
UiRect ui_inset(UiRect r, int a) { return ui_rect(r.x + a, r.y + a, r.w - 2 * a, r.h - 2 * a); }
int ui_text_height(UiTextStyle s) { (void)s; return 16; }
void ui_text_in_color(UiRect r, const char *s, UiTextStyle st, UiAlign al, UiRgb c) { (void)r; (void)s; (void)st; (void)al; (void)c; }
const UiTheme *ui_theme(void) { return &THEME_STORE; }
int ntfs_unsupported_line(void) { return 0; }
const char *ntfs_unsupported_why(void) { return 0; }
void ui_text(int x, int y, const char *s, UiTextStyle st) { (void)x; (void)y; (void)s; (void)st; }
void ui_text_color(int x, int y, const char *s, UiTextStyle st, UiRgb c) { (void)x; (void)y; (void)s; (void)st; (void)c; }
void ui_edit_draw(UiRect r, UiEdit *e, const char *p, int f) { (void)r; (void)e; (void)p; (void)f; }
int ui_edit_key(UiEdit *e, const KeyEvent *k) /* a field that types printable keys and handles Backspace */
{
    if (k->pressed && k->ch >= 32 && k->ch < 127 && !(k->mods & KMOD_CTRL) && e->len < UI_EDIT_MAX)
    {
        e->text[e->len++] = (char)k->ch;
        e->text[e->len] = 0;
        e->cursor = e->anchor = e->len;
        return 1;
    }
    if (k->pressed && k->key == KEY_BACKSPACE && e->len > 0)
    {
        e->text[--e->len] = 0;
        e->cursor = e->anchor = e->len;
        return 1;
    }
    return 0;
}
void ui_edit_select_all(UiEdit *e) { e->anchor = 0; e->cursor = e->len; }
void ui_button(UiRect r, const char *l, UiButtonKind k, UiState s) { (void)r; (void)l; (void)k; (void)s; }
void ui_button_icon(UiRect r, const char *l, int i, UiButtonKind k, UiState s) { (void)r; (void)l; (void)i; (void)k; (void)s; }
int ui_edit_mouse(UiEdit *e, int x, int y, unsigned b) { (void)e; (void)x; (void)y; (void)b; return 0; }
void ui_edit_set_text(UiEdit *e, const char *s)
{
    strncpy(e->text, s, UI_EDIT_MAX);
    e->text[UI_EDIT_MAX] = 0;
    e->len = e->cursor = e->anchor = (int)strlen(e->text);
}
void ui_text_in(UiRect r, const char *s, UiTextStyle st, UiAlign a) { (void)r; (void)s; (void)st; (void)a; }
void ui_icon(UiIcon i, int x, int y, int size, UiRgb t) { (void)i; (void)x; (void)y; (void)size; (void)t; }
void wm_set_button_icon(Window *w, int i, UiIcon icon) { (void)w; (void)i; (void)icon; }
static unsigned long long NOW;
unsigned long long timer_ms(void) { return NOW; }
static Window *STUB_CLOSED; /* the last window wm_close() was asked to close */
void wm_close(Window *w)
{
    STUB_CLOSED = w;
    if (w && w->on_close)
        w->on_close(w); /* like the real window manager: the app learns that its window is gone */
}
int wm_add_button(Window *w, const char *l, UiButtonKind k, UiRect r, UiButtonFn f, void *u)
{
    (void)w; (void)l; (void)k; (void)r; (void)f; (void)u;
    return 0;
}
int wm_add_button_ex(Window *w, const char *l, UiButtonKind k, UiRect r, int a, UiButtonFn f, void *u)
{
    (void)w; (void)l; (void)k; (void)r; (void)a; (void)f; (void)u;
    return 0;
}
static int STUB_DIALOG; /* 1 = wm_create_dialog gives a window, 0 = "no free window" */
static Window STUB_DLGW;
Window *wm_create_dialog(Window *o, const char *t, int x, int y, int w, int h, WmDrawFn d, void *u)
{
    (void)o; (void)t; (void)x; (void)y; (void)w; (void)h; (void)d; (void)u;
    return STUB_DIALOG ? &STUB_DLGW : 0;
}
void wm_activate(Window *w) { (void)w; }
void wm_add_damage(UiRect r) { (void)r; }
UiRect wm_content(const Window *w) { (void)w; return ui_rect(0, 0, 400, 300); }
Window *wm_create(const char *t, int x, int y, int w, int h, WmDrawFn d, void *u)
{
    (void)t; (void)x; (void)y; (void)w; (void)h; (void)d; (void)u;
    return 0;
}
void wm_invalidate(void) {}
void wm_key_partial(void) {}
void wm_set_min_size(Window *w, int a, int b) { (void)w; (void)a; (void)b; }

/* ---- a tiny in-memory drive for the file tests ---- */
typedef struct
{
    int used;
    char name[VFS_NAME_MAX];
    unsigned char data[NP_MAX + 64];
    unsigned size;
} FFile;
static FFile FF[8];

static int ff_list(void *ctx, int dir)
{
    (void)ctx;
    for (int i = 0; i < 8; i++)
        if (FF[i].used)
        {
            unsigned char fs[VFS_FS_BYTES] = {(unsigned char)i};
            vfs_attach(dir, FF[i].name, 0, FF[i].size, fs);
        }
    return 0;
}
static int ff_create(void *ctx, VfsNode *parent, const char *name, int is_dir, unsigned char *fs_out)
{
    (void)ctx; (void)parent;
    if (is_dir)
        return VFS_ERR_UNSUPPORTED;
    for (int i = 0; i < 8; i++)
        if (!FF[i].used)
        {
            memset(&FF[i], 0, sizeof FF[i]);
            FF[i].used = 1;
            strcpy(FF[i].name, name);
            fs_out[0] = (unsigned char)i;
            return 0;
        }
    return VFS_ERR_FULL;
}
static int ff_remove(void *ctx, const VfsNode *n) { (void)ctx; FF[n->fs[0]].used = 0; return 0; }
static int ff_rename(void *ctx, VfsNode *n, const char *name) { (void)ctx; strcpy(FF[n->fs[0]].name, name); return 0; }
static int ff_read(void *ctx, const VfsNode *n, unsigned off, void *buf, unsigned len)
{
    (void)ctx;
    const FFile *f = &FF[n->fs[0]];
    if (off >= f->size)
        return 0;
    if (len > f->size - off)
        len = f->size - off;
    memcpy(buf, f->data + off, len);
    return (int)len;
}
static int ff_write(void *ctx, VfsNode *n, unsigned off, const void *buf, unsigned len)
{
    (void)ctx;
    FFile *f = &FF[n->fs[0]];
    if (off + len > sizeof f->data)
        return VFS_ERR_FULL;
    memcpy(f->data + off, buf, len);
    if (off + len > f->size)
        f->size = off + len;
    n->size = f->size;
    return (int)len;
}
static int FF_LOCKED;
static void ff_info(void *ctx, VfsVolInfo *out)
{
    (void)ctx;
    strcpy(out->fs_name, "FAKE");
    out->writable = 1;
    out->lockable = 1;
    out->locked = FF_LOCKED;
}
static const VfsBackend FAKE = {ff_list, ff_create, ff_remove, ff_rename, 0, ff_read, ff_write, ff_info, 0, 0, 0};

static void reset(void);
static void type(const char *s);
static int is(const char *expect);

static void put_file(const char *name, const void *data, unsigned len)
{
    int id = vfs_save(vfs_volume_node(1), name, data, len);
    assert(id > 0);
}

static void test_files(void)
{
    vfs_init();
    assert(vfs_mount(&FAKE, 0, "Fake") == 1);
    reset();
    NP_PATH[0] = 0;

    /* save a new file, then a changed text over it: exactly the text, one file */
    type("hello\nworld");
    assert(save_file("/Fake/note.txt") == 0 && !NP_DIRTY && same_str(NP_PATH, "/Fake/note.txt"));
    int id = vfs_lookup("/Fake/note.txt");
    assert(id > 0 && vfs_node(id)->size == 11 && memcmp(FF[0].data, "hello\nworld", 11) == 0);
    NP_CUR = NP_ANCHOR = 0;
    insert_text("X", 1);
    NP_DIRTY = 1;
    assert(save_file("/Fake/note.txt") == 0);
    id = vfs_lookup("/Fake/note.txt");
    assert(vfs_node(id)->size == 12 && vfs_lookup("/Fake/note.txt.tmp") < 0);

    /* path problems give a sentence and change nothing */
    assert(save_file("Fake/x.txt") != 0);
    assert(save_file("/Fake/") != 0);
    assert(save_file("/Nowhere/x.txt") != 0);
    assert(save_file("/RAM Disk/x.txt") != 0 && vfs_lookup("/RAM Disk/x.txt") < 0);
    assert(save_file("/x.txt") != 0);

    /* open: the text is replaced, undo history and state start over */
    put_file("a.txt", "one\r\ntwo\tthree\r\n", 16);
    assert(open_file("/Fake/a.txt") == 0);
    assert(is("one\ntwo    three\n") && !NP_DIRTY && NP_UN == 0 && NP_CUR == 0 && same_str(NP_PATH, "/Fake/a.txt"));
    undo();
    assert(is("one\ntwo    three\n")); /* nothing to undo after opening */

    /* open refusals keep the current text */
    put_file("bin.dat", "ab\x01" "cd", 5);
    put_file("utf.txt", "caf\xC3\xA9", 5);
    assert(open_file("/Fake/bin.dat") != 0 && open_file("/Fake/utf.txt") != 0);
    assert(open_file("/Fake/missing.txt") != 0 && open_file("/Fake") != 0);
    assert(open_file("/RAM Disk/readme.txt") != 0);
    assert(is("one\ntwo    three\n") && same_str(NP_PATH, "/Fake/a.txt"));

    /* a file of exactly the limit opens, one byte more does not; a Tab that grows it over the limit is refused */
    static char big[NP_MAX + 1];
    memset(big, 'a', sizeof big);
    put_file("limit.txt", big, NP_MAX);
    assert(open_file("/Fake/limit.txt") == 0 && NP_LEN == NP_MAX);
    put_file("over.txt", big, NP_MAX + 1);
    assert(open_file("/Fake/over.txt") != 0 && NP_LEN == NP_MAX);
    memset(big, 'a', NP_MAX);
    big[0] = '\t';
    put_file("tabs.txt", big, NP_MAX);
    assert(open_file("/Fake/tabs.txt") != 0 && NP_LEN == NP_MAX);

    /* a new document starts empty and untitled */
    new_document();
    assert(is("") && NP_PATH[0] == 0 && !NP_DIRTY && NP_UN == 0);
}

static void test_dialog(void)
{
    static Window DW;
    NP_DLG = &DW;
    NP_DLG_MODE = NP_DLG_OPEN;
    int fake = vfs_volume_node(1);
    assert(default_dir() == fake); /* the RAM Disk holds no files: the first real drive is the start */

    /* This PC lists the drives; a drive lists its files, sorted */
    dlg_enter(VFS_ROOT);
    assert(NP_DLG_N == 2 && NP_DLG_SEL == -1 && NP_DLG_TOP == 0);
    assert(same_str(vfs_node(NP_DLG_ITEMS[0])->name, "Fake") && same_str(vfs_node(NP_DLG_ITEMS[1])->name, "RAM Disk"));
    dlg_enter(fake);
    assert(NP_DLG_N == 7 && same_str(vfs_node(NP_DLG_ITEMS[0])->name, "a.txt"));

    /* the path of a name typed in the dialog */
    char p[UI_EDIT_MAX + 1];
    dlg_full_path("x.txt", p, (int)sizeof p);
    assert(same_str(p, "/Fake/x.txt"));
    dlg_full_path("/Fake/sub/y.txt", p, (int)sizeof p);
    assert(same_str(p, "/Fake/sub/y.txt"));
    NP_DLG_DIR = VFS_ROOT;
    dlg_full_path("x.txt", p, (int)sizeof p);
    assert(same_str(p, "/x.txt"));
    dlg_enter(fake);

    /* selecting rows: clamped, scrolls into view (the stub window shows 4 rows), a file's name fills the field */
    DlgLayout l = dlg_layout(wm_content(&DW));
    assert(dlg_rows(&l) == 4);
    dlg_select(6);
    assert(NP_DLG_SEL == 6 && NP_DLG_TOP == 3 && same_str(DW.edits[0].text, "utf.txt"));
    dlg_select(-5);
    assert(NP_DLG_SEL == 0 && NP_DLG_TOP == 0 && same_str(DW.edits[0].text, "a.txt"));
    dlg_select(99);
    assert(NP_DLG_SEL == 6);
    assert(dlg_wheel(&DW, -5) == 1 && NP_DLG_TOP == 0 && dlg_wheel(&DW, -1) == 0);
    assert(dlg_wheel(&DW, 5) == 1 && NP_DLG_TOP == 3 && dlg_wheel(&DW, 1) == 0); /* the last page: 7 rows - 4 */

    /* a single click selects; a second click on the same row soon after opens the file */
    dlg_enter(fake);
    NP_PATH[0] = 0;
    int row0 = l.list.y + 5, x = l.list.x + 30;
    NOW = 1000;
    assert(dlg_press(&DW, x, row0) == 1 && NP_DLG_SEL == 0 && NP_PATH[0] == 0);
    NOW = 2000; /* too late for a double click */
    assert(dlg_press(&DW, x, row0) == 1 && NP_PATH[0] == 0);
    NOW = 2100;
    assert(dlg_press(&DW, x, row0) == 1 && same_str(NP_PATH, "/Fake/a.txt") && is("one\ntwo    three\n"));

    /* a double click on a folder goes into it; "Up" goes back */
    dlg_enter(VFS_ROOT);
    NOW = 5000;
    assert(dlg_press(&DW, x, row0) == 1 && NP_DLG_DIR == VFS_ROOT);
    NOW = 5100;
    assert(dlg_press(&DW, x, row0) == 1 && NP_DLG_DIR == fake && NP_DLG_N == 7);
    on_dlg_up(0, 0);
    assert(NP_DLG_DIR == VFS_ROOT);
    on_dlg_up(0, 0); /* nothing above This PC */
    assert(NP_DLG_DIR == VFS_ROOT);

    /* a click outside the rows or below the last one does nothing */
    dlg_enter(VFS_ROOT);
    assert(dlg_press(&DW, x, l.list.y + 3 * DLG_ROW + 2) == 0);
    /* why a folder cannot take a file: This PC, the RAM Disk, and a locked drive each have their own sentence */
    assert(folder_problem(fake) == 0 && folder_problem(VFS_ROOT) != 0);
    assert(strstr(folder_problem(VFS_ROOT), "drive") != 0);
    assert(strstr(folder_problem(vfs_volume_node(0)), "RAM Disk") != 0);
    assert(volume_badge(vfs_volume_node(0)) != 0 && volume_badge(fake) == 0);
    FF_LOCKED = 1;
    assert(strstr(folder_problem(fake), "locked") != 0 && same_str(volume_badge(fake), "locked"));
    assert(default_dir() == fake); /* only a locked drive: still the best start, the dialog says why */
    NP_LEN = NP_CUR = NP_ANCHOR = 0;
    type("x");
    assert(strstr(save_file("/Fake/never.txt"), "locked") != 0 && vfs_lookup("/Fake/never.txt") < 0);
    assert(strstr(save_file("/RAM Disk/never.txt"), "RAM Disk") != 0);
    assert(strstr(save_file("/never.txt"), "drive") != 0);
    FF_LOCKED = 0;
    assert(save_file("/Fake/ok.txt") == 0);

    /* the eighth file filled the fake drive: the message names the file system, the step and the reason */
    const char *msg = save_file("/Fake/zzz.txt");
    assert(msg && same_str(msg, "FAKE: could not create the file (drive full)"));
    NP_DLG = 0;
}

/* the File Explorer opens a text file in Notepad (notepad_open_file) */
static void test_open_from_explorer(void)
{
    static Window NW;
    NP_WIN = &NW; /* Notepad is "open": notepad_app_open() only brings it to the front */
    NP_CONF = 0;
    new_document();
    assert(notepad_open_file("") != 0 && notepad_open_file(0) != 0);

    /* clean text: the file opens at once */
    assert(notepad_open_file("/Fake/ok.txt") == 0);
    assert(is("x") && same_str(NP_PATH, "/Fake/ok.txt") && !NP_DIRTY && same_str(NP_NOTE, "Opened"));

    /* a file that is not there: a reason, and the text stays */
    const char *err = notepad_open_file("/Fake/missing.txt");
    assert(err && same_str(err, "File not found") && is("x") && same_str(NP_PATH, "/Fake/ok.txt"));

    /* unsaved text: nothing happens until the user agrees; no window for the question = an error */
    NP_CUR = NP_ANCHOR = NP_LEN; /* opening leaves the caret at the start */
    type("yz");
    NP_DIRTY = 1;
    STUB_DIALOG = 0;
    assert(notepad_open_file("/Fake/a.txt") != 0 && is("xyz") && NP_DIRTY);
    STUB_DIALOG = 1;
    assert(notepad_open_file("/Fake/a.txt") == 0 && NP_CONF == &STUB_DLGW && is("xyz") && NP_DIRTY);
    on_conf_no(0, 0); /* Cancel: the text stays (the window itself is closed by the window manager) */
    assert(is("xyz") && NP_DIRTY);
    NP_CONF = &STUB_DLGW;
    on_conf_yes(0, 0); /* Discard: now the file opens */
    assert(is("one\ntwo    three\n") && same_str(NP_PATH, "/Fake/a.txt") && !NP_DIRTY);

    /* the answer applies to the newest request */
    NP_CUR = NP_ANCHOR = NP_LEN;
    type("q");
    NP_DIRTY = 1;
    assert(notepad_open_file("/Fake/ok.txt") == 0);
    assert(notepad_open_file("/Fake/a.txt") == 0); /* the question is already on screen: it only changes the target */
    NP_CONF = &STUB_DLGW;
    on_conf_yes(0, 0);
    assert(same_str(NP_PATH, "/Fake/a.txt"));

    NP_CONF = 0; /* a "New" question is on screen, then a file is asked for: the answer must open the file */
    NP_DIRTY = 1;
    cmd_new();
    assert(NP_CONF == &STUB_DLGW && NP_PENDING == NP_ACT_NEW);
    assert(notepad_open_file("/Fake/ok.txt") == 0 && NP_PENDING == NP_ACT_PATH);
    on_conf_yes(0, 0);
    assert(same_str(NP_PATH, "/Fake/ok.txt") && is("x"));

    /* Notepad is not open and no window can be made */
    NP_WIN = 0;
    STUB_DIALOG = 0;
    assert(notepad_open_file("/Fake/a.txt") != 0);
    NP_WIN = 0;
}

/* the close button / Alt+F4 with unsaved text (np_close_request) */
static void test_close_request(void)
{
    static Window NW;
    NP_WIN = &NW;
    NP_CONF = 0;
    STUB_DIALOG = 1;
    STUB_CLOSED = 0;
    new_document();
    assert(np_close_request(&NW) == 0);                       /* nothing unsaved: it closes */
    NP_DIRTY = 1;
    assert(np_close_request(&NW) == 1 && NP_CONF == &STUB_DLGW && NP_PENDING == NP_ACT_CLOSE);
    on_conf_no(0, 0);                                          /* Cancel: the window and the text stay */
    assert(NP_DIRTY && NP_WIN == &NW && STUB_CLOSED == &STUB_DLGW);
    STUB_CLOSED = 0;
    NP_CONF = &STUB_DLGW;
    on_conf_yes(0, 0);                                         /* Discard: now the window closes, without asking again */
    assert(!NP_DIRTY && STUB_CLOSED == &NW);
    NP_DIRTY = 1;                                              /* no window for the question: keep the text, say why */
    NP_CONF = 0;
    STUB_DIALOG = 0;
    NP_NOTE = "";
    assert(np_close_request(&NW) == 1 && NP_DIRTY && NP_NOTE[0]);
    NP_WIN = 0;
    NP_DIRTY = 0;
}

/* the "Save" answer of the unsaved-changes question */
static void test_save_answer(void)
{
    static Window NW;
    NP_WIN = &NW;
    NP_CONF = 0;
    NP_DLG = 0;
    NP_AFTER = -1;
    STUB_DIALOG = 1;
    STUB_CLOSED = 0;
    const char *gone[] = {"utf.txt", "bin.dat", "tabs.txt", "over.txt", "limit.txt"}; /* make room on the full fake drive */
    for (int i = 0; i < 5; i++)
    {
        int id = vfs_lookup(i == 0 ? "/Fake/utf.txt" : i == 1 ? "/Fake/bin.dat" : i == 2 ? "/Fake/tabs.txt" : i == 3 ? "/Fake/over.txt" : "/Fake/limit.txt");
        if (id > 0)
            vfs_remove(id);
        (void)gone;
    }

    /* a named file: Save writes it, then the file that was asked for opens */
    assert(open_file("/Fake/ok.txt") == 0);
    NP_CUR = NP_ANCHOR = NP_LEN;
    type("++");
    NP_DIRTY = 1;
    assert(notepad_open_file("/Fake/a.txt") == 0 && NP_CONF == &STUB_DLGW && NP_PENDING == NP_ACT_PATH);
    on_conf_save(0, 0);
    int ok = vfs_lookup("/Fake/ok.txt");
    unsigned char back[16] = {0};
    assert(ok > 0 && vfs_read(ok, 0, back, sizeof back) == 3 && memcmp(back, "x++", 3) == 0);
    assert(same_str(NP_PATH, "/Fake/a.txt") && is("one\ntwo    three\n") && !NP_DIRTY);

    /* saving fails: the message shows, the text stays, the file is not opened */
    NP_PATH[0] = 0;
    copy_str(NP_PATH, (int)sizeof NP_PATH, "/RAM Disk/x.txt");
    NP_CUR = NP_ANCHOR = NP_LEN;
    type("!");
    NP_DIRTY = 1;
    NP_CONF = 0;
    assert(notepad_open_file("/Fake/ok.txt") == 0);
    NP_NOTE = "";
    on_conf_save(0, 0);
    assert(NP_NOTE[0] && NP_DIRTY && is("one\ntwo    three\n!") && same_str(NP_PATH, "/RAM Disk/x.txt"));

    /* an untitled text: Save opens the Save As dialog; closing the window follows once it has saved */
    new_document();
    type("draft");
    NP_DIRTY = 1;
    NP_CONF = 0;
    STUB_CLOSED = 0;
    assert(np_close_request(&NW) == 1 && NP_PENDING == NP_ACT_CLOSE);
    on_conf_save(0, 0);
    assert(NP_DLG == &STUB_DLGW && NP_DLG_MODE == NP_DLG_SAVE && NP_AFTER == NP_ACT_CLOSE && STUB_CLOSED != &NW);
    dlg_enter(vfs_volume_node(1));
    ui_edit_set_text(&STUB_DLGW.edits[0], "draft.txt");
    dlg_ok();
    int d = vfs_lookup("/Fake/draft.txt");
    assert(d > 0 && vfs_node(d)->size == 5 && same_str(NP_PATH, "/Fake/draft.txt") && !NP_DIRTY);
    assert(STUB_CLOSED == &NW && NP_AFTER == -1);

    /* the same, but the dialog is cancelled: nothing follows */
    new_document();
    type("again");
    NP_DIRTY = 1;
    NP_CONF = 0;
    STUB_CLOSED = 0;
    assert(np_close_request(&NW) == 1);
    on_conf_save(0, 0);
    assert(NP_AFTER == NP_ACT_CLOSE);
    dlg_close(&STUB_DLGW); /* Cancel */
    assert(NP_AFTER == -1 && NP_DIRTY && STUB_CLOSED != &NW);

    /* no window for the Save As dialog: nothing is left pending */
    NP_CONF = 0;
    assert(np_close_request(&NW) == 1);
    STUB_DIALOG = 0;
    NP_DLG = 0;
    on_conf_save(0, 0);
    assert(NP_AFTER == -1 && NP_DIRTY);
    NP_WIN = 0;
    NP_DIRTY = 0;
    STUB_DIALOG = 0;
}

/* word wrap: how a line is cut into rows, and the caret, keys, clicks and the toggle working on rows */
static void set_text(const char *s)
{
    reset();
    NP_LEN = (int)strlen(s);
    memcpy(NP_BUF, s, (size_t)NP_LEN);
    NP_CUR = NP_ANCHOR = 0;
    NP_TOP = NP_HSCROLL = 0;
}

static void expect_rows(int n, const int *starts)
{
    Row r;
    row_first(&r);
    for (int i = 0; i < n; i++)
    {
        assert(r.start == starts[i]);
        assert(row_next(&r) == (i < n - 1));
    }
    assert(row_count() == n);
}

static void test_wrap(void)
{
    static Window NW;
    NP_WIN = &NW;
    NP_WRAP = 1;
    NP_WRAPW = 80; /* 10 characters of 8 pixels */

    /* a space that fits ends the row; the next row starts after it */
    set_text("hello world foo");
    expect_rows(2, (int[]){0, 6});
    Row r;
    assert(row_find(5, &r) == 0 && row_find(6, &r) == 1 && r.start == 6 && row_find(15, &r) == 1);
    row_first(&r);
    assert(row_soft(&r) && row_last_pos(&r) == 5);       /* the caret stays before the space that ends the row */
    /* a long word is cut inside; a word that ends exactly at the edge lets its space hang over */
    set_text("abcdefghijklmnopqrstuvwxyz");
    expect_rows(3, (int[]){0, 10, 20});
    set_text("abcdefghij klm");
    expect_rows(2, (int[]){0, 11});
    /* line breaks stay: empty lines are rows, a short line is one row */
    set_text("ab\n\ncd");
    expect_rows(3, (int[]){0, 3, 4});
    set_text("");
    expect_rows(1, (int[]){0});
    set_text("aaaa bbbb cccc\nz");
    expect_rows(3, (int[]){0, 10, 15});
    /* without wrap rows are lines again, whatever the width */
    NP_WRAP = 0;
    set_text("hello world foo bar baz");
    assert(row_count() == 1);
    set_text("one\ntwo\nthree");
    expect_rows(3, (int[]){0, 4, 8});
    NP_WRAP = 1;

    /* keys: the stub window is 400 wide, so a row is 45 characters; 30 words of 5 make rows of 9 words */
    NP_WRAPW = 0; /* np_key sets the real width itself */
    char text[151];
    for (int i = 0; i < 30; i++)
        memcpy(text + i * 5, "word ", 5);
    text[150] = 0;
    set_text(text);
    set_geometry(wm_content(&NW));
    assert(row_count() == 4);
    NP_CUR = NP_ANCHOR = 3;
    NP_DESIRED = 3;
    KeyEvent down = {KEY_DOWN, 0, 1, 0}, up = {KEY_UP, 0, 1, 0}, home = {KEY_HOME, 0, 1, 0}, end = {KEY_END, 0, 1, 0};
    np_key(&NW, &down);
    assert(NP_CUR == 48);                                /* the same column, one row down */
    np_key(&NW, &end);
    assert(NP_CUR == 89);                                /* before the space that ends the row, not at the next row's start */
    np_key(&NW, &home);
    assert(NP_CUR == 45);
    np_key(&NW, &up);
    assert(NP_CUR == 0);                                 /* column 0 of the row above (Home put the caret at column 0) */
    KeyEvent right = {KEY_RIGHT, 0, 1, 0};
    NP_CUR = NP_ANCHOR = 44;
    np_key(&NW, &right);
    assert(NP_CUR == 45 && col_in_row() == 0);           /* over the soft break onto the next row */
    KeyEvent pgdn = {KEY_PAGEDOWN, 0, 1, 0};
    NP_ROWS = 2;
    NP_CUR = NP_ANCHOR = 2;
    NP_DESIRED = 2;
    np_key(&NW, &pgdn);
    assert(NP_CUR == 92);

    /* a click right of a wrapped row puts the caret on that row's last position, not on the next row */
    set_text(text);
    set_geometry(wm_content(&NW));
    UiRect in = ui_inset(text_area(wm_content(&NW)), NP_PAD);
    assert(pos_at(in, in.x + 10000, in.y + 2) == 44);
    assert(pos_at(in, in.x + 10000, in.y + np_line_h() + 2) == 89);
    assert(pos_at(in, in.x + 3, in.y + np_line_h() + 2) == 45);
    assert(pos_at(in, in.x + 10000, in.y + 3 * np_line_h() + 2) == 150); /* the last row ends the text: the caret may be after its last character */
    NP_WRAP = 0;
    set_geometry(wm_content(&NW));
    assert(pos_at(in, in.x + 10000, in.y + 2) == 150);                  /* without wrap: the end of the line */
    NP_WRAP = 1;

    /* typing makes words move between rows; the row count follows */
    set_text("aaaa bbbb cccc dddd");
    set_geometry(wm_content(&NW));
    NP_WRAPW = 80;
    assert(row_count() == 2);
    NP_CUR = NP_ANCHOR = 0;
    insert_text("xxxxxx", 6);                           /* "xxxxxxaaaa bbbb cccc dddd": the last word no longer fits the second row */
    assert(row_count() == 3);
    undo();
    assert(row_count() == 2);

    /* the toggle: the first row of the view stays the same text, the button color follows */
    set_text(text);
    NP_WRAP = 0;
    NP_TOP = 0;
    NW.btn_count = 0;
    cmd_wrap();
    assert(NP_WRAP == 1 && NP_HSCROLL == 0 && NW.btn[NP_WRAP_BTN].kind == UI_BTN_PRIMARY);
    set_geometry(wm_content(&NW));
    NP_TOP = 2;                                          /* the third row starts at 90 */
    NP_HSCROLL = 0;
    cmd_wrap();
    assert(NP_WRAP == 0 && NP_TOP == 0 && NW.btn[NP_WRAP_BTN].kind == UI_BTN_SECONDARY);
    NP_WRAP = 0;
    NP_WIN = 0;
}

/* find and replace */
static void set_find(const char *s, const char *repl)
{
    ui_edit_set_text(&NP_FIND, s);
    ui_edit_set_text(&NP_REPL, repl);
}

static int sel_is(int a, int b)
{
    int x = 0, y = 0;
    return sel_range(&x, &y) && x == a && y == b;
}

static void test_find(void)
{
    static Window NW;
    NP_WIN = &NW;
    NP_WRAP = 0;
    NP_BAR = 0;
    NP_FOCUS = 0;
    NP_MATCH_CASE = 0;
    set_geometry(wm_content(&NW));
    const char *txt = "The quick brown fox. the QUICK fox. the end";
    set_text(txt);

    /* Find Next goes through the matches, wraps at the end; Prev goes back and wraps at the start */
    set_find("the", "");
    assert(do_find(0) && sel_is(0, 3) && same_str(NP_NOTE, "1 of 3"));
    assert(do_find(0) && sel_is(21, 24) && same_str(NP_NOTE, "2 of 3"));
    assert(do_find(0) && sel_is(36, 39) && same_str(NP_NOTE, "3 of 3"));
    assert(do_find(0) && sel_is(0, 3) && same_str(NP_NOTE, "1 of 3"));         /* wrapped */
    assert(do_find(1) && sel_is(36, 39) && same_str(NP_NOTE, "3 of 3"));       /* Prev from the first one wraps */
    assert(do_find(1) && sel_is(21, 24));
    /* match case */
    NP_MATCH_CASE = 1;
    set_text(txt);
    assert(do_find(0) && sel_is(21, 24) && same_str(NP_NOTE, "1 of 2"));
    NP_MATCH_CASE = 0;
    /* nothing found, nothing typed */
    set_find("zzz", "");
    set_text(txt);
    assert(!do_find(0) && same_str(NP_NOTE, "Not found") && !sel_range(&(int){0}, &(int){0}));
    set_find("", "");
    assert(!do_find(0) && same_str(NP_NOTE, "Type what to find"));

    /* Replace: the selected match is replaced, the next one is selected; undo takes it back */
    set_text(txt);
    set_find("the", "a");
    assert(do_find(0));                                  /* "The" at 0 */
    do_replace(&NW);
    assert(is("a quick brown fox. the QUICK fox. the end") && NP_DIRTY);
    assert(sel_is(19, 22));                              /* the next "the" is selected now */
    do_replace(&NW);
    do_replace(&NW);
    assert(is("a quick brown fox. a QUICK fox. a end"));
    assert(same_str(NP_NOTE, "Replaced - no more matches"));
    undo();
    assert(is("a quick brown fox. a QUICK fox. the end"));
    /* with no match selected Replace only finds */
    set_text(txt);
    NP_DIRTY = 0;
    do_replace(&NW);
    assert(is(txt) && sel_is(0, 3) && !NP_DIRTY);

    /* Replace All: every match, one undo step; counts non-overlapping matches */
    set_text(txt);
    set_find("fox", "cat");
    do_replace_all(&NW);
    assert(is("The quick brown cat. the QUICK cat. the end") && same_str(NP_NOTE, "2 replaced") && NP_DIRTY);
    assert(NP_CUR == 0 && !sel_range(&(int){0}, &(int){0}) && NP_FOCUS == 0);
    undo();
    assert(is(txt));
    redo();
    assert(is("The quick brown cat. the QUICK cat. the end"));
    undo();
    set_text("aaaa");
    set_find("aa", "b");
    do_replace_all(&NW);
    assert(is("bb") && same_str(NP_NOTE, "2 replaced"));
    set_text("a b a");
    set_find("a", "");
    do_replace_all(&NW);
    assert(is(" b ")); /* an empty replacement deletes the matches */
    set_text("abc");
    set_find("x", "y");
    do_replace_all(&NW);
    assert(is("abc") && same_str(NP_NOTE, "Not found"));
    /* the result must fit */
    NP_LEN = NP_MAX - 1;
    memset(NP_BUF, 'a', (size_t)NP_LEN);
    NP_CUR = NP_ANCHOR = 0;
    set_find("a", "bb");
    do_replace_all(&NW);
    assert(NP_LEN == NP_MAX - 1 && NP_BUF[0] == 'a' && strstr(NP_NOTE, "too long"));
    set_text("one two");
    set_find("one", "a much longer replacement");
    assert(do_find(0));
    NP_LEN = NP_MAX - 2;                                 /* pretend the buffer is nearly full */
    memset(NP_BUF + 7, 'x', (size_t)(NP_LEN - 7));
    do_replace(&NW);
    assert(strstr(NP_NOTE, "too long") && NP_BUF[0] == 'o');

    /* the bar: open, prefill from a selection, keys go to the fields */
    set_text(txt);
    NP_ANCHOR = 4;
    NP_CUR = 9;                                          /* "quick" */
    KeyEvent ctrl_f = {'f', 0, 1, KMOD_CTRL}, esc = {KEY_ESC, 0, 1, 0}, enter = {KEY_ENTER, 0, 1, 0}, tab = {KEY_TAB, 0, 1, 0};
    KeyEvent f3 = {KEY_F3, 0, 1, 0}, sf3 = {KEY_F3, 0, 1, KMOD_SHIFT}, ctrl_h = {'h', 0, 1, KMOD_CTRL};
    UiRect ta_closed = text_area(wm_content(&NW));
    assert(find_key(&NW, &ctrl_f) == 2 && NP_BAR && NP_FOCUS == 1 && same_str(NP_FIND.text, "quick"));
    assert(text_area(wm_content(&NW)).h == ta_closed.h - NP_BAR_H);          /* the text area gives room to the bar */
    assert(find_key(&NW, &enter) == 1 && sel_is(25, 30));                      /* Enter: the next match after the selection */
    assert(find_key(&NW, &enter) == 1 && sel_is(4, 9));                        /* ... wrapping to the top */
    assert(find_key(&NW, &(KeyEvent){KEY_ENTER, 0, 1, KMOD_SHIFT}) == 1 && sel_is(25, 30));  /* Shift+Enter: back, wrapping to the end */
    assert(find_key(&NW, &tab) == 1 && NP_FOCUS == 2 && find_key(&NW, &tab) == 1 && NP_FOCUS == 1);
    /* typing goes to the field, never to the text */
    int len = NP_LEN;
    np_key(&NW, &(KeyEvent){'z', 'z', 1, 0});
    assert(NP_LEN == len && same_str(NP_FIND.text, "quickz"));
    np_key(&NW, &(KeyEvent){KEY_BACKSPACE, 0, 1, 0});
    assert(same_str(NP_FIND.text, "quick"));
    assert(find_key(&NW, &(KeyEvent){'z', 0, 1, KMOD_CTRL}) == 1);             /* Ctrl+Z is the field's: the text is not undone */
    assert(find_key(&NW, &ctrl_h) == 2 && NP_FOCUS == 2);
    assert(find_key(&NW, &esc) == 2 && !NP_BAR && NP_FOCUS == 0);
    assert(text_area(wm_content(&NW)).h == ta_closed.h);
    /* F3 / Shift+F3 work with the bar closed; with no pattern they open it */
    NP_ANCHOR = NP_CUR = 0;
    assert(find_key(&NW, &f3) == 1 && sel_is(4, 9));
    assert(find_key(&NW, &sf3) == 1 && sel_is(25, 30));
    set_find("", "");
    assert(find_key(&NW, &f3) == 2 && NP_BAR);
    find_key(&NW, &esc);

    /* the mouse: the buttons of the bar */
    set_find("quick", "slow");
    set_text(txt);
    bar_open(&NW, 1);
    BarLayout l = bar_layout(wm_content(&NW));
    #define CLICK(r) bar_press(&NW, (r).x + 2, (r).y + 2)
    assert(CLICK(l.next) == 1 && sel_is(4, 9));
    assert(CLICK(l.next) == 1 && sel_is(25, 30));
    assert(CLICK(l.prev) == 1 && sel_is(4, 9));
    assert(CLICK(l.repl) == 1 && is("The slow brown fox. the QUICK fox. the end") && sel_is(24, 29));
    assert(CLICK(l.mcase) == 1 && NP_MATCH_CASE);
    assert(CLICK(l.mcase) == 1 && !NP_MATCH_CASE);
    assert(CLICK(l.all) == 1 && is("The slow brown fox. the slow fox. the end") && NP_FOCUS == 0);
    assert(CLICK(l.repl_f) == 1 && NP_FOCUS == 2 && NP_BAR_DRAG);
    assert(CLICK(l.find_f) == 1 && NP_FOCUS == 1);
    assert(bar_press(&NW, l.bar.x + 2, l.bar.y + l.bar.h + 20) == 0);          /* below the bar: not a bar click */
    assert(CLICK(l.close) == 1 && !NP_BAR);
    assert(bar_press(&NW, l.next.x + 2, l.next.y + 2) == 0);                   /* the bar is gone: nothing to click */
    #undef CLICK
    NP_WIN = 0;
}

/* closing the window forgets everything: "Don't save" and then opening Notepad again must start empty */
static void test_close_clears(void)
{
    static Window NW;
    NW.on_close = np_close; /* like the window made by notepad_app_open */
    NP_WIN = &NW;
    NP_CONF = 0;
    NP_DLG = 0;
    STUB_DIALOG = 1;
    reset();
    NP_PATH[0] = 0;
    copy_str(NP_PATH, (int)sizeof NP_PATH, "/Fake/a.txt");
    type("unsaved words");
    NP_DIRTY = 1;
    assert(np_close_request(&NW) == 1 && NP_PENDING == NP_ACT_CLOSE);
    on_conf_yes(0, 0);                                         /* "Don't save" */
    assert(NP_WIN == 0 && is("") && NP_LEN == 0 && NP_PATH[0] == 0 && !NP_DIRTY && NP_UN == 0 && NP_UPOS == 0);
    assert(NP_CUR == 0 && NP_ANCHOR == 0 && NP_TOP == 0 && !NP_BAR && NP_FOCUS == 0);
    /* a saved text is forgotten too (the window is closed with no question) */
    NP_WIN = &NW;
    type("saved text");
    copy_str(NP_PATH, (int)sizeof NP_PATH, "/Fake/a.txt");
    NP_DIRTY = 0;
    assert(np_close_request(&NW) == 0);
    wm_close(&NW);
    assert(is("") && NP_PATH[0] == 0);
    /* the find bar is closed with the window; a pending "after saving" action is dropped */
    NP_WIN = &NW;
    NP_BAR = 1;
    NP_FOCUS = 1;
    NP_AFTER = NP_ACT_CLOSE;
    wm_close(&NW);
    assert(!NP_BAR && NP_FOCUS == 0 && NP_AFTER == -1);
    STUB_DIALOG = 0;
    NP_WIN = 0;
}

/* ---- helpers ---- */
static void reset(void)
{
    NP_LEN = NP_CUR = NP_ANCHOR = 0;
    NP_UN = NP_UPOS = 0;
    NP_LASTKIND = NP_KIND_NONE;
    NP_EDITS = 0;
}

static void type(const char *s)
{
    for (; *s; s++)
        insert_text(s, 1);
}

static int is(const char *expect)
{
    return NP_LEN == (int)strlen(expect) && memcmp(NP_BUF, expect, (size_t)NP_LEN) == 0;
}

static void end_run(void) { NP_LASTKIND = NP_KIND_NONE; } /* what a movement key or a click does */

int main(void)
{
    /* typing in a row is one undo step */
    reset();
    type("hello");
    assert(is("hello") && NP_UN == 1);
    undo();
    assert(is("") && NP_CUR == 0);
    redo();
    assert(is("hello") && NP_CUR == 5);

    /* a movement in between starts a new step; Enter is a step of its own */
    reset();
    type("ab");
    end_run();
    type("cd");
    insert_text("\n", 1);
    type("e");
    assert(is("abcd\ne") && NP_UN == 4);
    undo();
    assert(is("abcd\n"));
    undo();
    assert(is("abcd"));
    undo();
    assert(is("ab"));
    undo();
    assert(is(""));
    undo(); /* nothing left: harmless */
    assert(is("") && NP_UPOS == 0);

    /* Backspace in a row is one step, and undo puts the text back in the right order */
    reset();
    type("abcdef");
    end_run();
    for (int i = 0; i < 3; i++)
        edit_replace(NP_CUR - 1, 1, 0, 0, NP_KIND_BACK);
    assert(is("abc") && NP_UN == 2);
    undo();
    assert(is("abcdef") && NP_CUR == 6);
    redo();
    assert(is("abc") && NP_CUR == 3);

    /* Delete in a row, from the middle */
    reset();
    type("abcdef");
    NP_CUR = NP_ANCHOR = 2;
    end_run();
    for (int i = 0; i < 3; i++)
        edit_replace(NP_CUR, 1, 0, 0, NP_KIND_FWD);
    assert(is("abf") && NP_UN == 2);
    undo();
    assert(is("abcdef") && NP_CUR == 2);
    redo();
    assert(is("abf") && NP_CUR == 2);

    /* typing over a selection is one step; undo selects the old text again */
    reset();
    type("hello world");
    NP_ANCHOR = 6;
    NP_CUR = 11;
    end_run();
    insert_text("x", 1);
    assert(is("hello x") && NP_CUR == 7 && NP_ANCHOR == 7);
    undo();
    assert(is("hello world") && NP_ANCHOR == 6 && NP_CUR == 11);
    redo();
    assert(is("hello x"));

    /* cut and paste, with Tab and carriage return in the clipboard */
    reset();
    type("one two");
    NP_ANCHOR = 0;
    NP_CUR = 3;
    copy_selection();
    assert(CLIP_N == 3 && memcmp(CLIP_STORE, "one", 3) == 0);
    delete_selection();
    assert(is(" two"));
    undo();
    assert(is("one two"));
    CLIP_N = 5;
    memcpy(CLIP_STORE, "a\tb\r\n", 5);
    NP_CUR = NP_ANCHOR = 7;
    end_run();
    paste();
    assert(is("one twoa    b\n"));
    undo();
    assert(is("one two"));

    /* a new edit ends the redo history */
    reset();
    type("abc");
    end_run();
    type("def");
    undo();
    assert(is("abc") && NP_UPOS == 1 && NP_UN == 2);
    type("X");
    assert(is("abcX") && NP_UN == 2 && NP_UPOS == 2);
    redo(); /* nothing to redo */
    assert(is("abcX"));

    /* the buffer limit: a paste is cut to what fits and still undoes cleanly */
    reset();
    for (int i = 0; i < NP_MAX - 2; i++)
        NP_BUF[i] = 'a';
    NP_LEN = NP_MAX - 2;
    NP_CUR = NP_ANCHOR = NP_LEN;
    insert_text("12345", 5);
    assert(NP_LEN == NP_MAX && NP_BUF[NP_MAX - 1] == '2');
    undo();
    assert(NP_LEN == NP_MAX - 2);

    /* random edits: undo everything gives the empty text, redo everything gives the final text */
    unsigned seed = 12345;
    for (int round = 0; round < 200; round++)
    {
        reset();
        int steps = 20 + (int)(seed % 60);
        for (int i = 0; i < steps; i++)
        {
            seed = seed * 1103515245u + 12345u;
            int r = (int)((seed >> 16) % 9);
            if (r < 4)
            {
                char c = (char)('a' + (seed >> 8) % 26);
                insert_text(&c, 1);
            }
            else if (r == 4 && NP_CUR > 0)
                edit_replace(NP_CUR - 1, 1, 0, 0, NP_KIND_BACK);
            else if (r == 5 && NP_CUR < NP_LEN)
                edit_replace(NP_CUR, 1, 0, 0, NP_KIND_FWD);
            else if (r == 6)
                insert_text("\n", 1);
            else if (r == 7)
            {
                NP_CUR = NP_ANCHOR = NP_LEN ? (int)((seed >> 4) % (unsigned)(NP_LEN + 1)) : 0;
                end_run();
            }
            else if (r == 8 && NP_LEN > 2)
            {
                NP_ANCHOR = (int)((seed >> 3) % (unsigned)NP_LEN);
                NP_CUR = (int)((seed >> 9) % (unsigned)NP_LEN);
                insert_text("zz", 2);
            }
        }
        static char final_text[NP_MAX + 1];
        int final_len = NP_LEN;
        memcpy(final_text, NP_BUF, (size_t)NP_LEN);
        while (NP_UPOS > 0)
            undo();
        assert(NP_LEN == 0);
        while (NP_UPOS < NP_UN)
            redo();
        assert(NP_LEN == final_len && memcmp(NP_BUF, final_text, (size_t)final_len) == 0);
    }

    /* more edits than records: the oldest are forgotten, the newest still undo */
    reset();
    for (int i = 0; i < NP_UNDO_OPS + 50; i++)
    {
        insert_text("\n", 1);
        if (NP_LEN > 3000)
            NP_LEN = NP_CUR = NP_ANCHOR = 0, NP_UN = NP_UPOS = 0;
    }
    assert(NP_UN <= NP_UNDO_OPS);
    int len_now = NP_LEN;
    undo();
    assert(NP_LEN == len_now - 1);
    redo();
    assert(NP_LEN == len_now);

    test_files();
    test_dialog();
    test_open_from_explorer();
    test_close_request();
    test_save_answer();
    test_wrap();
    test_find();
    test_close_clears();

    printf("notepad tests passed\n");
    return 0;
}
