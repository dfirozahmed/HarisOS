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

#define TOOLBAR_H 52
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
static const char *NOTE; /* one-line message for the status bar, cleared by the next action */
static int LAST_ROW = -1;
static unsigned long long LAST_CLICK_MS;

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
static void update_nav_buttons(void)
{
    if (!EX_WIN)
        return;
    EX_WIN->btn[0].enabled = HIST_N > 0;       /* Back */
    EX_WIN->btn[1].enabled = CUR != VFS_ROOT;  /* Up */
}

static void reload(void)
{
    int total = vfs_list(CUR, ITEMS, VFS_MAX_NODES);
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
    if (push)
    {
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

    int px = tb.x + 10 + 70 + 6 + 60 + 10; /* after Back and Up */
    UiRect bar = ui_rect(px, tb.y + 9, tb.w - (px - tb.x) - 10, 34);
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

static void draw_list(const Layout *l, const UiTheme *t)
{
    int show_type = l->list.w >= 360;
    int right = l->list.x + l->list.w - 14; /* leave room for the scrollbar */
    int size_x = right - SIZE_COL_W;
    int type_x = show_type ? size_x - TYPE_COL_W : size_x;

    /* column header */
    UiRect h = l->head;
    ui_text_in_color(ui_rect(h.x + 44, h.y, 200, h.h), "Name", UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    if (show_type)
        ui_text_in_color(ui_rect(type_x, h.y, TYPE_COL_W, h.h), "Type", UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    ui_text_in_color(ui_rect(size_x, h.y, SIZE_COL_W - 6, h.h), "Size", UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
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
        else if (ri & 1)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->surface_alt), 40);
        }
        int kind = kind_of(n);
        draw_icon(r.x + 24, r.y + r.h / 2, ICON_PX, kind);
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

static void ex_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    Layout l = layout(body);
    clamp_scroll(&l);
    draw_sidebar(&l, t);
    draw_list(&l, t);
    draw_toolbar(&l, t);
    draw_status(&l, t);
}

/* ---- input ----------------------------------------------------- */
static int ex_press(Window *w, int x, int y)
{
    UiRect body = wm_content(w);
    Layout l = layout(body);

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

    if ((k->mods & KMOD_ALT) && k->key == KEY_LEFT)
    {
        go_back();
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
    case KEY_BACKSPACE:
        go_up();
        return;
    case KEY_ENTER:
        open_item(SEL);
        return;
    default:
        break;
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
}

static void explorer_app_open(void)
{
    if (EX_WIN)
    {
        wm_activate(EX_WIN);
        return;
    }
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 760, h = 500;
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
    win->on_close = ex_close;
    win->icon = TASKBAR_ICON_FILES;
    wm_set_min_size(win, 480, 320);
    EX_WIN = win;
    desktop_set_status(0);

    /* Back and Up sit at the left of the toolbar (button 0 and 1: update_nav_buttons relies on it) */
    wm_add_button_ex(win, "Back", UI_BTN_SECONDARY, ui_rect(10, 9, 70, 34), 0, on_back, 0);
    wm_add_button_ex(win, "Up", UI_BTN_SECONDARY, ui_rect(86, 9, 60, 34), 0, on_up, 0);

    CUR = VFS_ROOT;
    HIST_N = 0;
    SEL = -1;
    SCROLL = 0;
    NOTE = 0;
    LAST_ROW = -1;
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
