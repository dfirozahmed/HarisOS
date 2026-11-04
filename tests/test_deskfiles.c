/* Host-side tests for the Desktop folder (kernel/desktop/deskfiles.c): finding it, noticing changes, what a double click opens, deleting and
 * the question before it. The file system, the apps and the windows are stubs. */
#include "deskfiles.h"
#include "deskicons.h"
#include "console.h"
#include "exec.h"
#include "fileexplorer.h"
#include "look.h"
#include "notepad.h"
#include "taskbar.h"
#include "timer.h"
#include "toast.h"
#include "vfs.h"
#include "wm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- a tiny file system: nodes in the order a folder lists them (folders first, then by name, as the real vfs_list does) ---- */
#define MAXN 64
static VfsNode NODES[MAXN];
static void node_set(int id, int parent, const char *name, int is_dir)
{
    memset(&NODES[id], 0, sizeof NODES[id]);
    NODES[id].used = 1;
    NODES[id].parent = parent;
    NODES[id].is_dir = is_dir;
    strcpy(NODES[id].name, name);
}
static void node_del(int id) { NODES[id].used = 0; }
static int VOL_FOLDER = 1; /* what look_data_folder says */
int look_data_folder(void) { return VOL_FOLDER; }
const VfsNode *vfs_node(int id) { return id > 0 && id < MAXN && NODES[id].used ? &NODES[id] : 0; }
int vfs_list(int dir, int *out, int max)
{
    int total = 0;
    for (int pass = 0; pass < 2; pass++) /* folders first */
        for (int i = 1; i < MAXN; i++)
            if (NODES[i].used && NODES[i].parent == dir && NODES[i].is_dir == (pass == 0))
            {
                if (total < max)
                    out[total] = i;
                total++;
            }
    return total;
}
/* the rest of what "Send to desktop" uses */
static int ADDS, ADD_OK = 1, ADDED_PARENT = -1, COPIES, COPY_OK = 1, COPIED = -1, COPY_DEST = -1;
static char ADDED_NAME[VFS_NAME_MAX];
static int free_id(void)
{
    for (int i = 22; i < MAXN; i++)
        if (!NODES[i].used)
            return i;
    return -1;
}
int vfs_add(int parent, const char *name, int is_dir, unsigned size)
{
    (void)size;
    ADDS++;
    ADDED_PARENT = parent;
    strcpy(ADDED_NAME, name);
    if (!ADD_OK)
        return -1;
    int id = free_id();
    node_set(id, parent, name, is_dir);
    return id;
}
static unsigned COPY_FAIL_MASK, COPY_CALL; /* bit k set: the k-th copy since COPY_CALL was reset fails */
int vfs_copy(int id, int dest)
{
    COPIES++;
    COPIED = id;
    COPY_DEST = dest;
    if (!COPY_OK || ((COPY_FAIL_MASK >> COPY_CALL++) & 1u))
        return -1;
    int c = free_id();
    node_set(c, dest, NODES[id].name, NODES[id].is_dir);
    return c;
}
int vfs_is_under(int id, int ancestor)
{
    for (int i = id; i > 0; i = NODES[i].parent)
        if (i == ancestor)
            return 1;
    return 0;
}
int vfs_is_volume_root(int id) { return id == 1; } /* the boot drive */
static int REFRESHES;
int vfs_refresh(int dir) { (void)dir; REFRESHES++; return 0; }
int vfs_path(int id, char *out, int cap)
{
    char tmp[256] = "";
    int chain[8], n = 0;
    for (int i = id; i > 0 && n < 8; i = NODES[i].parent)
        chain[n++] = i;
    for (int k = n - 1; k >= 0; k--)
    {
        strcat(tmp, "/");
        strcat(tmp, NODES[chain[k]].name);
    }
    strncpy(out, tmp, (size_t)cap - 1);
    out[cap - 1] = 0;
    return (int)strlen(out);
}
static int REMOVE_RESULT;
static int REMOVED_NODE = -1;
int vfs_remove(int id)
{
    REMOVED_NODE = id;
    if (REMOVE_RESULT == 0)
        node_del(id);
    return REMOVE_RESULT;
}
int vfs_last_error(void) { return VFS_ERR_RO; }
const char *vfs_error_text(int code) { (void)code; return "This drive is locked (read-only)."; }

