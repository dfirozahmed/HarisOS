/* HarisOS desktop icons: see include/deskicons.h. */
#include "deskicons.h"
#include "app.h"
#include "deskfiles.h"
#include "exec.h"
#include "look.h"
#include "mouse.h"
#include "taskbar.h"
#include "timer.h"
#include "toast.h"
#include "vfs.h"
#include "wm.h"

#define CFG_NAME "ICONS.CFG"
#define CFG_MAX 4096
#define CELL_W 96  /* a grid cell: the tile, the name under it, and some air around */
#define CELL_H 104
#define MARGIN 12  /* from the screen's top and left edge to the first cell */
#define TILE 56    /* the rounded tile behind the picture */
#define NAME_LINE_H 16
#define DOUBLE_MS 400
#define DRAG_PX 4  /* the pointer must move this far before a press becomes a drag */
#define MAX_ICONS (APP_MAX + DESK_FILES_MAX)

typedef struct
{
    int file;                  /* 0 = an app shortcut, 1 = a file or folder of the Desktop folder */
    int app;                   /* app: its index in the registry */
    int node;                  /* file: its vfs node */
    int is_dir;                /* file: a folder */
    char name[DESK_FILE_NAME]; /* file: its name */
    int col, row;              /* the grid cell; -1 while it is being placed */
    int wc, wr;                /* the cell it is asked for when the icons are put in order (-1 = none) */
} Icon;

typedef struct
{
    char name[DESK_FILE_NAME];
    int col, row;
} Pending; /* a place that ICONS.CFG gave a file that is not on the desktop (yet) */

static Icon IC[MAX_ICONS];
static Icon ITEMS[MAX_ICONS]; /* the icons being put in order (too big for the stack) */
static int N, READY;
static unsigned char HIDDEN[APP_MAX]; /* app shortcuts taken off the desktop */
static Pending PEND[DESK_FILES_MAX];
static int PEND_N;
static int SEL = -1, HOVER = -1;
static unsigned char MARK[MAX_ICONS]; /* 1 = selected (a rubber band can select several; SEL is the last of them, -1 = none) */
static int BAND;                      /* rubber band: 0 = none, 1 = pressed on the bare desktop (not moved far yet), 2 = being drawn */
static int BX, BY;                    /* where the band started */
static UiRect BRECT;                  /* the band now (clamped to the desktop) */
static int overlaps(UiRect a, UiRect b);
static unsigned char BASE[MAX_ICONS]; /* the selection when a Ctrl / Shift band started: the band adds to it (Ctrl: flips) */
static unsigned MODS;                 /* KMOD_SHIFT / KMOD_CTRL while held: a mouse event carries no modifiers, the key events say */
static int GROUP;                     /* the drag moves every selected icon, not just the pressed one */
static int GDC, GDR;                  /* a drag: how many columns / rows the icon (or the group) has moved from its cells */
static int BAND_ADD;                  /* the band adds to the selection: 0 = no, 1 = Shift (adds), 2 = Ctrl (flips) */
static int PRESS = -1;      /* the icon the left button went down on, -1 = none */
static int DRAGGING;         /* it has been moved far enough to be dragged */
static int PX, PY;           /* where the button went down */
static int GRABX, GRABY;     /* where in its cell the pointer took it */
static int DX, DY;           /* the dragged icon's cell while it follows the pointer (screen pixels, top left) */
static int TCOL, TROW;       /* the cell it would snap to now */
static unsigned LAST_BTN;    /* the left button at the last event */
static int LAST_CLICK = -1;  /* the icon of the last click and when, for the double click */
static unsigned long long LAST_CLICK_MS;

/* ---- small string helpers (no libc) ---- */
static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int same_ci(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b))
        a++, b++;
    return !*a && !*b;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static char *put(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;
    return o;
}

static char *put_int(char *o, int v)
{
    char t[12];
    int n = 0;
    if (v < 0)
        v = 0;
    do
        t[n++] = (char)('0' + v % 10);
    while ((v /= 10) > 0);
    while (n)
        *o++ = t[--n];
    return o;
}

static void copy_name(char *dst, const char *src)
{
    int k = 0;
    for (; src[k] && k < DESK_FILE_NAME - 1; k++)
        dst[k] = src[k];
    dst[k] = 0;
}

/* does the name end in ".ext" (ignoring case)? */
static int ext_is(const char *name, const char *ext)
{
    int n = slen(name), e = slen(ext);
    if (n <= e + 1 || name[n - e - 1] != '.')
        return 0;
    return same_ci(name + n - e, ext);
}

/* ---- the grid ---- */
static void grid(int *cols, int *rows)
{
    int c = ((int)gfx_width() - MARGIN) / CELL_W;
    int r = ((int)gfx_height() - TASKBAR_HEIGHT - MARGIN) / CELL_H;
    *cols = c < 1 ? 1 : c;
    *rows = r < 1 ? 1 : r;
}

static int capacity(void)
{
    int c, r;
    grid(&c, &r);
    return c * r < MAX_ICONS ? c * r : MAX_ICONS;
}

