/* Task Manager app
 * HarisOS has no separate processes, so a "process" here is an open window (an app). The list comes
 * from wm_list(), is refreshed once a second, and "End task" closes the selected window.
 * Up/Down select, Delete ends the task, Tab switches the tab.
 * An app uses only public headers (ui.h, wm.h, desktop.h, ...) - never desktop_internal.h. */
#include "app.h"
#include "cpuload.h"
#include "desktop.h"
#include "gfx.h"
#include "pmm.h"
#include "taskbar.h"
#include "timer.h"
#include "ui.h"
#include "wm.h"

#define TAB_H 32
#define ROW_H 28
#define HEAD_H 24
#define PAD 12
#define FOOT_H 52
#define STATUS_W 96
#define PID_W 56
#define REFRESH_MS 1000
#define SIDE_W 120
#define SIDE_ROW_H 40
#define HIST_N 60 /* history samples, one per refresh */

enum
{
    TAB_PROCESSES,
    TAB_PERFORMANCE,
    TAB_COUNT
};
static const char *const TAB_NAME[TAB_COUNT] = {"Processes", "Performance"};

enum
{
    PERF_MEMORY,
    PERF_CPU,
    PERF_DISK,
    PERF_COUNT
};
static const char *const PERF_NAME[PERF_COUNT] = {"Memory", "CPU", "Disk"};

static Window *TM_WIN;
static int TAB;
static int PERF = PERF_MEMORY;
static unsigned char MEM_HIST[HIST_N]; /* used memory in percent, oldest first */
static int MEM_HIST_N;
static unsigned char CPU_HIST[HIST_N]; /* CPU load in percent, oldest first */
static int CPU_HIST_N;
static Window *PROCS[WM_MAX_WINDOWS];
static int NPROCS;
static int SEL_ID = -1; /* id of the selected window (survives list refreshes), -1 = none */
static unsigned long long LAST_MS;

static void put_dec(char *s, int *n, unsigned v)
{
    char tmp[10];
    int k = 0;
    do
    {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k)
        s[(*n)++] = tmp[--k];
    s[*n] = 0;
}

static void put_str(char *s, int *n, const char *t)
{
    while (*t)
        s[(*n)++] = *t++;
    s[*n] = 0;
}

static void reload(void)
{
    NPROCS = wm_list(PROCS, WM_MAX_WINDOWS);
    int found = 0;
    for (int i = 0; i < NPROCS; i++)
        if (PROCS[i]->id == SEL_ID)
            found = 1;
    if (!found)
        SEL_ID = -1;
}

static int sel_index(void)
{
    for (int i = 0; i < NPROCS; i++)
        if (PROCS[i]->id == SEL_ID)
            return i;
    return -1;
}

static UiRect tab_rect(UiRect body, int i)
{
    int w = (body.w - 2 * PAD) / TAB_COUNT;
    return ui_rect(body.x + PAD + i * w, body.y + 8, w, TAB_H);
}

static UiRect list_rect(UiRect body)
{
    return ui_rect(body.x + PAD, body.y + 8 + TAB_H + 8 + HEAD_H, body.w - 2 * PAD,
                   body.h - (8 + TAB_H + 8 + HEAD_H) - FOOT_H);
}

static void draw_tabs(UiRect body)
{
    const UiTheme *t = ui_theme();
    for (int i = 0; i < TAB_COUNT; i++)
    {
        UiRect r = tab_rect(body, i);
        if (i == TAB)
        {
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 40);
            gfx_fill_rect(r.x + 8, r.y + r.h - 3, r.w - 16, 3, ui_color(t->accent));
        }
        ui_text_in_color(r, TAB_NAME[i], UI_TEXT_LABEL, UI_ALIGN_CENTER, i == TAB ? t->text : t->text_dim);
    }
}

