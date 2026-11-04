/* Host-side tests for the desktop icons (kernel/desktop/deskicons.c): the default grid, hit testing, select, double click, drag and drop
 * (snap, swap, clamp), the ICONS.CFG that is written and read, and what is drawn. Drawing and the disk are stubs. */
#include "deskicons.h"
#include "app.h"
#include "exec.h"
#include "look.h"
#include "mouse.h"
#include "taskbar.h"
#include "timer.h"
#include "vfs.h"
#include "wm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- the apps ---- */
static int OPENED[8];
static void open0(void) { OPENED[0]++; }
static void open1(void) { OPENED[1]++; }
static void open2(void) { OPENED[2]++; }
static void open3(void) { OPENED[3]++; }
static void open4(void) { OPENED[4]++; }
static void open5(void) { OPENED[5]++; }
static void open6(void) { OPENED[6]++; }
static void open7(void) { OPENED[7]++; }
static const AppInfo APPS[8] = {
    {"File Explorer", "", UI_ICON_FILES, open0},    {"Device Manager", "", UI_ICON_DEVMGR, open1}, {"Display Settings", "", UI_ICON_DISPLAY, open2},
    {"Task Manager", "", UI_ICON_TASKMGR, open3},   {"Notepad", "", UI_ICON_NOTEPAD, open4},       {"Terminal", "", UI_ICON_TERMINAL, open5},
    {"Wi-Fi", "", UI_ICON_WIFI, open6},             {"Personalize", "", UI_ICON_SETTINGS, open7},
};
int app_count(void) { return 8; }
const AppInfo *app_get(int i) { return i >= 0 && i < 8 ? &APPS[i] : 0; }

/* ---- stubs ---- */
static UiTheme THEME_STORE;
const UiTheme *ui_theme(void) { return &THEME_STORE; }
UiRect ui_rect(int x, int y, int w, int h) { UiRect r = {x, y, w, h}; return r; }
int ui_contains(UiRect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
UiRgb ui_rgb(unsigned char r, unsigned char g, unsigned char b) { UiRgb c = {r, g, b}; return c; }
GfxColor ui_color(UiRgb c) { (void)c; return 0; }
GfxColor gfx_rgb(unsigned char r, unsigned char g, unsigned char b) { (void)r; (void)g; (void)b; return 0; }
int ui_text_width(const char *s, UiTextStyle st) { (void)st; return (int)strlen(s) * 8; }
static int FILLS, ROUNDS, ICONS, TEXTS;
static char LAST_TEXTS[8][40];
void gfx_fill_round_rect_alpha(int x, int y, int w, int h, int r, GfxColor c, unsigned char a) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)c; (void)a; FILLS++; }
void gfx_round_rect_alpha(int x, int y, int w, int h, int r, int t, GfxColor c, unsigned char a) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)t; (void)c; (void)a; ROUNDS++; }
void ui_icon(UiIcon i, int cx, int cy, int size, UiRgb t) { (void)i; (void)cx; (void)cy; (void)size; (void)t; ICONS++; }
void ui_text_in_color(UiRect r, const char *s, UiTextStyle st, UiAlign a, UiRgb c)
{
    (void)r; (void)st; (void)a; (void)c;
    if (TEXTS < 8)
        strncpy(LAST_TEXTS[TEXTS], s, 39);
    TEXTS++;
}
static GfxClip CLIP = {0, 0, 1280, 800};
GfxClip gfx_get_clip(void) { return CLIP; }
unsigned gfx_width(void) { return 1280; }
unsigned gfx_height(void) { return 800; }
int gfx_ready(void) { return 1; }
static unsigned long long NOW;
unsigned long long timer_ms(void) { return NOW; }
static int DAMAGES;
void wm_add_screen_damage(UiRect r) { (void)r; DAMAGES++; }

static int CFG_NODE = -1;
static char CFG_TEXT[512];
static char SAVED[512];
static int SAVES;
int exec_find(const char *name) { (void)name; return CFG_NODE; }
int vfs_read(int id, unsigned off, void *buf, unsigned len)
{
    (void)id; (void)off;
    unsigned n = (unsigned)strlen(CFG_TEXT);
    if (n > len)
        n = len;
    memcpy(buf, CFG_TEXT, n);
    return (int)n;
}
int vfs_save(int parent, const char *name, const void *buf, unsigned len)
{
    (void)parent;
    assert(strcmp(name, "ICONS.CFG") == 0);
    memcpy(SAVED, buf, len);
    SAVED[len] = 0;
    SAVES++;
    return 0;
}
static int FOLDER = 7;
int look_config_folder(void) { return FOLDER; }

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