static UiRect cell_rect(int col, int row)
{
    return ui_rect(MARGIN + col * CELL_W, MARGIN + row * CELL_H, CELL_W, CELL_H);
}

/* the icon sitting in a cell, -1 = free */
static int who_in(int col, int row)
{
    for (int i = 0; i < N; i++)
        if (IC[i].col == col && IC[i].row == row)
            return i;
    return -1;
}

/* the first free cell, top to bottom and then left to right (like Windows); all full: the first cell */
static void place_first_free(int i)
{
    int cols, rows;
    grid(&cols, &rows);
    for (int c = 0; c < cols; c++)
        for (int r = 0; r < rows; r++)
            if (who_in(c, r) < 0)
            {
                IC[i].col = c;
                IC[i].row = r;
                return;
            }
    IC[i].col = IC[i].row = 0;
}

/* ---- what an icon is ---- */
static const char *name_of(int i)
{
    if (IC[i].file)
        return IC[i].name;
    const AppInfo *a = app_get(IC[i].app);
    return a ? a->name : "";
}

static int same_icon(const Icon *a, const Icon *b)
{
    return a->file == b->file && (a->file ? same(a->name, b->name) : a->app == b->app);
}

static int icon_of_app(int app)
{
    for (int i = 0; i < N; i++)
        if (!IC[i].file && IC[i].app == app)
            return i;
    return -1;
}

static UiIcon icon_glyph(int i)
{
    if (!IC[i].file)
    {
        const AppInfo *a = app_get(IC[i].app);
        return a ? a->icon : UI_ICON_LOGO;
    }
    const char *n = IC[i].name;
    if (IC[i].is_dir)
        return UI_ICON_FILES;
    static const char *const TEXT[] = {"txt", "md", "ini", "log", "c", "h", "asm", 0};
    for (int k = 0; TEXT[k]; k++)
        if (ext_is(n, TEXT[k]))
            return UI_ICON_NOTEPAD;
    if (ext_is(n, "elf"))
        return UI_ICON_TERMINAL;
    if (ext_is(n, "bmp") || ext_is(n, "png") || ext_is(n, "jpg"))
        return UI_ICON_DISPLAY;
    if (ext_is(n, "wav") || ext_is(n, "mp3"))
        return UI_ICON_VOLUME;
    return UI_ICON_NOTEPAD; /* some file: a page */
}

/* ---- redraw marks ---- */
static void dmg_rect(UiRect r)
{
    wm_add_screen_damage(ui_rect(r.x - 4, r.y - 4, r.w + 8, r.h + 8));
}

static void dmg_cell(int col, int row)
{
    dmg_rect(cell_rect(col, row));
}

static void dmg_icon(int i)
{
    if (i >= 0 && i < N)
        dmg_cell(IC[i].col, IC[i].row);
}

/* ---- putting the icons in order ---- */
/* The icons of `items` (at most the room there is) become the desktop's icons. Each asks for a cell (wc, wr); it gets it if that is on
 * the grid and no icon before it has it, else the first free cell. What was drawn before and what is drawn now is marked. */
static void assemble(const Icon *items, int n)
{
    UiRect old = deskicons_bounds();
    Icon key;
    key.file = 0;
    key.app = -1;
    key.name[0] = 0;
    int had_sel = SEL >= 0 && SEL < N;
    if (had_sel)
        key = IC[SEL];
    int cap = capacity();
    if (n > cap)
        n = cap;
    unsigned char was[MAX_ICONS]; /* which of the new icons were selected (the same icon, wherever it is now) */
    for (int i = 0; i < n; i++)
    {
        was[i] = 0;
        for (int j = 0; j < N; j++)
            if (MARK[j] && same_icon(&IC[j], &items[i]))
                was[i] = 1;
    }
    for (int i = 0; i < n; i++)
    {
        IC[i] = items[i];
        IC[i].col = IC[i].row = -1;
    }
    N = n;
    int cols, rows;
    grid(&cols, &rows);
    for (int i = 0; i < N; i++)
        if (IC[i].wc >= 0 && IC[i].wc < cols && IC[i].wr >= 0 && IC[i].wr < rows && who_in(IC[i].wc, IC[i].wr) < 0)
        {
            IC[i].col = IC[i].wc;
            IC[i].row = IC[i].wr;
        }
    for (int i = 0; i < N; i++)
        if (IC[i].col < 0)
            place_first_free(i);
    SEL = -1;
    for (int i = 0; i < MAX_ICONS; i++)
        MARK[i] = i < N ? was[i] : 0;
    if (had_sel)
        for (int i = 0; i < N; i++)
            if (same_icon(&IC[i], &key))
                SEL = i;
    HOVER = -1;
    PRESS = -1; /* a drag in the middle of it is let go */
    DRAGGING = GROUP = 0;
    BAND = 0;
    LAST_CLICK = -1;
    wm_add_screen_damage(ui_rect(old.x, old.y, old.w + 8, old.h + 8));
    UiRect now = deskicons_bounds();
    wm_add_screen_damage(ui_rect(now.x, now.y, now.w + 8, now.h + 8));
}