static void draw_processes(UiRect body)
{
    const UiTheme *t = ui_theme();
    UiRect list = list_rect(body);
    int hy = list.y - HEAD_H;
    ui_text_in_color(ui_rect(list.x + 8, hy, list.w - STATUS_W - PID_W - 16, HEAD_H), "Name", UI_TEXT_SMALL,
                     UI_ALIGN_LEFT, t->text_dim);
    ui_text_in_color(ui_rect(list.x + list.w - STATUS_W - PID_W - 8, hy, STATUS_W, HEAD_H), "Status", UI_TEXT_SMALL,
                     UI_ALIGN_LEFT, t->text_dim);
    ui_text_in_color(ui_rect(list.x + list.w - PID_W - 4, hy, PID_W, HEAD_H), "ID", UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                     t->text_dim);
    ui_separator(list.x, list.y - 2, list.w);

    int vis = list.h / ROW_H;
    Window *top = wm_top();
    for (int i = 0; i < NPROCS && i < vis; i++)
    {
        const Window *p = PROCS[i];
        UiRect r = ui_rect(list.x, list.y + i * ROW_H, list.w, ROW_H);
        if (p->id == SEL_ID)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h - 2, t->radius_s, ui_color(t->accent), 40);
        ui_text_in(ui_rect(r.x + 8, r.y, r.w - STATUS_W - PID_W - 16, r.h), p->title, UI_TEXT_BODY, UI_ALIGN_LEFT);
        const char *st = p->minimized ? "Minimized" : (p == top ? "Active" : "Running");
        ui_text_in_color(ui_rect(r.x + r.w - STATUS_W - PID_W - 8, r.y, STATUS_W, r.h), st, UI_TEXT_SMALL,
                         UI_ALIGN_LEFT, p == top ? t->success : t->text_dim);
        char id[8];
        int n = 0;
        put_dec(id, &n, (unsigned)p->id);
        ui_text_in_color(ui_rect(r.x + r.w - PID_W - 4, r.y, PID_W, r.h), id, UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                         t->text_dim);
    }

    char line[32];
    int n = 0;
    put_dec(line, &n, (unsigned)NPROCS);
    put_str(line, &n, NPROCS == 1 ? " app running" : " apps running");
    ui_text_color(body.x + PAD, body.y + body.h - FOOT_H + 18, line, UI_TEXT_SMALL, t->text_dim);
}

/* "123 MB" for a page count (4 KiB pages: 256 per MiB) */
static void put_mb(char *s, int *n, unsigned long long pages)
{
    put_dec(s, n, (unsigned)(pages / 256));
    put_str(s, n, " MB");
}

static unsigned mem_used_percent(void)
{
    unsigned long long total = pmm_total_pages();
    return total ? (unsigned)((total - pmm_free_pages()) * 100 / total) : 0;
}

/* append a sample to a history, dropping the oldest when it is full */
static void hist_push(unsigned char *hist, int *n, unsigned value)
{
    if (*n == HIST_N)
    {
        for (int i = 1; i < HIST_N; i++)
            hist[i - 1] = hist[i];
        (*n)--;
    }
    hist[(*n)++] = (unsigned char)(value > 100 ? 100 : value);
}

static void take_samples(void)
{
    hist_push(MEM_HIST, &MEM_HIST_N, mem_used_percent());
    hist_push(CPU_HIST, &CPU_HIST_N, (unsigned)cpuload_percent());
}

/* history graph: one bar per sample, newest at the right */
static void draw_graph(UiRect g, const unsigned char *hist, int n)
{
    const UiTheme *t = ui_theme();
    gfx_fill_round_rect(g.x, g.y, g.w, g.h, t->radius_s, ui_color(t->surface_alt));
    int bar_w = (g.w - 4) / HIST_N;
    if (bar_w < 1)
        bar_w = 1;
    int x0 = g.x + g.w - 2 - n * bar_w;
    for (int i = 0; i < n; i++)
    {
        int h = (g.h - 4) * hist[i] / 100;
        if (h < 1)
            h = 1;
        gfx_fill_rect(x0 + i * bar_w, g.y + g.h - 2 - h, bar_w, h, ui_color(t->accent));
    }
}

static UiRect side_rect(UiRect body, int i)
{
    return ui_rect(body.x + PAD, body.y + 8 + TAB_H + 8 + i * SIDE_ROW_H, SIDE_W, SIDE_ROW_H - 4);
}