/* the grid on a 1280 x 800 screen with a 52 px taskbar: 13 columns, 7 rows; cell (c, r) starts at (12 + 96 c, 12 + 104 r) */
static void center(int c, int r, int *x, int *y) { *x = 12 + 96 * c + 48; *y = 12 + 104 * r + 50; }
static int at(int c, int r) { int x, y; center(c, r, &x, &y); return deskicons_at(x, y); }

/* a click: the button goes down and up on the bare desktop */
static int click(int c, int r)
{
    int x, y;
    center(c, r, &x, &y);
    int a = deskicons_pointer(x, y, MOUSE_LEFT, 1);
    deskicons_pointer(x, y, 0, 1);
    return a;
}

static void draw_counts(void)
{
    FILLS = ROUNDS = ICONS = TEXTS = 0;
    deskicons_draw();
}

int main(void)
{
    /* default layout: top to bottom, then the next column */
    for (int i = 0; i < 7; i++)
        CHECK(at(0, i) == i);
    CHECK(at(1, 0) == 7);
    CHECK(at(1, 1) == -1 && at(5, 5) == -1);
    CHECK(deskicons_at(0, 0) == -1);       /* the margin */
    CHECK(deskicons_at(12, 12) == 0);      /* the first cell's corner */
    CHECK(deskicons_at(12 + 95, 12 + 103) == 0);  /* the first cell's last pixel */
    CHECK(deskicons_at(12 + 96, 12 + 50) == 7);   /* the next column starts right after it */
    CHECK(deskicons_visible());

    /* what is drawn: a tile, a picture and a name per icon, nothing selected or hovered yet */
    draw_counts();
    CHECK(FILLS == 8 && ROUNDS == 8 && ICONS == 8);
    CHECK(TEXTS >= 16 && TEXTS <= 32); /* a shadow and the name for each line */
    /* a long name is split at its space into two lines; a short one stays on one */
    CLIP = (GfxClip){12, 12 + 104 * 2, 12 + 96, 12 + 104 * 3}; /* the cell of "Display Settings" only */
    draw_counts();
    CHECK(FILLS == 1 && ICONS == 1 && TEXTS == 4);
    CHECK(strcmp(LAST_TEXTS[0], "Display") == 0 && strcmp(LAST_TEXTS[2], "Settings") == 0);
    CLIP = (GfxClip){12, 12 + 104 * 5, 12 + 96, 12 + 104 * 6}; /* "Terminal" */
    draw_counts();
    CHECK(TEXTS == 2 && strcmp(LAST_TEXTS[1], "Terminal") == 0);
    CLIP = (GfxClip){0, 0, 1280, 800};

    /* a press that is not on the bare desktop (a window is over it) does nothing */
    int x, y;
    center(0, 0, &x, &y);
    CHECK(deskicons_pointer(x, y, MOUSE_LEFT, 0) == 0);
    deskicons_pointer(x, y, 0, 0);
    draw_counts();
    CHECK(FILLS == 8 && ROUNDS == 8);

    /* select: the press is the icons' (the windows must not see it); the selected one gets a highlight and a border */
    DAMAGES = 0;
    NOW = 10000;
    CHECK(click(0, 1) == 1);
    CHECK(DAMAGES >= 1);
    draw_counts();
    CHECK(FILLS == 9 && ROUNDS == 9);
    /* a click on the empty desktop selects nothing, and is left to the windows */
    center(5, 5, &x, &y);
    CHECK(deskicons_pointer(x, y, MOUSE_LEFT, 1) == 0);
    deskicons_pointer(x, y, 0, 1);
    draw_counts();
    CHECK(FILLS == 8 && ROUNDS == 8);
    deskicons_select(3);
    draw_counts();
    CHECK(FILLS == 9);
    deskicons_select(-1);
    deskicons_select(99); /* out of range: none */
    draw_counts();
    CHECK(FILLS == 8);

    /* hover: a light highlight under the pointer, gone when it leaves or goes over a window */
    center(0, 2, &x, &y);
    DAMAGES = 0;
    deskicons_pointer(x, y, 0, 1);
    CHECK(DAMAGES >= 1);
    draw_counts();
    CHECK(FILLS == 9 && ROUNDS == 8);
    deskicons_pointer(x, y, 0, 0);
    draw_counts();
    CHECK(FILLS == 8);

    /* double click: the second press on the same icon within 400 ms opens it, once */
    memset(OPENED, 0, sizeof OPENED);
    NOW = 20000;
    click(0, 3);
    CHECK(OPENED[3] == 0);
    NOW = 20300;
    CHECK(click(0, 3) == 1);
    CHECK(OPENED[3] == 1);
    NOW = 20400; /* a third press is a first press again */
    click(0, 3);
    CHECK(OPENED[3] == 1);
    NOW = 30000; /* too slow */
    click(0, 4);
    NOW = 30500;
    click(0, 4);
    CHECK(OPENED[4] == 0);
    NOW = 40000; /* another icon in between */
    click(0, 5);
    NOW = 40100;
    click(0, 6);
    CHECK(OPENED[5] == 0 && OPENED[6] == 0);
    deskicons_open(2); /* what the menu's Open does */
    CHECK(OPENED[2] == 1);
    deskicons_open(-1);
    deskicons_open(8);
    int total = 0;
    for (int i = 0; i < 8; i++)
        total += OPENED[i];
    CHECK(total == 2);

    /* a press that moves less than 4 px is a click, not a drag: nothing moves, nothing is written */
    SAVES = 0;
    center(0, 0, &x, &y);
    deskicons_pointer(x, y, MOUSE_LEFT, 1);
    deskicons_pointer(x + 2, y + 2, MOUSE_LEFT, 1);
    deskicons_pointer(x + 2, y + 2, 0, 1);
    CHECK(SAVES == 0 && at(0, 0) == 0);

    /* drag to a free cell: the icon is lifted, follows the pointer, snaps on release, and the new place is written */
    NOW = 50000;
    int tx, ty;
    center(3, 2, &tx, &ty);
    CHECK(deskicons_pointer(x, y, MOUSE_LEFT, 1) == 1);
    CHECK(deskicons_pointer(x + 30, y + 20, MOUSE_LEFT, 1) == 1);
    CHECK(deskicons_pointer(tx - 10, ty + 8, MOUSE_LEFT, 0) == 1); /* over a window now: still the icon's */
    CHECK(deskicons_dragging());
    draw_counts();
    CHECK(ICONS == 7 && FILLS == 7 && ROUNDS == 7); /* the grid layer has the other seven; the lifted icon is not in it */
    FILLS = ROUNDS = ICONS = TEXTS = 0;
    deskicons_ghost_draw();                          /* the layer above the windows: the lifted icon where the pointer has it ... */
    CHECK(ICONS == 1 && FILLS == 2 && ROUNDS == 3);  /* ... tile and highlight (it is selected), their borders, and the outline of the cell it would land in */
    CHECK(deskicons_pointer(tx - 10, ty + 8, 0, 0) == 1); /* the release */
    CHECK(deskicons_pointer(tx, ty, 0, 1) == 0);          /* and the icons let go of the pointer */
    CHECK(!deskicons_dragging());
    FILLS = ROUNDS = ICONS = TEXTS = 0;
    deskicons_ghost_draw();
    CHECK(ICONS == 0 && FILLS == 0 && ROUNDS == 0);       /* nothing above the windows any more */
    CHECK(at(3, 2) == 0 && at(0, 0) == -1);
    CHECK(SAVES == 1 && strstr(SAVED, "File Explorer=3,2\r\n") != 0);
    CHECK(strstr(SAVED, "Device Manager=0,1\r\n") != 0 && strstr(SAVED, "Personalize=1,0\r\n") != 0);
    draw_counts();
    CHECK(ROUNDS == 8 + 1); /* no outline any more; the dropped icon stays selected */

    /* dropped on an icon: the two swap places */
    SAVES = 0;
    int ax, ay, bx, by;
    center(3, 2, &ax, &ay); /* File Explorer */
    center(0, 1, &bx, &by); /* Device Manager */
    NOW += 1000; /* not a double click with the press before */
    deskicons_pointer(ax, ay, MOUSE_LEFT, 1);
    deskicons_pointer(bx, by, MOUSE_LEFT, 1);
    deskicons_pointer(bx, by, 0, 1);
    CHECK(at(0, 1) == 0 && at(3, 2) == 1 && SAVES == 1);

    /* dropped off the grid: the nearest cell (clamped to the corners) */
    center(0, 1, &ax, &ay);
    NOW += 1000; /* not a double click with the press before */
    deskicons_pointer(ax, ay, MOUSE_LEFT, 1);
    deskicons_pointer(5000, 5000, MOUSE_LEFT, 1);
    deskicons_pointer(5000, 5000, 0, 1);
    CHECK(at(12, 6) == 0);
    center(12, 6, &ax, &ay);
    NOW += 1000; /* not a double click with the press before */
    deskicons_pointer(ax, ay, MOUSE_LEFT, 1);
    deskicons_pointer(-500, -500, MOUSE_LEFT, 1);
    deskicons_pointer(-500, -500, 0, 1);
    CHECK(at(0, 0) == 0);

    /* where the icons are, for a layer of their own: the cells in use, or the whole desktop while one is dragged */
    UiRect b = deskicons_bounds();
    CHECK(b.x == 0 && b.y == 0 && b.w == 12 + 4 * 96 + 4 && b.h == 12 + 7 * 104 + 4); /* four columns (0 .. 3: the swap put one at column 3) and seven rows (0 .. 6) */
    center(0, 0, &ax, &ay);
    NOW += 1000; /* not a double click with the press before */
    deskicons_pointer(ax, ay, MOUSE_LEFT, 1);
    deskicons_pointer(ax + 40, ay + 40, MOUSE_LEFT, 1);
    b = deskicons_ghost_bounds(); /* the dragged icon can be anywhere */
    CHECK(b.x == 0 && b.y == 0 && b.w == 1280 && b.h == 800 - TASKBAR_HEIGHT);
    b = deskicons_bounds();       /* the grid layer stays as it was */
    CHECK(b.w < 1280);
    deskicons_pointer(ax + 40, ay + 40, 0, 1);

    /* Arrange icons: the default order again, and written */
    SAVES = 0;
    deskicons_arrange();
    for (int i = 0; i < 7; i++)
        CHECK(at(0, i) == i);
    CHECK(at(1, 0) == 7);
    CHECK(SAVES == 1 && strstr(SAVED, "File Explorer=0,0\r\n") != 0 && strstr(SAVED, "Personalize=1,0\r\n") != 0);
    FOLDER = -1; /* no drive that can be written: nothing is saved, nothing breaks */
    SAVES = 0;
    deskicons_arrange();
    CHECK(SAVES == 0);
    FOLDER = 7;

    /* ICONS.CFG: places are taken as written; a cell that is off the grid or already taken, an app with no line, and a line of an app
     * that does not exist all end up on the first free cell, and no two icons share one */
    CFG_NODE = 5;
    strcpy(CFG_TEXT, "File Explorer=2,3\r\nTerminal=2,3\r\nBogus=1,1\r\nWi-Fi=99,99\r\nTask Manager=0,0\r\nnonsense\r\nNotepad=x,y\r\n");
    deskicons_load();
    CHECK(at(2, 3) == 0);  /* File Explorer, as written */
    CHECK(at(0, 0) == 3);  /* Task Manager, as written */
    CHECK(at(0, 1) == 1);  /* Device Manager: no line, first free cell */
    CHECK(at(0, 2) == 2);  /* Display Settings */
    CHECK(at(0, 3) == 4);  /* Notepad: a line it cannot be read from */
    CHECK(at(0, 4) == 5);  /* Terminal: its cell was taken by File Explorer */
    CHECK(at(0, 5) == 6);  /* Wi-Fi: off the grid */
    CHECK(at(0, 6) == 7);  /* Personalize */
    int seen[8] = {0};
    for (int c = 0; c < 13; c++)
        for (int r = 0; r < 7; r++)
            if (at(c, r) >= 0)
                seen[at(c, r)]++;
    for (int i = 0; i < 8; i++)
        CHECK(seen[i] == 1);

    /* what is written can be read back: the same places */
    deskicons_arrange();
    char first[512];
    strcpy(first, SAVED);
    center(0, 0, &ax, &ay);
    center(4, 4, &tx, &ty);
    NOW += 1000; /* not a double click with the press before */
    deskicons_pointer(ax, ay, MOUSE_LEFT, 1);
    deskicons_pointer(tx, ty, MOUSE_LEFT, 1);
    deskicons_pointer(tx, ty, 0, 1);
    CHECK(strcmp(first, SAVED) != 0); /* the move was written */
    strcpy(CFG_TEXT, SAVED);
    deskicons_arrange(); /* back to the default, then read the saved places in again */
    CHECK(at(0, 0) == 0);
    deskicons_load();
    CHECK(at(4, 4) == 0);                     /* File Explorer is where it was dropped */
    CHECK(at(0, 0) == -1 && at(0, 1) == 1);   /* and its old cell is empty; the others are where the default order put them */

    /* no config file: the default order, nothing is read */
    CFG_NODE = -1;
    CFG_TEXT[0] = 0;
    deskicons_arrange();
    deskicons_load();
    CHECK(at(0, 0) == 0 && at(1, 0) == 7);

    printf("deskicons tests passed\n");
    return 0;
}