/* the apps that are on the desktop, in the registry's order, with the cell they have now (from_now) or the one in wantc / wantr (-1 = none) */
static int app_items(Icon *out, const int *wantc, const int *wantr, int from_now)
{
    int m = 0;
    for (int a = 0; a < app_count() && a < APP_MAX; a++)
    {
        if (HIDDEN[a])
            continue;
        Icon it;
        it.file = 0;
        it.app = a;
        it.node = -1;
        it.is_dir = 0;
        it.name[0] = 0;
        it.col = it.row = -1;
        it.wc = wantc ? wantc[a] : -1;
        it.wr = wantr ? wantr[a] : -1;
        if (from_now)
        {
            int i = icon_of_app(a);
            if (i >= 0)
            {
                it.wc = IC[i].col;
                it.wr = IC[i].row;
            }
        }
        out[m++] = it;
    }
    return m;
}

static void ensure(void)
{
    if (READY)
        return;
    READY = 1;
    Icon *items = ITEMS;
    int m = app_items(items, 0, 0, 0);
    assemble(items, m);
}

/* ---- ICONS.CFG ---- */
/* Write ICONS.CFG. 1 = written; 0 = it could not be, and a toast says why (so a caller that has something to say itself keeps quiet then:
 * the one toast on the screen must be the warning). */
static int save(void)
{
    static char text[CFG_MAX];
    char *o = text, *end = text + CFG_MAX - 3 * DESK_FILE_NAME;
    for (int a = 0; a < app_count() && a < APP_MAX && o < end; a++)
    {
        const AppInfo *ap = app_get(a);
        int i = icon_of_app(a);
        o = put(o, ap->name);
        *o++ = '=';
        if (i < 0)
            o = put(o, "hidden");
        else
        {
            o = put_int(o, IC[i].col);
            *o++ = ',';
            o = put_int(o, IC[i].row);
        }
        o = put(o, "\r\n");
    }
    for (int i = 0; i < N && o < end; i++)
        if (IC[i].file)
        {
            o = put(o, "file:");
            o = put(o, IC[i].name);
            *o++ = '=';
            o = put_int(o, IC[i].col);
            *o++ = ',';
            o = put_int(o, IC[i].row);
            o = put(o, "\r\n");
        }
    for (int p = 0; p < PEND_N && o < end; p++) /* places of files the desktop does not have (yet): kept */
    {
        o = put(o, "file:");
        o = put(o, PEND[p].name);
        *o++ = '=';
        o = put_int(o, PEND[p].col);
        *o++ = ',';
        o = put_int(o, PEND[p].row);
        o = put(o, "\r\n");
    }
    int dir = look_config_folder();
    if (dir < 0) /* the drives start locked: nothing can be written until one is unlocked */
    {
        toast_show(TOAST_WARNING, "Icon places not saved - no drive can be written to. Unlock one (the lock in File Explorer's status bar).");
        return 0;
    }
    if (vfs_save(dir, CFG_NAME, text, (unsigned)(o - text)) < 0)
    {
        toast_show(TOAST_ERROR, "Icon places could not be saved: ICONS.CFG was not written.");
        return 0;
    }
    return 1;
}

/* one number at *p (digits), then skips them; -1 if there is none */
static int read_int(const char **p)
{
    if (**p < '0' || **p > '9')
        return -1;
    int v = 0;
    while (**p >= '0' && **p <= '9')
    {
        if (v < 100000)
            v = v * 10 + (**p - '0');
        (*p)++;
    }
    return v;
}

/* "col,row" at v; 0 if it is not that */
static int read_cell(const char *v, int *c, int *r)
{
    *c = read_int(&v);
    if (*c < 0 || *v != ',')
        return 0;
    v++;
    *r = read_int(&v);
    return *r >= 0;
}

/* Apply the text of ICONS.CFG: a line names an app ("Notepad=2,3", or "Notepad=hidden") or a file ("file:notes.txt=2,3") and its cell. A
 * cell that is off the grid or taken by an icon before it, and an app that is not named, get the first free cell. */