static UiRect pane_rect(UiRect body)
{
    int x = body.x + PAD + SIDE_W + PAD;
    int y = body.y + 8 + TAB_H + 8;
    return ui_rect(x, y, body.x + body.w - PAD - x, body.y + body.h - PAD - y);
}

static void draw_memory(UiRect pane)
{
    const UiTheme *t = ui_theme();
    unsigned long long total = pmm_total_pages(), free_p = pmm_free_pages();
    char line[48];
    int n = 0;

    ui_text(pane.x, pane.y, "Memory", UI_TEXT_TITLE);
    put_dec(line, &n, mem_used_percent());
    put_str(line, &n, "%");
    ui_text_in(ui_rect(pane.x + pane.w - 80, pane.y, 80, 28), line, UI_TEXT_LABEL, UI_ALIGN_RIGHT);

    UiRect g = ui_rect(pane.x, pane.y + 36, pane.w, 110);
    draw_graph(g, MEM_HIST, MEM_HIST_N);

    ui_progress(ui_rect(pane.x, g.y + g.h + 14, pane.w, 12), (int)mem_used_percent());

    int y = g.y + g.h + 40;
    n = 0;
    put_str(line, &n, "In use: ");
    put_mb(line, &n, total - free_p);
    ui_text(pane.x, y, line, UI_TEXT_BODY);
    n = 0;
    put_str(line, &n, "Available: ");
    put_mb(line, &n, free_p);
    ui_text(pane.x, y + 26, line, UI_TEXT_BODY);
    n = 0;
    put_str(line, &n, "Total: ");
    put_mb(line, &n, total);
    ui_text_color(pane.x, y + 52, line, UI_TEXT_SMALL, t->text_dim);
}

static void draw_cpu(UiRect pane)
{
    const UiTheme *t = ui_theme();
    char line[48];
    int n = 0;

    ui_text(pane.x, pane.y, "CPU", UI_TEXT_TITLE);
    put_dec(line, &n, (unsigned)cpuload_percent());
    put_str(line, &n, "%");
    ui_text_in(ui_rect(pane.x + pane.w - 80, pane.y, 80, 28), line, UI_TEXT_LABEL, UI_ALIGN_RIGHT);

    UiRect g = ui_rect(pane.x, pane.y + 36, pane.w, 110);
    draw_graph(g, CPU_HIST, CPU_HIST_N);
    ui_progress(ui_rect(pane.x, g.y + g.h + 14, pane.w, 12), cpuload_percent());

    int y = g.y + g.h + 40;
    ui_text(pane.x, y, "Busy share of the main loop over the last second.", UI_TEXT_BODY);
    ui_text_color(pane.x, y + 26, "Counts input handling and redraws; waiting for events is idle.",
                  UI_TEXT_SMALL, t->text_dim);
}

static void draw_performance(UiRect body)
{
    const UiTheme *t = ui_theme();
    for (int i = 0; i < PERF_COUNT; i++)
    {
        UiRect r = side_rect(body, i);
        if (i == PERF)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 40);
        ui_text_in_color(ui_rect(r.x + 10, r.y, r.w - 10, r.h), PERF_NAME[i], UI_TEXT_BODY, UI_ALIGN_LEFT,
                         i == PERF ? t->text : t->text_dim);
    }
    UiRect pane = pane_rect(body);
    if (PERF == PERF_MEMORY)
        draw_memory(pane);
    else if (PERF == PERF_CPU)
        draw_cpu(pane);
    else
        ui_text_in(pane, "Coming next", UI_TEXT_DIM, UI_ALIGN_CENTER);
}

static void tm_draw(Window *w, UiRect body)
{
    (void)w;
    draw_tabs(body);
    if (TAB == TAB_PROCESSES)
        draw_processes(body);
    else
        draw_performance(body);
}

static void end_selected(void)
{
    int i = sel_index();
    if (i < 0)
        return;
    Window *p = PROCS[i];
    SEL_ID = -1;
    if (p == TM_WIN)
        return; /* ending ourselves would free the window under the running handler; use the close button */
    wm_close(p);
    reload();
}

static void on_end_task(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    end_selected();
}

