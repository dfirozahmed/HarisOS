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
#define MAXN 32
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

    printf("deskfiles tests passed\n");
    return 0;
}