static void apply(char *text)
{
    int wc[APP_MAX], wr[APP_MAX];
    for (int i = 0; i < APP_MAX; i++)
    {
        wc[i] = wr[i] = -1;
        HIDDEN[i] = 0;
    }
    PEND_N = 0;
    for (char *line = text; *line;)
    {
        char *end = line;
        while (*end && *end != '\n' && *end != '\r')
            end++;
        char keep = *end;
        *end = 0;
        char *eq = 0;
        for (char *s = line; *s; s++)
            if (*s == '=')
                eq = s; /* the last one: a name may hold anything else */
        if (eq)
        {
            *eq = 0;
            const char *v = eq + 1;
            int c, r;
            if (line[0] == 'f' && line[1] == 'i' && line[2] == 'l' && line[3] == 'e' && line[4] == ':')
            {
                if (PEND_N < DESK_FILES_MAX && line[5] && slen(line + 5) < DESK_FILE_NAME && read_cell(v, &c, &r))
                {
                    copy_name(PEND[PEND_N].name, line + 5);
                    PEND[PEND_N].col = c;
                    PEND[PEND_N].row = r;
                    PEND_N++;
                }
            }
            else
                for (int i = 0; i < app_count() && i < APP_MAX; i++)
                    if (same_ci(app_get(i)->name, line))
                    {
                        if (same_ci(v, "hidden"))
                            HIDDEN[i] = 1;
                        else if (read_cell(v, &c, &r))
                        {
                            wc[i] = c;
                            wr[i] = r;
                        }
                    }
        }
        *end = keep;
        line = *end ? end + 1 : end;
    }
    Icon *items = ITEMS;
    int m = app_items(items, wc, wr, 0);
    int have_files = 0;
    for (int i = 0; i < N; i++) /* files that are already on the desktop: the place of the config, else where they are */
        if (IC[i].file && m < MAX_ICONS)
        {
            Icon it = IC[i];
            it.wc = IC[i].col;
            it.wr = IC[i].row;
            for (int p = 0; p < PEND_N; p++)
                if (same(PEND[p].name, it.name))
                {
                    it.wc = PEND[p].col;
                    it.wr = PEND[p].row;
                }
            items[m++] = it;
            have_files = 1;
        }
    if (have_files)
        PEND_N = 0; /* the files are known already; the places of others are not needed */
    assemble(items, m);
}

void deskicons_load(void)
{
    ensure();
    static char text[CFG_MAX + 1];
    int node = exec_find(CFG_NAME);
    int got = node >= 0 ? vfs_read(node, 0, text, CFG_MAX) : -1;
    if (got > 0)
    {
        text[got] = 0;
        apply(text);
    }
}

/* the Desktop folder's contents changed (or are known for the first time) */
void deskicons_set_files(const DeskFile *files, int n, int found)
{
    ensure();
    Icon *items = ITEMS;
    int m = app_items(items, 0, 0, 1);
    for (int j = 0; j < n && m < MAX_ICONS; j++)
    {
        Icon it;
        it.file = 1;
        it.app = -1;
        it.node = files[j].node;
        it.is_dir = files[j].is_dir;
        copy_name(it.name, files[j].name);
        it.col = it.row = -1;
        it.wc = it.wr = -1;
        for (int i = 0; i < N; i++) /* where it was */
            if (IC[i].file && same(IC[i].name, it.name))
            {
                it.wc = IC[i].col;
                it.wr = IC[i].row;
            }
        if (it.wc < 0)
            for (int p = 0; p < PEND_N; p++) /* where ICONS.CFG says it goes */
                if (same(PEND[p].name, it.name))
                {
                    it.wc = PEND[p].col;
                    it.wr = PEND[p].row;
                }
        items[m++] = it;
    }
    if (found)
        PEND_N = 0;
    assemble(items, m);
}

/* ---- using the icons ---- */
int deskicons_visible(void)
{
    ensure();
    return N > 0;
}

int deskicons_at(int x, int y)
{
    ensure();
    for (int i = 0; i < N; i++)
        if (ui_contains(cell_rect(IC[i].col, IC[i].row), x, y))
            return i;
    return -1;
}

void deskicons_select(int icon)
{
    ensure();
    if (icon < 0 || icon >= N)
        icon = -1;
    int several = 0; /* other icons are marked too: they have to be cleared even if `icon` is the SEL already */
    for (int i = 0; i < N; i++)
        if (MARK[i] && i != icon)
            several = 1;
    if (icon == SEL && !several && (icon < 0 || MARK[icon]))
        return;
    for (int i = 0; i < N; i++)
        if (MARK[i])
        {
            MARK[i] = 0;
            dmg_icon(i);
        }
    SEL = icon;
    if (icon >= 0)
        MARK[icon] = 1;
    dmg_icon(SEL);
}

void deskicons_set_mods(unsigned mods)
{
    MODS = mods & (KMOD_SHIFT | KMOD_CTRL);
}

int deskicons_is_selected(int icon)
{
    ensure();
    return icon >= 0 && icon < N && MARK[icon];
}

static int marked_count(void)
{
    int c = 0;
    for (int i = 0; i < N; i++)
        c += MARK[i];
    return c;
}

int deskicons_is_file(int icon)
{
    ensure();
    return icon >= 0 && icon < N && IC[icon].file;
}

void deskicons_open(int icon)
{
    ensure();
    if (icon < 0 || icon >= N)
        return;
    if (IC[icon].file)
    {
        deskfiles_open(IC[icon].node);
        return;
    }
    const AppInfo *a = app_get(IC[icon].app);
    if (a && a->open)
        a->open();
}

static void remove_icon(int i)
{
    UiRect old = deskicons_bounds();
    for (int k = i; k < N - 1; k++)
    {
        IC[k] = IC[k + 1];
        MARK[k] = MARK[k + 1];
    }
    N--;
    MARK[N] = 0;
    if (SEL == i)
        SEL = -1;
    else if (SEL > i)
        SEL--;
    HOVER = -1;
    PRESS = -1;
    DRAGGING = GROUP = 0;
    LAST_CLICK = -1;
    wm_add_screen_damage(ui_rect(old.x, old.y, old.w + 8, old.h + 8));
}

