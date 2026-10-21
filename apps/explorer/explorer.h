/* File Explorer app: private header shared by the explorer_*.c files of this folder.
 *
 * The app is split by job:
 *   explorer_app.c      the AppInfo, opening and closing the window
 *   explorer_state.c    the state every part shares (current folder, selection, history, clipboard ...)
 *   explorer_util.c     file kinds, size text, small string helpers
 *   explorer_nav.c      current folder, history, sorting, layout of the window, selection
 *   explorer_ops.c      rename, new folder / file, clipboard (cut / copy / paste), delete with its dialog
 *   explorer_drive.c    drives: the write lock, drive information, VFS error messages
 *   explorer_menu.c     the right-click menu and the sort menu
 *   explorer_toolbar.c  toolbar buttons, their tooltips, path bar
 *   explorer_draw.c     icons, sidebar, file list, status bar, the window's draw function
 *   explorer_input.c    mouse and keyboard handlers of the window
 *
 * An app uses only public headers (ui.h, wm.h, desktop.h, vfs.h) - never desktop_internal.h. */
#ifndef HARIS_EXPLORER_H
#define HARIS_EXPLORER_H

#include "app.h"
#include "desktop.h"
#include "taskbar.h"
#include "timer.h"
#include "ui.h"
#include "vfs.h"
#include "wm.h"

/* ---- sizes ----------------------------------------------------- */
#define NAV_ROW_H 52 /* Back / Forward / Up + path bar */
#define CMD_ROW_H 44 /* New folder, Cut, Copy, Paste, Rename ... */
#define TOOLBAR_H (NAV_ROW_H + CMD_ROW_H)
#define STATUS_H 28
#define SIDE_W_DEFAULT 200 /* the sidebar can be dragged between SIDE_W_MIN and SIDE_W_MAX */
#define SIDE_W_MIN 140
#define SIDE_W_MAX 360
#define SIDE_GRAB 4        /* the drag zone reaches this far to each side of the divider */
#define LIST_W_MIN 240     /* the list always keeps at least this much room */
#define HEAD_H 26
#define ROW_H 32
#define PLACE_H 34
#define ICON_PX 22
#define SIZE_COL_W 92
#define TYPE_COL_W 120
#define WHEEL_ROWS 3
#define DOUBLE_CLICK_MS 450
#define HIST_MAX 16

/* toolbar button ids = the order they are added in ex_toolbar_create() */
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
#define NAVBTN 36                          /* icon-only Back / Forward / Up buttons */
#define PATH_X (10 + 3 * (NAVBTN + 2) + 6) /* path bar starts after the three navigation buttons */
#define NEWBTN_X 10                        /* "New folder" in the command row */
#define NEWBTN_W 118
#define CMD_X0 (NEWBTN_X + NEWBTN_W + 2 + NAVBTN + 16) /* Cut / Copy / Paste / Rename start here, after a divider */
#define CMD_Y (NAV_ROW_H + 5)
#define SORT_X (CMD_X0 + 5 * (NAVBTN + 2) + 14) /* "Sort" after a second divider */
#define SORT_W 82
#define TIP_DELAY_MS 500

/* ---- file kinds ------------------------------------------------ */
enum
{
    FK_FOLDER,
    FK_TEXT,
    FK_CODE,
    FK_IMAGE,
    FK_AUDIO,
    FK_EFI,
    FK_OTHER,
    FK_DRIVE /* a drive under This PC */
};

/* ---- window layout --------------------------------------------- */
typedef struct
{
    UiRect toolbar, side, head, list, status;
} Layout;

/* where the Type and Size columns are (shared by drawing and header clicks) */
typedef struct
{
    int show_type, type_x, size_x;
} Cols;

#define PLACE_MAX (VFS_MAX_VOLUMES + 1) /* sidebar entries: This PC and the drives */

/* ---- right-click menu: a small panel drawn inside the window ---- */
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

/* ---- shared state (explorer_state.c) ---------------------------- */
extern Window *EX_WIN;
extern int EX_CUR;                 /* folder being shown */
extern int EX_ITEMS[VFS_MAX_NODES]; /* its children, in the sort order */
extern int EX_NITEMS;
extern int EX_SEL;                 /* selected row, -1 = none */
extern int EX_SCROLL;              /* first visible row */
extern int EX_HIST[HIST_MAX];      /* folders to go back to */
extern int EX_HIST_N;
extern int EX_FWD[HIST_MAX];       /* folders to go forward to after Back */
extern int EX_FWD_N;
extern const char *EX_NOTE;        /* one-line message for the status bar, cleared by the next action */
extern int EX_SIDE_W;               /* sidebar width, set by dragging its edge */
extern int EX_SIDE_DRAG;            /* 1 while the edge is held */
extern int EX_LAST_ROW;           /* double-click detection */
extern unsigned long long EX_LAST_CLICK_MS;
extern int EX_HOVER_ROW;           /* list row under the pointer, -1 = none */
extern int EX_HOVER_PLACE;         /* sidebar entry under the pointer */
extern int EX_SORT_KEY;            /* VFS_SORT_* */
extern int EX_SORT_DESC;
extern int EX_TIP_BTN;             /* toolbar button the pointer rests on, -1 = none */
extern unsigned long long EX_TIP_SINCE; /* when it started resting there */
extern int EX_TIP_SHOWN;           /* the tooltip is visible */
extern int EX_CLIP_ID;             /* entry on the clipboard (Cut or Copy), -1 = empty */
extern int EX_CLIP_CUT;            /* 1 = it moves on Paste, 0 = a copy is made */
extern int EX_RENAME_ID;           /* node being renamed in its row (EX_WIN->edits[0] holds the text), -1 = none */
extern Menu EX_MENU;