/* ---- the rest ---- */
static int SET_CALLS, SET_N, SET_FOUND;
static DeskFile SET_FILES[DESK_FILES_MAX];
void deskicons_set_files(const DeskFile *f, int n, int found)
{
    SET_CALLS++;
    SET_N = n;
    SET_FOUND = found;
    for (int i = 0; i < n; i++)
        SET_FILES[i] = f[i];
}
static int TOASTS;
static ToastKind LAST_KIND;
static char LAST_TEXT[TOAST_TEXT_MAX + 1];
void toast_show(ToastKind k, const char *t) { TOASTS++; LAST_KIND = k; strncpy(LAST_TEXT, t, TOAST_TEXT_MAX); }
static unsigned long long NOW;
unsigned long long timer_ms(void) { return NOW; }
static int EXPLORER_NODE = -1;
void fileexplorer_open_folder(int node) { EXPLORER_NODE = node; }
static char NOTEPAD_PATH[256];
static const char *NOTEPAD_REASON;
const char *notepad_open_file(const char *path) { strcpy(NOTEPAD_PATH, path); return NOTEPAD_REASON; }
static int CON_RESULT = 7, RUN_RESULT = 3, CON_WRITES, CON_FINISHED;
static char CON_TITLE[96], RUN_NAME[96], CON_TEXT[160];
int console_window_open(const char *title) { strcpy(CON_TITLE, title); return CON_RESULT; }
int console_write(int con, const char *s, unsigned n) { (void)con; CON_WRITES++; if (n < 150) { strncat(CON_TEXT, s, n); } return (int)n; }
void console_finish(int con, int code) { (void)con; (void)code; CON_FINISHED++; }
int exec_run_node(int node, const char *name, int console, int *err) { (void)node; (void)console; *err = 0; strcpy(RUN_NAME, name); return RUN_RESULT; }
const char *exec_error_text(int code, int err) { (void)code; (void)err; return "no memory for it"; }

/* windows: the question is one window with two buttons */
static Window WIN;
static int WINS_MADE, WINS_CLOSED;
static Window *WIN_RESULT = &WIN;
static const char *WIN_TITLE;
static int BTN_N;
static struct { char label[16]; UiButtonFn fn; void *user; } BTN[4];
Window *wm_create(const char *title, int x, int y, int w, int h, WmDrawFn draw, void *user)
{
    (void)x; (void)y; (void)w; (void)h; (void)user;
    if (!WIN_RESULT)
        return 0;
    memset(&WIN, 0, sizeof WIN);
    WIN.on_draw = draw;
    WIN_TITLE = title;
    WINS_MADE++;
    BTN_N = 0;
    return &WIN;
}
void wm_close(Window *w)
{
    WINS_CLOSED++;
    if (w->on_close)
        w->on_close(w);
}
void wm_set_resizable(Window *w, int on) { (void)w; (void)on; }
int wm_add_button_ex(Window *w, const char *label, UiButtonKind kind, UiRect rel, int anchor, UiButtonFn fn, void *user)
{
    (void)w; (void)kind; (void)rel; (void)anchor;
    strcpy(BTN[BTN_N].label, label);
    BTN[BTN_N].fn = fn;
    BTN[BTN_N].user = user;
    return BTN_N++;
}
static UiTheme THEME_STORE;
const UiTheme *ui_theme(void) { THEME_STORE.titlebar_h = 40; return &THEME_STORE; }
UiRect ui_rect(int x, int y, int w, int h) { UiRect r = {x, y, w, h}; return r; }
void ui_text_in(UiRect r, const char *s, UiTextStyle st, UiAlign a) { (void)r; (void)s; (void)st; (void)a; }
void ui_text_in_color(UiRect r, const char *s, UiTextStyle st, UiAlign a, UiRgb c) { (void)r; (void)s; (void)st; (void)a; (void)c; }
unsigned gfx_width(void) { return 1280; }
unsigned gfx_height(void) { return 800; }

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void reset_counters(void)
{
    SET_CALLS = 0;
    TOASTS = 0;
    EXPLORER_NODE = -1;
    NOTEPAD_PATH[0] = 0;
    NOTEPAD_REASON = 0;
    CON_TITLE[0] = RUN_NAME[0] = CON_TEXT[0] = 0;
    CON_WRITES = CON_FINISHED = 0;
    CON_RESULT = 7;
    RUN_RESULT = 3;
    REMOVED_NODE = -1;
    REMOVE_RESULT = 0;
}