/* Delete everything selected: the app shortcuts go at once (one save, one toast); the files are asked about together, once. */
static void delete_selected(void)
{
    int nodes[DESK_FILES_MAX], nf = 0, apps = 0;
    for (int i = 0; i < N; i++)
        if (MARK[i] && IC[i].file && nf < DESK_FILES_MAX)
            nodes[nf++] = IC[i].node;
    for (int i = N - 1; i >= 0; i--) /* from the end: a removal moves only the ones after it */
        if (MARK[i] && !IC[i].file)
        {
            HIDDEN[IC[i].app] = 1;
            remove_icon(i);
            apps++;
        }
    if (apps)
    {
        char msg[TOAST_TEXT_MAX + 1], *o = msg;
        o = put_int(o, apps);
        o = put(o, apps == 1 ? " shortcut is" : " shortcuts are");
        o = put(o, " off the desktop. Start menu: right-click, Send to desktop.");
        *o = 0;
        if (save())
            toast_show(TOAST_INFO, msg);
    }
    if (nf == 1)
        deskfiles_ask_delete(nodes[0]);
    else if (nf > 1)
        deskfiles_ask_delete_many(nodes, nf);
}

void deskicons_delete(int icon)
{
    ensure();
    if (icon < 0 || icon >= N)
        return;
    if (MARK[icon] && marked_count() > 1) /* part of a selection: the whole selection goes */
    {
        delete_selected();
        return;
    }
    if (IC[icon].file) /* a file: asked first (a window), and then deleted for good */
    {
        deskfiles_ask_delete(IC[icon].node);
        return;
    }
    int a = IC[icon].app;
    char msg[TOAST_TEXT_MAX + 1], *o = msg;
    o = put(o, app_get(a)->name);
    o = put(o, " is off the desktop. Start menu: right-click it, Send to desktop.");
    *o = 0;
    HIDDEN[a] = 1;
    remove_icon(icon);
    if (save()) /* (when it could not be saved the warning is the message) */
        toast_show(TOAST_INFO, msg);
}

void deskicons_send_app(int app)
{
    ensure();
    const AppInfo *ap = app_get(app);
    if (!ap || app >= APP_MAX)
        return;
    char msg[TOAST_TEXT_MAX + 1], *o = msg;
    o = put(o, ap->name);
    int i = icon_of_app(app);
    if (i >= 0) /* it is there already: show which */
    {
        deskicons_select(i);
        o = put(o, " is on the desktop already.");
        *o = 0;
        toast_show(TOAST_INFO, msg);
        return;
    }
    if (N >= capacity())
    {
        toast_show(TOAST_WARNING, "The desktop is full: delete an icon first.");
        return;
    }
    UiRect old = deskicons_bounds();
    HIDDEN[app] = 0;
    IC[N].file = 0;
    IC[N].app = app;
    IC[N].node = -1;
    IC[N].is_dir = 0;
    IC[N].name[0] = 0;
    IC[N].wc = IC[N].wr = -1;
    IC[N].col = IC[N].row = -1;
    MARK[N] = 0;
    N++;
    place_first_free(N - 1);
    deskicons_select(N - 1);
    wm_add_screen_damage(ui_rect(old.x, old.y, old.w + 8, old.h + 8));
    UiRect now = deskicons_bounds();
    wm_add_screen_damage(ui_rect(now.x, now.y, now.w + 8, now.h + 8));
    o = put(o, " is on the desktop.");
    *o = 0;
    if (save())
        toast_show(TOAST_SUCCESS, msg);
}

void deskicons_arrange(void)
{
    ensure();
    Icon *items = ITEMS;
    int m = 0;
    for (int i = 0; i < N; i++) /* the order they have (the apps, then the files), but no wishes: the default places */
    {
        items[m] = IC[i];
        items[m].wc = items[m].wr = -1;
        m++;
    }
    assemble(items, m);
    save();
}

int deskicons_key(const KeyEvent *k)
{
    ensure();
    if (k->pressed && (k->mods & KMOD_CTRL) && !(k->mods & (KMOD_SHIFT | KMOD_ALT)) && k->key == 'a' && N > 0) /* select all */
    {
        for (int i = 0; i < N; i++)
            if (!MARK[i])
            {
                MARK[i] = 1;
                dmg_icon(i);
            }
        SEL = N - 1;
        return 1;
    }
    if (!k->pressed || SEL < 0 || SEL >= N)
        return 0;
    if (k->key == KEY_DELETE)
    {
        deskicons_delete(SEL); /* (the whole selection when there are several) */
        return 1;
    }
    if (k->key == KEY_ENTER)
    {
        deskicons_open(SEL);
        return 1;
    }
    return 0;
}