/* ---- explorer_util.c -------------------------------------------- */
int ex_lower(int c);
int ex_kind_of(const VfsNode *n);
const char *ex_kind_name(int kind);
UiRgb ex_kind_color(int kind);
void ex_fmt_size(unsigned bytes, char *out);   /* "812 B", "1.2 KB", "3.0 MB" */
void ex_fmt_size_big(unsigned long long bytes, char *out); /* the same up to "1.8 TB" (for drives) */
int ex_put_uint(char *out, int o, int v);      /* append a number at out[o]; returns the new length */
int ex_put_str(char *out, int o, int cap, const char *s);
int ex_same_text(const char *a, const char *b);

/* ---- explorer_nav.c --------------------------------------------- */
void ex_update_nav_buttons(void);
void ex_reload(void);                          /* list the current folder again (in the sort order) */
void ex_refresh(void);                         /* ... after asking the drive again */
void ex_go_to(int dir, int push);
void ex_go_back(void);
void ex_go_forward(void);
void ex_go_up(void);
void ex_open_item(int row);                    /* folder: go in; file: no app yet */
void ex_set_sort(int key, int desc);
int ex_row_of(int id);                         /* row of a node in the list, -1 if not shown */
int ex_place_count(void);                      /* This PC + the drives */
int ex_place_id(int i);                        /* node of a sidebar place (0 = This PC, then the drives), -1 if none */
Layout ex_layout(UiRect body);
int ex_visible_rows(const Layout *l);
void ex_clamp_scroll(const Layout *l);
void ex_select_row(UiRect body, int row);      /* select and scroll into view */

/* ---- explorer_ops.c --------------------------------------------- */
void ex_start_rename(int row);
int ex_end_rename(int commit);                 /* 0 only when the new name is refused */
void ex_new_item(int is_dir);
void ex_clip_set(int cut);
void ex_paste(void);
void ex_ask_delete(int row);
int ex_delete_pending(void);                   /* the confirmation dialog is open */

/* A drive that has a lock to show: one that can be locked (FAT32, exFAT, NTFS) or one on a disk that cannot be written at all,
 * which always looks locked. The RAM Disk has none. */
#define EX_VOL_SHOWS_LOCK(vi) ((vi)->lockable || !(vi)->writable)
#define EX_VOL_LOCKED(vi) ((vi)->locked || !(vi)->writable)

/* ---- explorer_drive.c ------------------------------------------- */
int ex_is_drive(const VfsNode *n);             /* a drive under This PC */
int ex_cur_drive(VfsVolInfo *vi);              /* the drive the current folder is on; 0 at This PC */
void ex_note_error(void);                      /* show why the last VFS call failed */
int ex_check_writable(void);                   /* 1 if the current folder can be changed; else shows why and gives 0 */
int ex_lock_rect(const Layout *l, UiRect *r);  /* the Read-only / Writable pill in the status bar; 0 if none */
void ex_toggle_lock(void);                     /* lock the drive, or ask before unlocking it */
int ex_lock_dialog_pending(void);

/* ---- explorer_menu.c -------------------------------------------- */
void ex_menu_open(int x, int y, int kind);
void ex_menu_run(int item);
void ex_menu_step(int dir);
int ex_menu_hit(int x, int y);
void ex_menu_draw(const UiTheme *t);

/* ---- explorer_toolbar.c ----------------------------------------- */
void ex_toolbar_create(Window *win, int content_w);
void ex_sync_buttons(void);
void ex_draw_toolbar(const Layout *l, const UiTheme *t);
void ex_tip_draw(const Window *w, UiRect body, const UiTheme *t);
int ex_tick(Window *w);
int ex_cursor(Window *w, int x, int y);

/* ---- explorer_draw.c -------------------------------------------- */
void ex_draw_folder_icon(int cx, int cy, int size);
void ex_draw_drive_icon(int cx, int cy, int size);
void ex_draw_lock_icon(int cx, int cy, int size, int locked, UiRgb color);
void ex_draw_icon(int cx, int cy, int size, int kind);
Cols ex_columns(const Layout *l);
void ex_draw(Window *w, UiRect body);

/* ---- explorer_input.c ------------------------------------------- */
int ex_press(Window *w, int x, int y);
int ex_drag(Window *w, int x, int y, unsigned buttons);
int ex_rpress(Window *w, int x, int y);
int ex_hover(Window *w, int x, int y);
int ex_wheel(Window *w, int steps);
void ex_key(Window *w, const KeyEvent *k);

#endif