static int tm_press(Window *w, int x, int y)
{
    UiRect body = wm_content(w);
    for (int i = 0; i < TAB_COUNT; i++)
    {
        if (ui_contains(tab_rect(body, i), x, y))
        {
            int changed = TAB != i;
            TAB = i;
            return changed;
        }
    }
    if (TAB == TAB_PERFORMANCE)
    {
        for (int i = 0; i < PERF_COUNT; i++)
        {
            if (ui_contains(side_rect(body, i), x, y))
            {
                int changed = PERF != i;
                PERF = i;
                return changed;
            }
        }
        return 0;
    }
    UiRect list = list_rect(body);
    if (!ui_contains(list, x, y))
        return 0;
    int row = (y - list.y) / ROW_H;
    if (row < 0 || row >= NPROCS || PROCS[row]->id == SEL_ID)
        return 0;
    SEL_ID = PROCS[row]->id;
    return 1;
}

static void tm_key(Window *w, const KeyEvent *k)
{
    (void)w;
    if (!k->pressed)
        return;
    switch (k->key)
    {
    case KEY_TAB:
        TAB = (TAB + 1) % TAB_COUNT;
        break;
    case KEY_UP:
    case KEY_DOWN:
    {
        if (TAB != TAB_PROCESSES || NPROCS == 0)
            break;
        int i = sel_index();
        i = (k->key == KEY_DOWN) ? i + 1 : (i < 0 ? 0 : i - 1);
        if (i < 0)
            i = 0;
        if (i >= NPROCS)
            i = NPROCS - 1;
        SEL_ID = PROCS[i]->id;
        break;
    }
    case KEY_DELETE:
        if (TAB == TAB_PROCESSES)
            end_selected();
        break;
    case KEY_ESC:
        wm_close(TM_WIN);
        break;
    default:
        break;
    }
}

/* once a second: windows opened or closed elsewhere show up without a click */
static int tm_tick(Window *w)
{
    (void)w;
    unsigned long long now = timer_ms();
    if (now - LAST_MS < REFRESH_MS)
        return 0;
    LAST_MS = now;
    int old_n = NPROCS;
    Window *old[WM_MAX_WINDOWS];
    for (int i = 0; i < old_n; i++)
        old[i] = PROCS[i];
    reload();
    take_samples(); /* keep sampling while the other tab is shown, so the graph has no gap */
    if (TAB == TAB_PERFORMANCE)
        return 1;
    if (old_n != NPROCS)
        return 1;
    for (int i = 0; i < NPROCS; i++)
        if (old[i] != PROCS[i])
            return 1;
    return 0;
}

static void tm_close(Window *w)
{
    (void)w;
    TM_WIN = 0;
}

static void taskmgr_open(void)
{
    if (TM_WIN)
    {
        wm_activate(TM_WIN);
        return;
    }
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 520, h = 420;
    if (w > W - 20)
        w = W - 20;
    if (h > H - TASKBAR_HEIGHT - 8)
        h = H - TASKBAR_HEIGHT - 8;
    int x = (W - w) / 2, y = (H - TASKBAR_HEIGHT - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;

    Window *win = wm_create("Task Manager", x, y, w, h, tm_draw, 0);
    if (!win)
    {
        desktop_set_status("Window limit reached - close one first.");
        wm_invalidate();
        return;
    }
    win->on_key = tm_key;
    win->on_press = tm_press;
    win->on_tick = tm_tick;
    win->on_close = tm_close;
    win->icon = TASKBAR_ICON_SETTINGS;
    TM_WIN = win;
    TAB = TAB_PROCESSES;
    SEL_ID = -1;
    PERF = PERF_MEMORY;
    LAST_MS = timer_ms();
    MEM_HIST_N = 0;
    CPU_HIST_N = 0;
    take_samples();
    reload();
    desktop_set_status(0);

    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    wm_add_button_ex(win, "End task", UI_BTN_DANGER, ui_rect(cw - PAD - 110, ch - 8 - 36, 110, 36),
                     WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_end_task, 0);
}

const AppInfo APP_TASKMGR = {
    .name = "Task Manager",
    .desc = "Processes and performance",
    .icon = UI_ICON_SETTINGS,
    .open = taskmgr_open,
};