/* the cell nearest to the dragged icon's middle; a group moves as a block, so it stops where its outermost icon meets the grid's edge */
static void target_cell(int *col, int *row)
{
    int cols, rows;
    grid(&cols, &rows);
    int c = (DX + CELL_W / 2 - MARGIN) / CELL_W, r = (DY + CELL_H / 2 - MARGIN) / CELL_H; /* (the middle of the icon) */
    if (DX + CELL_W / 2 < MARGIN)
        c = -1; /* (integer division rounds toward zero) */
    if (DY + CELL_H / 2 < MARGIN)
        r = -1;
    if (!GROUP)
    {
        *col = c < 0 ? 0 : (c >= cols ? cols - 1 : c);
        *row = r < 0 ? 0 : (r >= rows ? rows - 1 : r);
        return;
    }
    int dc = c - IC[PRESS].col, dr = r - IC[PRESS].row;
    int c0 = cols, c1 = -1, r0 = rows, r1 = -1;
    for (int i = 0; i < N; i++)
        if (MARK[i])
        {
            c0 = IC[i].col < c0 ? IC[i].col : c0;
            c1 = IC[i].col > c1 ? IC[i].col : c1;
            r0 = IC[i].row < r0 ? IC[i].row : r0;
            r1 = IC[i].row > r1 ? IC[i].row : r1;
        }
    if (c0 + dc < 0)
        dc = -c0;
    if (c1 + dc >= cols)
        dc = cols - 1 - c1;
    if (r0 + dr < 0)
        dr = -r0;
    if (r1 + dr >= rows)
        dr = rows - 1 - r1;
    *col = IC[PRESS].col + dc;
    *row = IC[PRESS].row + dr;
}

static int moving(int i)
{
    return DRAGGING && (i == PRESS || (GROUP && MARK[i]));
}

/* where a lifted icon is drawn: its cell, moved as far as the pointer has moved the pressed one */
static UiRect moved_rect(int i)
{
    UiRect o = cell_rect(IC[PRESS].col, IC[PRESS].row), c = cell_rect(IC[i].col, IC[i].row);
    return ui_rect(c.x + DX - o.x, c.y + DY - o.y, CELL_W, CELL_H);
}

/* mark everything a drag draws or outlines now (the lifted icons and the cells they would land in) */
static void dmg_moving(void)
{
    for (int i = 0; i < N; i++)
        if (moving(i))
        {
            dmg_rect(moved_rect(i));
            dmg_cell(IC[i].col + GDC, IC[i].row + GDR);
        }
}

/* the button came up after a drag: drop the icon in the cell it is over; the icon that was there takes its old place. A group moves as a
 * block: the icons that were in its way go to the first free cells. */
static void drop(void)
{
    int i = PRESS;
    if (GROUP)
    {
        int nc[MAX_ICONS], nr[MAX_ICONS];
        dmg_moving();
        for (int k = 0; k < N; k++)
        {
            nc[k] = IC[k].col + GDC;
            nr[k] = IC[k].row + GDR;
        }
        for (int j = 0; j < N; j++) /* an icon that is not moving and sits where one lands steps aside */
            if (!MARK[j])
                for (int k = 0; k < N; k++)
                    if (MARK[k] && IC[j].col == nc[k] && IC[j].row == nr[k])
                    {
                        dmg_icon(j);
                        IC[j].col = IC[j].row = -1;
                    }
        for (int k = 0; k < N; k++)
            if (MARK[k])
            {
                dmg_icon(k);
                IC[k].col = nc[k];
                IC[k].row = nr[k];
                dmg_icon(k);
            }
        for (int j = 0; j < N; j++)
            if (IC[j].col < 0)
            {
                place_first_free(j);
                dmg_icon(j);
            }
        save();
        return;
    }
    int c = TCOL, r = TROW;
    dmg_rect(moved_rect(i));
    dmg_cell(c, r);
    dmg_icon(i);
    int other = who_in(c, r);
    if (other >= 0 && other != i)
    {
        IC[other].col = IC[i].col;
        IC[other].row = IC[i].row;
        dmg_icon(other);
    }
    IC[i].col = c;
    IC[i].row = r;
    dmg_icon(i);
    save();
}

