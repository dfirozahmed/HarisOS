/* HarisOS desktop icons: see include/deskicons.h. */
#include "deskicons.h"
#include "app.h"
#include "exec.h"
#include "look.h"
#include "mouse.h"
#include "taskbar.h"
#include "timer.h"
#include "vfs.h"
#include "wm.h"

#define CFG_NAME "ICONS.CFG"
#define CFG_MAX 512
#define CELL_W 96  /* a grid cell: the tile, the name under it, and some air around */
#define CELL_H 104
#define MARGIN 12  /* from the screen's top and left edge to the first cell */
#define TILE 56    /* the rounded tile behind the picture */
#define NAME_LINE_H 16
#define DOUBLE_MS 400
#define DRAG_PX 4  /* the pointer must move this far before a press becomes a drag */

typedef struct
{
    const AppInfo *app;
    int col, row; /* the grid cell; -1 while it is being placed */
} Icon;

static Icon IC[APP_MAX];
static int N, READY;
static int SEL = -1, HOVER = -1;
static int PRESS = -1;       /* the icon the left button went down on, -1 = none */
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

/* ---- the grid ---- */
static void grid(int *cols, int *rows)
{
    int c = ((int)gfx_width() - MARGIN) / CELL_W;
    int r = ((int)gfx_height() - TASKBAR_HEIGHT - MARGIN) / CELL_H;
    *cols = c < 1 ? 1 : c;
    *rows = r < 1 ? 1 : r;
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

static void default_layout(void)
{
    for (int i = 0; i < N; i++)
        IC[i].col = IC[i].row = -1;
    for (int i = 0; i < N; i++)
        place_first_free(i);
}

static void ensure(void)
{
    if (READY)
        return;
    READY = 1;
    N = app_count() < APP_MAX ? app_count() : APP_MAX;
    for (int i = 0; i < N; i++)
        IC[i].app = app_get(i);
    default_layout();
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

static void dmg_all(void)
{
    for (int i = 0; i < N; i++)
        dmg_icon(i);
}

/* ---- ICONS.CFG ---- */
static void save(void)
{
    char text[CFG_MAX], *o = text;
    for (int i = 0; i < N && o - text < CFG_MAX - 48; i++)
    {
        o = put(o, IC[i].app->name);
        *o++ = '=';
        o = put_int(o, IC[i].col);
        *o++ = ',';
        o = put_int(o, IC[i].row);
        o = put(o, "\r\n");
    }
    int dir = look_config_folder();
    if (dir >= 0)
        vfs_save(dir, CFG_NAME, text, (unsigned)(o - text)); /* no drive to write to, or it fails: the places last until the next boot */
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

/* apply the text of ICONS.CFG: a line names an app and its cell; a cell that is off the grid or taken by an earlier line, and an app
 * that is not named, get the first free cell */
static void apply(char *text)
{
    int cols, rows, wc[APP_MAX], wr[APP_MAX];
    grid(&cols, &rows);
    for (int i = 0; i < N; i++)
        wc[i] = wr[i] = -1;
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
            int c = read_int(&v), r = -1;
            if (c >= 0 && *v == ',')
            {
                v++;
                r = read_int(&v);
            }
            for (int i = 0; i < N && r >= 0; i++)
                if (same_ci(IC[i].app->name, line))
                {
                    wc[i] = c;
                    wr[i] = r;
                }
        }
        *end = keep;
        line = *end ? end + 1 : end;
    }
    for (int i = 0; i < N; i++)
        IC[i].col = IC[i].row = -1;
    for (int i = 0; i < N; i++)
        if (wc[i] >= 0 && wc[i] < cols && wr[i] >= 0 && wr[i] < rows && who_in(wc[i], wr[i]) < 0)
        {
            IC[i].col = wc[i];
            IC[i].row = wr[i];
        }
    for (int i = 0; i < N; i++)
        if (IC[i].col < 0)
            place_first_free(i);
}

void deskicons_load(void)
{
    ensure();
    char text[CFG_MAX + 1];
    int node = exec_find(CFG_NAME);
    int got = node >= 0 ? vfs_read(node, 0, text, CFG_MAX) : -1;
    if (got > 0)
    {
        text[got] = 0;
        apply(text);
    }
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
    if (icon == SEL)
        return;
    dmg_icon(SEL);
    SEL = icon;
    dmg_icon(SEL);
}

void deskicons_open(int icon)
{
    ensure();
    if (icon >= 0 && icon < N && IC[icon].app && IC[icon].app->open)
        IC[icon].app->open();
}

void deskicons_arrange(void)
{
    ensure();
    dmg_all();
    default_layout();
    dmg_all();
    save();
}

/* the cell nearest to the dragged icon's middle */
static void target_cell(int *col, int *row)
{
    int cols, rows;
    grid(&cols, &rows);
    int c = (DX + CELL_W / 2 - MARGIN) / CELL_W, r = (DY + CELL_H / 2 - MARGIN) / CELL_H; /* (the middle of the icon) */
    *col = c < 0 ? 0 : (c >= cols ? cols - 1 : c);
    *row = r < 0 ? 0 : (r >= rows ? rows - 1 : r);
}

static UiRect dragged_rect(void)
{
    return ui_rect(DX, DY, CELL_W, CELL_H);
}

/* the button came up after a drag: drop the icon in the cell it is over; the icon that was there takes its old place */
static void drop(void)
{
    int i = PRESS;
    int c = TCOL, r = TROW;
    dmg_rect(dragged_rect());
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
        deskicons_select(i);
        if (i < 0)
            LAST_CLICK = -1;
        else
        {
            unsigned long long t = timer_ms();
            if (i == LAST_CLICK && t - LAST_CLICK_MS <= DOUBLE_MS) /* a double click opens the app (and does not start a drag) */
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

    if (PRESS >= 0)
    {
        if (!DRAGGING && now && ((x - PX) * (x - PX) + (y - PY) * (y - PY) > DRAG_PX * DRAG_PX))
        {
            DRAGGING = 1; /* lifted: its cell is empty until it is dropped */
            DX = x - GRABX;
            DY = y - GRABY;
            TCOL = IC[PRESS].col;
            TROW = IC[PRESS].row;
            dmg_icon(PRESS);
            if (HOVER >= 0)
            {
                dmg_icon(HOVER);
                HOVER = -1;
            }
        }
        if (DRAGGING && now)
        {
            dmg_rect(dragged_rect());
            dmg_cell(TCOL, TROW);
            DX = x - GRABX;
            DY = y - GRABY;
            target_cell(&TCOL, &TROW);
            dmg_rect(dragged_rect());
            dmg_cell(TCOL, TROW);
        }
        if (released)
        {
            if (DRAGGING)
                drop();
            DRAGGING = 0;
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
    char first[40];
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
    if (i == SEL) /* selected: the accent behind the whole cell */
    {
        gfx_fill_round_rect_alpha(c.x + 4, c.y + 2, c.w - 8, c.h - 4, 10, ui_color(t->accent), 70);
        gfx_round_rect_alpha(c.x + 4, c.y + 2, c.w - 8, c.h - 4, 10, 1, ui_color(t->accent_hi), 150);
    }
    else if (i == HOVER)
        gfx_fill_round_rect_alpha(c.x + 4, c.y + 2, c.w - 8, c.h - 4, 10, gfx_rgb(255, 255, 255), 30);
    int tx = c.x + (c.w - TILE) / 2, ty = c.y + 6;
    gfx_fill_round_rect_alpha(tx, ty, TILE, TILE, 14, gfx_rgb(16, 20, 36), 130);
    gfx_round_rect_alpha(tx, ty, TILE, TILE, 14, 1, gfx_rgb(255, 255, 255), 46);
    ui_icon(IC[i].app->icon, tx + TILE / 2, ty + TILE / 2, 32, ui_rgb(236, 241, 255));
    draw_name(IC[i].app->name, c.x + 2, ty + TILE + 6, c.w - 4);
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
        if (DRAGGING && i == PRESS)
            continue; /* it is lifted: deskicons_ghost_draw draws it, above the windows */
        UiRect c = cell_rect(IC[i].col, IC[i].row);
        if (overlaps(c, clip))
            draw_icon(i, c);
    }
}

int deskicons_dragging(void) { return DRAGGING; }

UiRect deskicons_ghost_bounds(void)
{
    return ui_rect(0, 0, (int)gfx_width(), (int)gfx_height() - TASKBAR_HEIGHT);
}

void deskicons_ghost_draw(void)
{
    if (!DRAGGING || !gfx_ready())
        return;
    GfxClip cl = gfx_get_clip();
    UiRect clip = ui_rect(cl.x0, cl.y0, cl.x1 - cl.x0, cl.y1 - cl.y0);
    UiRect tc = cell_rect(TCOL, TROW); /* where it would land */
    if (overlaps(tc, clip))
        gfx_round_rect_alpha(tc.x + 4, tc.y + 2, tc.w - 8, tc.h - 4, 10, 2, gfx_rgb(255, 255, 255), 120);
    UiRect dr = dragged_rect();
    if (overlaps(dr, clip))
        draw_icon(PRESS, dr);
}

UiRect deskicons_bounds(void)
{
    ensure();
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