int main(void)
{
    /* no drive, or no "Desktop" in its top folder: nothing to show, and deskicons is told that there is no folder */
    VOL_FOLDER = -1;
    CHECK(deskfiles_folder() == -1);
    deskfiles_scan(1);
    CHECK(SET_CALLS == 1 && SET_N == 0 && SET_FOUND == 0);
    VOL_FOLDER = 1;
    node_set(1, 0, "HARISOS", 1);
    node_set(2, 1, "THEME.CFG", 0);
    node_set(3, 1, "Desktop.txt", 0); /* a file of that name is not the folder */
    CHECK(deskfiles_folder() == -1);
    node_set(4, 1, "desktop", 0);
    CHECK(deskfiles_folder() == -1);

    /* the folder (any capitals) with files in it: they are listed, folders first, and the folder is read from the drive when asked */
    node_set(5, 1, "DESKTOP", 1);
    node_set(6, 5, "notes.txt", 0);
    node_set(7, 5, "Stuff", 1);
    node_set(8, 5, "run.ELF", 0);
    CHECK(deskfiles_folder() == 5);
    REFRESHES = 0;
    deskfiles_scan(0);
    CHECK(REFRESHES == 0 && SET_CALLS == 2 && SET_FOUND == 1 && SET_N == 3);
    CHECK(SET_FILES[0].node == 7 && SET_FILES[0].is_dir == 1 && strcmp(SET_FILES[0].name, "Stuff") == 0); /* the folder first */
    CHECK(SET_FILES[1].node == 6 && SET_FILES[1].is_dir == 0 && strcmp(SET_FILES[1].name, "notes.txt") == 0);
    CHECK(SET_FILES[2].node == 8 && strcmp(SET_FILES[2].name, "run.ELF") == 0);
    deskfiles_scan(1);
    CHECK(REFRESHES == 1 && SET_CALLS == 3);

    /* the folder is looked at about once a second; a change is told at once, no change is not told */
    SET_CALLS = 0;
    NOW = 10000;
    CHECK(deskfiles_tick() == 0 && SET_CALLS == 0); /* the same as the last scan */
    node_set(9, 5, "readme.md", 0);
    NOW = 10500;
    CHECK(deskfiles_tick() == 0 && SET_CALLS == 0); /* too soon */
    NOW = 11000;
    CHECK(deskfiles_tick() == 1 && SET_CALLS == 1 && SET_N == 4);
    NOW = 12000;
    CHECK(deskfiles_tick() == 0 && SET_CALLS == 1);
    node_del(6);
    NOW = 13000;
    CHECK(deskfiles_tick() == 1 && SET_N == 3);
    node_set(6, 5, "notes.txt", 0); /* a file deleted and made again with the same name and node: the same list */
    NOW = 14000;
    CHECK(deskfiles_tick() == 1 && SET_N == 4);
    node_set(6, 5, "other.txt", 0); /* the same node, another name: that is a change */
    NOW = 15000;
    CHECK(deskfiles_tick() == 1);
    node_del(5); /* the Desktop folder itself goes, and what was in it */
    for (int i = 1; i < MAXN; i++)
        if (NODES[i].parent == 5)
            node_del(i);
    NOW = 16000;
    CHECK(deskfiles_tick() == 1 && SET_N == 0 && SET_FOUND == 0);
    node_set(5, 1, "Desktop", 1);
    node_set(6, 5, "notes.txt", 0);
    node_set(7, 5, "Stuff", 1);
    NOW = 17000;
    CHECK(deskfiles_tick() == 1 && SET_FOUND == 1 && SET_N == 2);

    /* what a double click does, by what the item is */
    reset_counters();
    deskfiles_open(7); /* Stuff: a folder */
    CHECK(EXPLORER_NODE == 7 && TOASTS == 0);
    deskfiles_open(6); /* notes.txt */
    CHECK(strcmp(NOTEPAD_PATH, "/HARISOS/Desktop/notes.txt") == 0 && TOASTS == 0);
    node_set(10, 5, "README.TXT", 0);
    node_set(11, 5, "main.c", 0);
    node_set(12, 5, "boot.asm", 0);
    NOTEPAD_PATH[0] = 0;
    deskfiles_open(10);
    CHECK(strcmp(NOTEPAD_PATH, "/HARISOS/Desktop/README.TXT") == 0); /* capitals do not matter */
    deskfiles_open(11);
    CHECK(strcmp(NOTEPAD_PATH, "/HARISOS/Desktop/main.c") == 0);
    deskfiles_open(12);
    CHECK(strcmp(NOTEPAD_PATH, "/HARISOS/Desktop/boot.asm") == 0);
    NOTEPAD_REASON = "Notepad says no.";
    deskfiles_open(6);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_WARNING && strcmp(LAST_TEXT, "Notepad says no.") == 0);
    reset_counters();
    node_set(13, 5, "photo.png", 0);
    node_set(14, 5, "weird", 0);
    node_set(15, 5, ".txt", 0);
    deskfiles_open(13);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_INFO && strstr(LAST_TEXT, "No app can open") != 0);
    deskfiles_open(14);
    deskfiles_open(15);
    CHECK(TOASTS == 3 && NOTEPAD_PATH[0] == 0 && EXPLORER_NODE == -1);

    /* a program (.elf) runs in a console window, named after it */
    reset_counters();
    node_set(16, 5, "hello.elf", 0);
    deskfiles_open(16);
    CHECK(strcmp(CON_TITLE, "hello.elf") == 0 && strcmp(RUN_NAME, "hello.elf") == 0 && TOASTS == 0 && CON_WRITES == 0);
    CON_RESULT = -1; /* no console or window to run it in */
    deskfiles_open(16);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_ERROR && strstr(LAST_TEXT, "no free console") != 0);
    CON_RESULT = 7;
    RUN_RESULT = -1; /* it did not start: the reason goes to the window (which stays) and to a toast */
    deskfiles_open(16);
    CHECK(TOASTS == 2 && LAST_KIND == TOAST_ERROR && strstr(LAST_TEXT, "Cannot run: no memory for it.") != 0);
    CHECK(CON_WRITES == 2 && CON_FINISHED == 1 && strstr(CON_TEXT, "Cannot run: no memory for it.") != 0);

    /* an item that is gone: a toast, and the icons are brought up to date */
    reset_counters();
    deskfiles_open(30);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_WARNING && SET_CALLS == 1);

    /* delete: it goes, a toast says so, and the icons follow */
    reset_counters();
    deskfiles_delete(6);
    CHECK(REMOVED_NODE == 6 && TOASTS == 1 && LAST_KIND == TOAST_SUCCESS && strcmp(LAST_TEXT, "Deleted notes.txt") == 0 && SET_CALLS == 1);
    CHECK(vfs_node(6) == 0);
    reset_counters();
    REMOVE_RESULT = -1; /* the drive is locked: nothing goes, and the reason is the drive's own words */
    deskfiles_delete(10);
    CHECK(REMOVED_NODE == 10 && TOASTS == 1 && LAST_KIND == TOAST_ERROR && strstr(LAST_TEXT, "locked") != 0);
    CHECK(vfs_node(10) != 0 && SET_CALLS == 1); /* it looked again anyway: some of a folder may be gone */
    reset_counters();
    deskfiles_delete(31); /* no such node */
    CHECK(REMOVED_NODE == -1 && TOASTS == 0 && SET_CALLS == 1);

    /* the question: a window with the name and two buttons; Cancel only closes it, Delete deletes and closes it */
    reset_counters();
    WINS_MADE = WINS_CLOSED = 0;
    deskfiles_ask_delete(10);
    CHECK(WINS_MADE == 1 && strcmp(WIN_TITLE, "Delete") == 0 && BTN_N == 2);
    CHECK(strcmp(BTN[0].label, "Cancel") == 0 && strcmp(BTN[1].label, "Delete") == 0);
    CHECK(REMOVED_NODE == -1); /* asking deletes nothing */
    BTN[0].fn(0, BTN[0].user);
    CHECK(WINS_CLOSED == 1 && REMOVED_NODE == -1 && TOASTS == 0);
    deskfiles_ask_delete(10);
    BTN[1].fn(0, BTN[1].user);
    CHECK(WINS_CLOSED == 2 && REMOVED_NODE == 10 && TOASTS == 1 && LAST_KIND == TOAST_SUCCESS);

    /* asking again replaces the question; Esc cancels; Enter deletes; the other keys and releases do nothing */
    reset_counters();
    WINS_MADE = WINS_CLOSED = 0;
    node_set(17, 5, "a.txt", 0);
    node_set(18, 5, "b.txt", 0);
    deskfiles_ask_delete(17);
    deskfiles_ask_delete(18);
    CHECK(WINS_MADE == 2 && WINS_CLOSED == 1); /* the first question was closed */
    KeyEvent k = {'x', 0, 1, 0};
    WIN.on_key(&WIN, &k);
    k.key = KEY_ENTER;
    k.pressed = 0;
    WIN.on_key(&WIN, &k);
    CHECK(REMOVED_NODE == -1 && WINS_CLOSED == 1);
    k.pressed = 1;
    k.key = KEY_ESC;
    WIN.on_key(&WIN, &k);
    CHECK(WINS_CLOSED == 2 && REMOVED_NODE == -1);
    deskfiles_ask_delete(18);
    k.key = KEY_ENTER;
    WIN.on_key(&WIN, &k);
    CHECK(WINS_CLOSED == 3 && REMOVED_NODE == 18);

    /* no window for the question: a toast says so and nothing is deleted */
    reset_counters();
    WIN_RESULT = 0;
    node_set(19, 5, "c.txt", 0);
    deskfiles_ask_delete(19);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_ERROR && REMOVED_NODE == -1);
    WIN_RESULT = &WIN;
    deskfiles_ask_delete(30); /* a node that is gone */
    CHECK(REMOVED_NODE == -1);

    /* ================= Send to desktop ================= */
    node_set(20, 1, "Docs", 1);
    node_set(21, 20, "report.txt", 0);
    reset_counters();
    ADDS = COPIES = 0;
    COPY_OK = ADD_OK = 1;
    int desk = deskfiles_folder();
    CHECK(desk == 5);

    /* a copy goes into the Desktop folder, a toast says so, and the icons are told */
    int c = deskfiles_send(21);
    CHECK(COPIES == 1 && COPIED == 21 && COPY_DEST == 5 && ADDS == 0);
    CHECK(c > 0 && vfs_node(c) && vfs_node(c)->parent == 5 && strcmp(vfs_node(c)->name, "report.txt") == 0);
    CHECK(vfs_node(21) != 0 && vfs_node(21)->parent == 20); /* the original stays where it was */
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_SUCCESS && strcmp(LAST_TEXT, "Sent report.txt to the desktop.") == 0);
    CHECK(SET_CALLS == 1 && SET_FOUND == 1);
    int has_it = 0;
    for (int i = 0; i < SET_N; i++)
        has_it |= strcmp(SET_FILES[i].name, "report.txt") == 0;
    CHECK(has_it); /* the new icon is in what deskicons was told */

    /* a folder goes the same way */
    reset_counters();
    COPIES = 0;
    c = deskfiles_send(20);
    CHECK(COPIES == 1 && COPIED == 20 && vfs_node(c)->is_dir && TOASTS == 1 && LAST_KIND == TOAST_SUCCESS);

    /* something that is on the desktop already (or the Desktop folder itself) is not copied */
    reset_counters();
    COPIES = 0;
    CHECK(vfs_node(11) && vfs_node(11)->parent == 5); /* main.c: made earlier in this test, still there */
    CHECK(deskfiles_send(11) == -1);
    CHECK(COPIES == 0 && TOASTS == 1 && LAST_KIND == TOAST_INFO && strstr(LAST_TEXT, "main.c is on the desktop already") != 0);
    CHECK(deskfiles_send(5) == -1 && COPIES == 0 && LAST_KIND == TOAST_INFO);

    /* a drive, the root and a node that is gone */
    reset_counters();
    CHECK(deskfiles_send(1) == -1 && LAST_KIND == TOAST_WARNING && strstr(LAST_TEXT, "drive") != 0);
    CHECK(deskfiles_send(60) == -1 && LAST_KIND == TOAST_WARNING && strstr(LAST_TEXT, "not there") != 0);
    CHECK(COPIES == 0 && ADDS == 0);

    /* the copy fails (a locked drive): the drive's words in a toast, and nothing is claimed */
    reset_counters();
    COPY_OK = 0;
    CHECK(deskfiles_send(21) == -1);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_ERROR && strstr(LAST_TEXT, "Cannot send report.txt") != 0 && strstr(LAST_TEXT, "locked") != 0);
    COPY_OK = 1;

    /* no Desktop folder: it is made first, on the boot drive's top folder, then the copy goes in */
    for (int i = 1; i < MAXN; i++) /* the folder and all that was in it go */
        if (NODES[i].parent == 5)
            node_del(i);
    node_del(5);
    CHECK(deskfiles_folder() == -1);
    reset_counters();
    ADDS = COPIES = 0;
    c = deskfiles_send(21);
    CHECK(ADDS == 1 && ADDED_PARENT == 1 && strcmp(ADDED_NAME, "Desktop") == 0);
    int made = deskfiles_folder();
    CHECK(made > 0 && NODES[made].is_dir && COPIES == 1 && COPY_DEST == made && c > 0);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_SUCCESS && SET_FOUND == 1 && SET_N == 1);

    /* no Desktop folder and the drive cannot be written: the drive's own words, no copy, no half-made folder */
    for (int i = 1; i < MAXN; i++)
        if (NODES[i].parent == made)
            node_del(i);
    node_del(made);
    reset_counters();
    ADDS = COPIES = 0;
    ADD_OK = 0;
    CHECK(deskfiles_send(21) == -1);
    CHECK(ADDS == 1 && COPIES == 0 && deskfiles_folder() == -1);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_ERROR && strstr(LAST_TEXT, "Cannot make the Desktop folder") != 0 && strstr(LAST_TEXT, "locked") != 0);
    ADD_OK = 1;

    /* no drive at all to make it on */
    reset_counters();
    ADDS = COPIES = 0;
    VOL_FOLDER = -1;
    CHECK(deskfiles_send(21) == -1);
    CHECK(ADDS == 0 && COPIES == 0 && TOASTS == 1 && LAST_KIND == TOAST_WARNING && strstr(LAST_TEXT, "no drive") != 0);
    VOL_FOLDER = 1;


    /* ================= Send to desktop, several at once ================= */
    for (int i = 1; i < MAXN; i++) /* a clean slate: only the boot drive, a Desktop folder with a file in it, and some files to send */
        NODES[i].used = 0;
    node_set(1, 0, "HARISOS", 1);
    node_set(5, 1, "Desktop", 1);
    node_set(6, 5, "there.txt", 0);
    node_set(20, 1, "Docs", 1);
    node_set(21, 20, "a.txt", 0);
    node_set(22, 20, "b.txt", 0);
    node_set(23, 20, "c.txt", 0);
    ADD_OK = COPY_OK = 1;
    COPY_FAIL_MASK = COPY_CALL = 0;

    /* nothing to send, one, several */
    reset_counters();
    ADDS = COPIES = 0;
    int none[1] = {21};
    CHECK(deskfiles_send_many(none, 0) == 0 && TOASTS == 0 && COPIES == 0);
    int one[1] = {21};
    CHECK(deskfiles_send_many(one, 1) == 1 && COPIES == 1 && strcmp(LAST_TEXT, "Sent a.txt to the desktop.") == 0); /* one: as deskfiles_send */

    /* several: each copied, one toast for all, the icons told once */
    reset_counters();
    ADDS = COPIES = 0;
    int three[3] = {21, 22, 23};
    CHECK(deskfiles_send_many(three, 3) == 3);
    CHECK(COPIES == 3 && ADDS == 0 && COPY_DEST == 5);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_SUCCESS && strcmp(LAST_TEXT, "Sent 3 items to the desktop.") == 0);
    CHECK(SET_CALLS == 1);
    int on_desk = 0;
    for (int i = 0; i < SET_N; i++)
        on_desk += strcmp(SET_FILES[i].name, "a.txt") == 0 || strcmp(SET_FILES[i].name, "b.txt") == 0 || strcmp(SET_FILES[i].name, "c.txt") == 0;
    CHECK(on_desk >= 3);

    /* some of them are on the desktop already: not copied again, and the message says so */
    reset_counters();
    ADDS = COPIES = 0;
    int mixed[4] = {21, 6, 22, 23}; /* 6 is in the Desktop folder */
    CHECK(deskfiles_send_many(mixed, 4) == 3 && COPIES == 3);
    CHECK(LAST_KIND == TOAST_SUCCESS && strcmp(LAST_TEXT, "Sent 3 items to the desktop (1 was there already).") == 0);
    int twice[3] = {6, 21, 6};
    reset_counters();
    COPIES = 0;
    CHECK(deskfiles_send_many(twice, 3) == 1 && strstr(LAST_TEXT, "Sent 1 item to the desktop (2 were there already).") != 0);

    /* all of them are on the desktop: nothing is copied, and no Desktop folder is made (there is one here, so look without) */
    reset_counters();
    ADDS = COPIES = 0;
    node_set(7, 5, "also.txt", 0);
    int there[2] = {6, 7};
    CHECK(deskfiles_send_many(there, 2) == 0 && COPIES == 0 && TOASTS == 1 && LAST_KIND == TOAST_INFO);
    CHECK(strcmp(LAST_TEXT, "They are on the desktop already.") == 0);

    /* only a drive, or a node that is gone: nothing to send, and nothing made */
    reset_counters();
    ADDS = COPIES = 0;
    int drives[2] = {1, 50};
    CHECK(deskfiles_send_many(drives, 2) == 0 && COPIES == 0 && ADDS == 0);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_WARNING && strstr(LAST_TEXT, "Nothing to send") != 0);

    /* the drive can take some copies and not others: the ones that went are counted, the first reason is told */
    reset_counters();
    ADDS = COPIES = 0;
    COPY_CALL = 0;
    COPY_FAIL_MASK = 2u; /* the second copy fails */
    CHECK(deskfiles_send_many(three, 3) == 2 && COPIES == 3);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_WARNING && strstr(LAST_TEXT, "Sent 2 of 3 items; the rest: ") != 0 && strstr(LAST_TEXT, "locked") != 0);
    CHECK(SET_CALLS == 1); /* it looked again: two copies were made */
    COPY_FAIL_MASK = 0;

    /* none could be copied */
    reset_counters();
    ADDS = COPIES = 0;
    COPY_OK = 0;
    CHECK(deskfiles_send_many(three, 3) == 0);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_ERROR && strstr(LAST_TEXT, "Cannot send 3 items: ") != 0 && strstr(LAST_TEXT, "locked") != 0);
    COPY_OK = 1;

    /* no Desktop folder: made once, for all of them */
    for (int i = 1; i < MAXN; i++)
        if (NODES[i].parent == 5)
            node_del(i);
    node_del(5);
    reset_counters();
    ADDS = COPIES = 0;
    CHECK(deskfiles_send_many(three, 3) == 3);
    CHECK(ADDS == 1 && ADDED_PARENT == 1 && strcmp(ADDED_NAME, "Desktop") == 0 && COPIES == 3);
    CHECK(TOASTS == 1 && LAST_KIND == TOAST_SUCCESS && strcmp(LAST_TEXT, "Sent 3 items to the desktop.") == 0);

    /* no Desktop folder and none can be made (a locked drive): nothing is copied, the drive's words are the toast */
    for (int i = 1; i < MAXN; i++)
        if (NODES[i].parent == deskfiles_folder() && deskfiles_folder() > 0)
            node_del(i);
    if (deskfiles_folder() > 0)
        node_del(deskfiles_folder());
    reset_counters();
    ADDS = COPIES = 0;
    ADD_OK = 0;
    CHECK(deskfiles_send_many(three, 3) == 0);
    CHECK(ADDS == 1 && COPIES == 0 && TOASTS == 1 && LAST_KIND == TOAST_ERROR && strstr(LAST_TEXT, "Cannot make the Desktop folder") != 0);
    ADD_OK = 1;

    printf("deskfiles tests passed\n");
    return 0;
}