int deskicons_pointer(int x, int y, unsigned buttons, int on_desktop)
{
    ensure();
    unsigned now = buttons & MOUSE_LEFT;
    int pressed = now && !(LAST_BTN & MOUSE_LEFT);
    int released = !now && (LAST_BTN & MOUSE_LEFT);
    LAST_BTN = now;

    if (PRESS < 0) /* the highlight under the pointer */
    {
        int h = on_desktop ? deskicons_at(x, y) : -1;
        if (h != HOVER)
        {
            dmg_icon(HOVER);
            HOVER = h;
            dmg_icon(HOVER);
        }
    }

    if (pressed && on_desktop)
    {
        int i = deskicons_at(x, y);
        int add = (MODS & (KMOD_CTRL | KMOD_SHIFT)) != 0;
        if (i >= 0 && add) /* Ctrl+click flips this icon in the selection, Shift+click adds it; no drag, no double click */
        {
            MARK[i] = (MODS & KMOD_CTRL) ? (unsigned char)!MARK[i] : 1;
            SEL = -1;
            for (int k = 0; k < N; k++)
                if (MARK[k])
                    SEL = k;
            dmg_icon(i);
            LAST_CLICK = -1;
            return 1;
        }
        if (i < 0 && add)
            for (int k = 0; k < N; k++)
                BASE[k] = MARK[k]; /* the band adds to what is selected */
        else if (!(i >= 0 && MARK[i] && marked_count() > 1)) /* (a press on one of several selected keeps them, to drag them all) */
            deskicons_select(i);
        if (i < 0)
        {
            LAST_CLICK = -1;
            BAND = 1; /* maybe a rubber band: it shows once the pointer has moved */
            BAND_ADD = add ? ((MODS & KMOD_CTRL) ? 2 : 1) : 0;
            BX = x;
            BY = y;
        }
        else
        {
            unsigned long long t = timer_ms();
            if (i == LAST_CLICK && t - LAST_CLICK_MS <= DOUBLE_MS) /* a double click opens it (and does not start a drag) */
            {
                LAST_CLICK = -1;
                deskicons_open(i);
                return 1;
            }
            LAST_CLICK = i;
            LAST_CLICK_MS = t;
            PRESS = i;
            PX = x;
            PY = y;
            UiRect c = cell_rect(IC[i].col, IC[i].row);
            GRABX = x - c.x;
            GRABY = y - c.y;
            DRAGGING = 0;
            return 1;
        }
    }

    if (BAND)
    {
        int shown = BAND == 2;
        if (now && BAND == 1 && ((x - BX) * (x - BX) + (y - BY) * (y - BY) > DRAG_PX * DRAG_PX))
        {
            BAND = 2;
            BRECT = ui_rect(0, 0, 0, 0);
            shown = 1;
        }
        if (now && shown)
        {
            int W = (int)gfx_width(), H = (int)gfx_height() - TASKBAR_HEIGHT;
            int cx = x < 0 ? 0 : (x >= W ? W - 1 : x), cy = y < 0 ? 0 : (y >= H ? H - 1 : y); /* the band stays on the desktop */
            int x0 = BX < cx ? BX : cx, y0 = BY < cy ? BY : cy;
            UiRect nr = ui_rect(x0, y0, (BX < cx ? cx - BX : BX - cx) + 1, (BY < cy ? cy - BY : BY - cy) + 1);
            dmg_rect(BRECT);
            dmg_rect(nr);
            BRECT = nr;
            SEL = -1;
            for (int i = 0; i < N; i++) /* every icon whose cell the band touches is selected */
            {
                UiRect c = cell_rect(IC[i].col, IC[i].row);
                int m = overlaps(nr, ui_rect(c.x + 4, c.y + 2, c.w - 8, c.h - 4));
                if (BAND_ADD == 1)
                    m |= BASE[i];
                else if (BAND_ADD == 2)
                    m ^= BASE[i];
                if (m != MARK[i])
                {
                    MARK[i] = (unsigned char)m;
                    dmg_icon(i);
                }
                if (m)
                    SEL = i;
            }
            if (HOVER >= 0)
            {
                dmg_icon(HOVER);
                HOVER = -1;
            }
        }
        if (!now || released)
        {
            if (shown)
                dmg_rect(BRECT);
            BAND = 0;
        }
        return shown;
    }

    if (PRESS >= 0)
    {
        if (!DRAGGING && now && ((x - PX) * (x - PX) + (y - PY) * (y - PY) > DRAG_PX * DRAG_PX))
        {
            DRAGGING = 1; /* lifted: its cell is empty until it is dropped */
            GROUP = MARK[PRESS] && marked_count() > 1;
            DX = x - GRABX;
            DY = y - GRABY;
            TCOL = IC[PRESS].col;
            TROW = IC[PRESS].row;
            GDC = GDR = 0;
            for (int i = 0; i < N; i++)
                if (moving(i))
                    dmg_icon(i);
            if (HOVER >= 0)
            {
                dmg_icon(HOVER);
                HOVER = -1;
            }
        }
        if (DRAGGING && now)
        {
            dmg_moving();
            DX = x - GRABX;
            DY = y - GRABY;
            target_cell(&TCOL, &TROW);
            GDC = TCOL - IC[PRESS].col;
            GDR = TROW - IC[PRESS].row;
            dmg_moving();
        }
        if (released)
        {
            if (DRAGGING)
                drop();
            else if (MARK[PRESS] && marked_count() > 1) /* a click (no drag) on one of several selected: just that one */
                deskicons_select(PRESS);
            DRAGGING = GROUP = 0;
            PRESS = -1;
        }
        return 1;
    }
    return 0;
}

/* ---- drawing ---- */
static int overlaps(UiRect a, UiRect b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

/* a name under the tile: white with a dark edge so it reads on any wallpaper; one line, or two when it does not fit and has a space */
static void draw_name(const char *name, int x, int y, int w)
{
    char first[DESK_FILE_NAME];
    const char *a = name, *b = 0;
    int n = slen(name);
    if (ui_text_width(name, UI_TEXT_SMALL) > w && n < (int)sizeof first)
    {
        int cut = -1;
        for (int k = 0; k < n; k++)
        {
            if (name[k] != ' ')
                continue;
            for (int j = 0; j < k; j++)
                first[j] = name[j];
            first[k] = 0;
            if (ui_text_width(first, UI_TEXT_SMALL) <= w)
                cut = k;
        }
        if (cut >= 0)
        {
            for (int j = 0; j < cut; j++)
                first[j] = name[j];
            first[cut] = 0;
            a = first;
            b = name + cut + 1;
        }
    }
    UiRgb ink = ui_rgb(246, 248, 255), edge = ui_rgb(0, 0, 0);
    for (int line = 0; line < 2; line++)
    {
        const char *s = line ? b : a;
        if (!s)
            break;
        UiRect r = ui_rect(x, y + line * NAME_LINE_H, w, NAME_LINE_H);
        ui_text_in_color(ui_rect(r.x + 1, r.y + 1, r.w, r.h), s, UI_TEXT_SMALL, UI_ALIGN_CENTER, edge);
        ui_text_in_color(r, s, UI_TEXT_SMALL, UI_ALIGN_CENTER, ink);
    }
}

static void draw_icon(int i, UiRect c)
{
    const UiTheme *t = ui_theme();
    if (MARK[i]) /* selected: the accent behind the whole cell */
    {
        gfx_fill_round_rect_alpha(c.x + 4, c.y + 2, c.w - 8, c.h - 4, 10, ui_color(t->accent), 70);
        gfx_round_rect_alpha(c.x + 4, c.y + 2, c.w - 8, c.h - 4, 10, 1, ui_color(t->accent_hi), 150);
    }
    else if (i == HOVER)
        gfx_fill_round_rect_alpha(c.x + 4, c.y + 2, c.w - 8, c.h - 4, 10, gfx_rgb(255, 255, 255), 30);
    int tx = c.x + (c.w - TILE) / 2, ty = c.y + 6;
    gfx_fill_round_rect_alpha(tx, ty, TILE, TILE, 14, gfx_rgb(16, 20, 36), 130);
    gfx_round_rect_alpha(tx, ty, TILE, TILE, 14, 1, gfx_rgb(255, 255, 255), 46);
    ui_icon(icon_glyph(i), tx + TILE / 2, ty + TILE / 2, 32, ui_rgb(236, 241, 255));
    draw_name(name_of(i), c.x + 2, ty + TILE + 6, c.w - 4);
}

void deskicons_draw(void)
{
    ensure();
    if (N == 0 || !gfx_ready())
        return;
    GfxClip cl = gfx_get_clip();
    UiRect clip = ui_rect(cl.x0, cl.y0, cl.x1 - cl.x0, cl.y1 - cl.y0);
    for (int i = 0; i < N; i++)
    {
        if (moving(i))
            continue; /* it is lifted: deskicons_ghost_draw draws it, above the windows */
        UiRect c = cell_rect(IC[i].col, IC[i].row);
        if (overlaps(c, clip))
            draw_icon(i, c);
    }
}

int deskicons_dragging(void) { return DRAGGING || BAND == 2; }

UiRect deskicons_ghost_bounds(void)
{
    return ui_rect(0, 0, (int)gfx_width(), (int)gfx_height() - TASKBAR_HEIGHT);
}

void deskicons_ghost_draw(void)
{
    if ((!DRAGGING && BAND != 2) || !gfx_ready())
        return;
    GfxClip cl = gfx_get_clip();
    UiRect clip = ui_rect(cl.x0, cl.y0, cl.x1 - cl.x0, cl.y1 - cl.y0);
    if (BAND == 2) /* the rubber band: a light fill and a border, above the windows */
    {
        if (overlaps(BRECT, clip))
        {
            const UiTheme *t = ui_theme();
            gfx_fill_round_rect_alpha(BRECT.x, BRECT.y, BRECT.w, BRECT.h, 0, ui_color(t->accent), 60);
            gfx_round_rect_alpha(BRECT.x, BRECT.y, BRECT.w, BRECT.h, 0, 1, ui_color(t->accent_hi), 200);
        }
        return;
    }
    for (int i = 0; i < N; i++)
        if (moving(i))
        {
            UiRect tc = cell_rect(IC[i].col + GDC, IC[i].row + GDR); /* where it would land */
            if (overlaps(tc, clip))
                gfx_round_rect_alpha(tc.x + 4, tc.y + 2, tc.w - 8, tc.h - 4, 10, 2, gfx_rgb(255, 255, 255), 120);
        }
    for (int i = 0; i < N; i++)
        if (moving(i))
        {
            UiRect dr = moved_rect(i);
            if (overlaps(dr, clip))
                draw_icon(i, dr);
        }
}

UiRect deskicons_bounds(void)
{
    int maxc = 0, maxr = 0;
    for (int i = 0; i < N; i++)
    {
        if (IC[i].col > maxc)
            maxc = IC[i].col;
        if (IC[i].row > maxr)
            maxr = IC[i].row;
    }
    return ui_rect(0, 0, MARGIN + (maxc + 1) * CELL_W + 4, MARGIN + (maxr + 1) * CELL_H + 4);
}
