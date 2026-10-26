/* Task Manager app
 * HarisOS has no separate processes, so a "process" here is an open window (an app). The list comes
 * from wm_list(), is refreshed once a second, and "End task" closes the selected window.
 * Up/Down select, Delete ends the task, Tab switches the tab. The Threads tab lists the kernel threads (task.h).
 * An app uses only public headers (ui.h, wm.h, desktop.h, ...) - never desktop_internal.h. */
#include "app.h"
#include "blk.h"
#include "cpuinfo.h"
#include "cpuload.h"
#include "desktop.h"
#include "gfx.h"
#include "heap.h"
#include "pmm.h"
#include "smp.h"
#include "task.h"
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
#define DISK_ROW_H 38
#define MEM_FREE PMM_TAG_COUNT /* the breakdown has one more part than there are tags: the free pages */
#define MEM_PARTS (PMM_TAG_COUNT + 1)
#define HIST_N 60 /* history samples, one per refresh */

enum
{
    TAB_PROCESSES,
    TAB_PERFORMANCE,
    TAB_THREADS,
    TAB_COUNT
};
static const char *const TAB_NAME[TAB_COUNT] = {"Processes", "Performance", "Threads"};
static const UiIcon TAB_ICON[TAB_COUNT] = {UI_ICON_LIST, UI_ICON_CHART, UI_ICON_CPU};

enum
{
    PERF_MEMORY,
    PERF_CPU,
    PERF_DISK,
    PERF_COUNT
};
static const char *const PERF_NAME[PERF_COUNT] = {"Memory", "CPU", "Disk"};
static const UiIcon PERF_ICON[PERF_COUNT] = {UI_ICON_MEMORY, UI_ICON_CPU, UI_ICON_DISK};

static Window *TM_WIN;
static int TAB;
static int PERF = PERF_MEMORY;
static unsigned char MEM_HIST[HIST_N]; /* used memory in percent, oldest first */
static int MEM_HIST_N;
static unsigned char CPU_HIST[HIST_N]; /* CPU load in percent, oldest first */
static int CPU_HIST_N;
static unsigned DISK_HIST[HIST_N]; /* disk throughput (read + write) in KB/s, oldest first */
static int DISK_HIST_N;
static BlkStats DISK_PREV; /* counters at the last sample, to get the change per second */
static unsigned DISK_READ_KBS, DISK_WRITE_KBS;
static unsigned long long DPREV_R[BLK_MAX], DPREV_W[BLK_MAX]; /* the same per disk (index in the blk table) */
static unsigned DKBS_R[BLK_MAX], DKBS_W[BLK_MAX];
static Window **PROCS; /* the open windows (wm_list), a heap block of PROCS_CAP pointers */
static int PROCS_CAP;
static int NPROCS;
static Window **PREV; /* the list before the last refresh, to see whether it changed */
static int PREV_CAP;
static void revalidate(void); /* (defined below) */
static int SEL_ID = -1; /* id of the selected window (survives list refreshes), -1 = none */
static unsigned long long LAST_MS;
#define TM_THREADS 256 /* threads whose CPU figure is kept (the lowest ids) */
static unsigned long long T_PREV[TM_THREADS]; /* task_run_ticks at the last sample */
static unsigned long long T_ALL_PREV[CPULOAD_MAX_CPUS]; /* each processor's tick count at the last sample */
static int T_PCT[TM_THREADS];                 /* tenths of a percent of the capacity of all processors, in the last interval */

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

/* Room for at least `need` pointers in *arr (grown by doubling); 0 if the heap is out of memory. */
static int grow(Window ***arr, int *cap, int need)
{
    if (need <= *cap)
        return 1;
    int n = *cap ? *cap : 16;
    while (n < need)
        n *= 2;
    Window **p = krealloc(*arr, (unsigned long long)n * sizeof(Window *));
    if (!p)
        return 0;
    *arr = p;
    *cap = n;
    return 1;
}

static void reload(void)
{
    NPROCS = grow(&PROCS, &PROCS_CAP, wm_count()) ? wm_list(PROCS, PROCS_CAP) : 0;
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
        UiRgb fg = i == TAB ? t->text : t->text_dim;
        /* icon + label as one group, centered in the tab */
        int isz = 18, gap = 8;
        int gw = isz + gap + ui_text_width(TAB_NAME[i], UI_TEXT_LABEL);
        int gx = r.x + (r.w - gw) / 2;
        if (gx < r.x + 4)
            gx = r.x + 4;
        ui_icon(TAB_ICON[i], gx + isz / 2, r.y + (r.h - 3) / 2, isz, fg);
        ui_text_in_color(ui_rect(gx + isz + gap, r.y, r.x + r.w - (gx + isz + gap), r.h), TAB_NAME[i],
                         UI_TEXT_LABEL, UI_ALIGN_LEFT, fg);
    }
}

/* ---- CPU figures as Windows shows them: a share of the capacity of ALL processors (one processor fully busy of four = 25%) ---- */

/* processors that run (the boot processor and the application processors that came up), at least 1 */
static int ncpus(void)
{
    int n = 0;
    for (int i = 0; i < smp_cpu_total(); i++)
        if (smp_cpu_online(i))
            n++;
    return n ? n : 1;
}

/* the average load of the running processors in percent: the boot processor's is the main loop's busy share, the others' the share of ticks
 * that found them running a user thread (cpuload.h) */
static int total_cpu_percent(void)
{
    int sum = cpuload_percent();
    for (int i = 1; i < smp_cpu_total(); i++)
        if (smp_cpu_online(i))
            sum += cpuload_cpu_percent(i);
    return sum / ncpus();
}

/* tenths of a percent: "0.3%", "7.5%", "42%" (one decimal only below 10%, where whole numbers would hide small apps) */
static void put_pct10(char *s, int *n, unsigned tenths)
{
    if (tenths >= 100)
        put_dec(s, n, (tenths + 5) / 10);
    else
    {
        put_dec(s, n, tenths / 10);
        put_str(s, n, ".");
        put_dec(s, n, tenths % 10);
    }
    put_str(s, n, "%");
}

static void draw_processes(UiRect body)
{
    const UiTheme *t = ui_theme();
    UiRect list = list_rect(body);
    int hy = list.y - HEAD_H;
    /* columns from the right: ID, the app's CPU share, the status, then the name */
    const int CPU_W = 48;
    const int x_cpu = list.x + list.w - PID_W - 4 - CPU_W, x_state = x_cpu - STATUS_W;
    ui_text_in_color(ui_rect(list.x + 8, hy, x_state - list.x - 16, HEAD_H), "Name", UI_TEXT_SMALL, UI_ALIGN_LEFT,
                     t->text_dim);
    ui_text_in_color(ui_rect(x_state, hy, STATUS_W, HEAD_H), "Status", UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    ui_text_in_color(ui_rect(x_cpu, hy, CPU_W, HEAD_H), "CPU", UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
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
        ui_text_in(ui_rect(r.x + 8, r.y, x_state - list.x - 16, r.h), p->title, UI_TEXT_BODY, UI_ALIGN_LEFT);
        const char *st = p->minimized ? "Minimized" : (p == top ? "Active" : "Running");
        ui_text_in_color(ui_rect(x_state, r.y, STATUS_W, r.h), st, UI_TEXT_SMALL, UI_ALIGN_LEFT,
                         p == top ? t->success : t->text_dim);
        char pct[8];
        int pn = 0;
        put_pct10(pct, &pn, (unsigned)p->cpu_pct);
        ui_text_in_color(ui_rect(x_cpu, r.y, CPU_W, r.h), pct, UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                         p->cpu_pct > 0 ? t->text : t->text_dim);
        char id[8];
        int n = 0;
        put_dec(id, &n, (unsigned)p->id);
        ui_text_in_color(ui_rect(r.x + r.w - PID_W - 4, r.y, PID_W, r.h), id, UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                         t->text_dim);
    }

    /* The apps' figures count only their own code (drawing, input, timers). Everything else the main loop does, the desktop, the taskbar, the
     * window frames, the drivers, has no window: it is one more row, so that the column adds up to the processor's load (the CPU page).
     * The two figures come from slightly different one-second windows, so the rest is clamped at 0. */
    int apps_sum = 0;
    for (int i = 0; i < NPROCS; i++)
        apps_sum += PROCS[i]->cpu_pct;
    int main_loop = cpuload_percent() * 10 / ncpus(); /* the main loop's busy share, as a share of all processors, in tenths */
    int total = total_cpu_percent();
    int rest = main_loop > apps_sum ? main_loop - apps_sum : 0;
    if (NPROCS < vis)
    {
        UiRect r = ui_rect(list.x, list.y + NPROCS * ROW_H, list.w, ROW_H);
        ui_text_in_color(ui_rect(r.x + 8, r.y, x_state - list.x - 16, r.h), "Desktop and system", UI_TEXT_BODY, UI_ALIGN_LEFT,
                         t->text_dim);
        char pct[8];
        int pn = 0;
        put_pct10(pct, &pn, (unsigned)rest);
        ui_text_in_color(ui_rect(x_cpu, r.y, CPU_W, r.h), pct, UI_TEXT_SMALL, UI_ALIGN_RIGHT, rest > 0 ? t->text : t->text_dim);
    }

    char line[64];
    int n = 0;
    put_dec(line, &n, (unsigned)NPROCS);
    put_str(line, &n, NPROCS == 1 ? " app running, " : " apps running, ");
    put_dec(line, &n, (unsigned)total);
    put_str(line, &n, "% of all processors");
    ui_text_color(body.x + PAD, body.y + body.h - FOOT_H + 18, line, UI_TEXT_SMALL, t->text_dim);
}

/* Kernel threads (task.h): the main loop is thread 0; the others are background workers. Read-only view, redrawn
 * with the once-a-second refresh. Names are the ones given to task_create(). */
static void draw_threads(UiRect body)
{
    const UiTheme *t = ui_theme();
    UiRect list = list_rect(body);
    int hy = list.y - HEAD_H;
    /* columns from the right: ID, the processor it last ran on, its CPU share, the state, then the name */
    const int CPU_W = 48, CORE_W = 52;
    const int x_core = list.x + list.w - PID_W - 4 - CORE_W, x_cpu = x_core - CPU_W, x_state = x_cpu - STATUS_W;
    ui_text_in_color(ui_rect(list.x + 8, hy, x_state - list.x - 16, HEAD_H), "Name", UI_TEXT_SMALL, UI_ALIGN_LEFT,
                     t->text_dim);
    ui_text_in_color(ui_rect(x_state, hy, STATUS_W, HEAD_H), "State", UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    ui_text_in_color(ui_rect(x_cpu, hy, CPU_W, HEAD_H), "CPU", UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
    ui_text_in_color(ui_rect(x_core, hy, CORE_W, HEAD_H), "Core", UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
    ui_text_in_color(ui_rect(list.x + list.w - PID_W - 4, hy, PID_W, HEAD_H), "ID", UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                     t->text_dim);
    ui_separator(list.x, list.y - 2, list.w);

    int vis = list.h / ROW_H, row = 0, live = 0;
    for (int id = 0; id < task_slots(); id++)
    {
        const char *name = task_name(id);
        TaskState st = task_state(id);
        if (!name || st == TASK_DEAD)
            continue;
        live++;
        if (row >= vis)
            continue; /* no room: still counted below */
        UiRect r = ui_rect(list.x, list.y + row * ROW_H, list.w, ROW_H);
        row++;
        ui_text_in(ui_rect(r.x + 8, r.y, x_state - list.x - 16, r.h), name, UI_TEXT_BODY, UI_ALIGN_LEFT);
        const char *label = st == TASK_RUNNING    ? "Running"
                            : st == TASK_SLEEPING ? "Sleeping"
                            : st == TASK_BLOCKED  ? "Waiting"
                                                  : "Ready";
        ui_text_in_color(ui_rect(x_state, r.y, STATUS_W, r.h), label, UI_TEXT_SMALL, UI_ALIGN_LEFT,
                         st == TASK_RUNNING ? t->success : t->text_dim);
        char cols[12];
        int cn = 0;
        if (id < TM_THREADS)
        {
            put_pct10(cols, &cn, (unsigned)T_PCT[id]);
        }
        else
            put_str(cols, &cn, "-");
        ui_text_in_color(ui_rect(x_cpu, r.y, CPU_W, r.h), cols, UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                         T_PCT[id < TM_THREADS ? id : 0] > 0 && id < TM_THREADS ? t->text : t->text_dim);
        cn = 0;
        cols[0] = 0;
        if (task_last_cpu(id) >= 0)
        {
            put_str(cols, &cn, "CPU ");
            put_dec(cols, &cn, (unsigned)task_last_cpu(id));
        }
        else
            put_str(cols, &cn, "-");
        ui_text_in_color(ui_rect(x_core, r.y, CORE_W, r.h), cols, UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text_dim);
        char id_s[8];
        int n = 0;
        put_dec(id_s, &n, (unsigned)id);
        ui_text_in_color(ui_rect(r.x + r.w - PID_W - 4, r.y, PID_W, r.h), id_s, UI_TEXT_SMALL, UI_ALIGN_RIGHT,
                         t->text_dim);
    }

    char line[64];
    int n = 0;
    put_dec(line, &n, (unsigned)live);
    put_str(line, &n, live == 1 ? " thread, " : " threads, ");
    put_dec(line, &n, (unsigned)task_switches());
    put_str(line, &n, " switches");
    ui_text_color(body.x + PAD, body.y + body.h - FOOT_H + 18, line, UI_TEXT_SMALL, t->text_dim);
}

/* "123 MB" for a page count (4 KiB pages: 256 per MiB) */
static void put_mb(char *s, int *n, unsigned long long pages)
{
    put_dec(s, n, (unsigned)(pages / 256));
    put_str(s, n, " MB");
}

/* "812 KB" below 1 MB, else "42 MB" (small owners such as USB are only a few pages) */
static void put_pages(char *s, int *n, unsigned long long pages)
{
    if (pages < 256)
    {
        put_dec(s, n, (unsigned)(pages * 4));
        put_str(s, n, " KB");
    }
    else
        put_mb(s, n, pages);
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

/* The disk history keeps KB/s (not a percent), so the graph can scale itself to the busiest second. */
/* KB/s from a byte counter; a counter that went down (the slot got a new drive) counts as no I/O */
static unsigned rate_kbs(unsigned long long now, unsigned long long prev, unsigned long long dt_ms)
{
    return (dt_ms && now >= prev) ? (unsigned)((now - prev) * 1000 / dt_ms / 1024) : 0;
}

static void disk_sample(unsigned long long dt_ms)
{
    for (int i = 0; i < BLK_MAX; i++)
    {
        const BlkDev *d = blk_get(i);
        if (!d || d->parent >= 0 || d->removed)
        {
            DPREV_R[i] = DPREV_W[i] = 0;
            DKBS_R[i] = DKBS_W[i] = 0;
            continue;
        }
        DKBS_R[i] = rate_kbs(d->read_bytes, DPREV_R[i], dt_ms);
        DKBS_W[i] = rate_kbs(d->write_bytes, DPREV_W[i], dt_ms);
        DPREV_R[i] = d->read_bytes;
        DPREV_W[i] = d->write_bytes;
    }

    BlkStats now;
    blk_get_stats(&now);
    if (dt_ms == 0) /* the first sample: just remember the counters, nothing to compare with yet */
    {
        DISK_PREV = now;
        DISK_READ_KBS = DISK_WRITE_KBS = 0;
    }
    else
    {
        DISK_READ_KBS = (unsigned)((now.read_bytes - DISK_PREV.read_bytes) * 1000 / dt_ms / 1024);
        DISK_WRITE_KBS = (unsigned)((now.write_bytes - DISK_PREV.write_bytes) * 1000 / dt_ms / 1024);
        DISK_PREV = now;
    }
    if (DISK_HIST_N == HIST_N)
    {
        for (int i = 1; i < HIST_N; i++)
            DISK_HIST[i - 1] = DISK_HIST[i];
        DISK_HIST_N--;
    }
    DISK_HIST[DISK_HIST_N++] = DISK_READ_KBS + DISK_WRITE_KBS;
}

/* dt_ms = time since the previous sample, 0 for the first one */
/* Per app (window): the microseconds its own callbacks took in the last interval (Window.cpu_us, counted by the window manager) as a share of
 * the interval. This is the time spent in the app's code (drawing, input, timers); the desktop, the taskbar and the window frames are not
 * included. The first call (dt_ms = 0) only takes the starting figures. */
static void app_samples(unsigned long long dt_ms)
{
    Window *all[128];
    int n = wm_list(all, 128);
    for (int i = 0; i < n; i++)
    {
        Window *w = all[i];
        unsigned long long delta = w->cpu_us >= w->cpu_prev_us ? w->cpu_us - w->cpu_prev_us : 0;
        unsigned long long p = dt_ms ? delta * 1000 / (dt_ms * 1000 * (unsigned long long)ncpus()) : 0; /* tenths of a percent of all processors */
        w->cpu_pct = p > 1000 ? 1000 : (int)p;
        w->cpu_prev_us = w->cpu_us;
    }
}

/* Per thread: the ticks it was found running in the last interval, as a share of ALL the ticks the processor it last ran on took in that
 * interval (the tick rate is not exactly 1000 per second, so milliseconds would be the wrong yardstick). A thread runs on one processor at a
 * time, so 100 is the most. Thread 0 is the main loop, which sleeps in HLT: it shows its own busy share instead. The first call (dt_ms = 0)
 * only takes the starting figures. */
static void thread_samples(unsigned long long dt_ms)
{
    unsigned long long all_delta[CPULOAD_MAX_CPUS] = {0};
    int ncpu = smp_cpu_total() < CPULOAD_MAX_CPUS ? smp_cpu_total() : CPULOAD_MAX_CPUS;
    for (int c = 0; c < ncpu; c++)
    {
        unsigned long long busy, all;
        task_cpu_ticks(c, &busy, &all);
        all_delta[c] = all >= T_ALL_PREV[c] ? all - T_ALL_PREV[c] : 0;
        T_ALL_PREV[c] = all;
    }
    int n = task_slots() < TM_THREADS ? task_slots() : TM_THREADS;
    for (int id = 0; id < n; id++)
    {
        unsigned long long ticks = task_run_ticks(id);
        unsigned long long delta = ticks >= T_PREV[id] ? ticks - T_PREV[id] : ticks; /* a slot used by a new thread starts again at 0 */
        int cpu = task_last_cpu(id);
        unsigned long long all = cpu >= 0 && cpu < ncpu ? all_delta[cpu] : 0;
        unsigned long long p = dt_ms && all ? delta * 1000 / (all * (unsigned long long)ncpus()) : 0; /* tenths of a percent of all processors */
        T_PCT[id] = p > 1000 ? 1000 : (int)p;
        T_PREV[id] = ticks;
    }
    T_PCT[0] = cpuload_percent() * 10 / ncpus();
}

static void take_samples(unsigned long long dt_ms)
{
    hist_push(MEM_HIST, &MEM_HIST_N, mem_used_percent());
    hist_push(CPU_HIST, &CPU_HIST_N, (unsigned)total_cpu_percent());
    thread_samples(dt_ms);
    app_samples(dt_ms);
    disk_sample(dt_ms);
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

    UiRect g = ui_rect(pane.x, pane.y + 36, pane.w, 80);
    draw_graph(g, MEM_HIST, MEM_HIST_N);

    n = 0;
    put_str(line, &n, "In use ");
    put_pages(line, &n, total - free_p);
    put_str(line, &n, " of ");
    put_pages(line, &n, total);
    ui_text_color(pane.x, g.y + g.h + 6, line, UI_TEXT_SMALL, t->text_dim);

    /* breakdown: the used part is split by owner (see PmmTag); "Other" is whatever the tags do not explain */
    unsigned long long part[MEM_PARTS];
    unsigned long long known = 0;
    for (int i = 0; i < PMM_TAG_COUNT; i++)
        if (i != PMM_TAG_OTHER)
        {
            part[i] = pmm_tag_pages((PmmTag)i);
            known += part[i];
        }
    part[PMM_TAG_OTHER] = (total - free_p > known) ? total - free_p - known : 0;
    part[MEM_FREE] = free_p;

    UiRgb col[MEM_PARTS];
    col[MEM_FREE] = t->surface_alt;
    col[PMM_TAG_SYSTEM] = t->text_dim;
    col[PMM_TAG_GRAPHICS] = t->accent;
    col[PMM_TAG_USB] = t->success;
    col[PMM_TAG_STORAGE] = t->warning;
    col[PMM_TAG_HEAP] = t->text;
    col[PMM_TAG_OTHER] = t->danger;
    static const char *const name[MEM_PARTS] = {"Other", "System tables", "Graphics buffers", "USB", "Storage", "Kernel heap", "Free"};

    UiRect bar = ui_rect(pane.x, g.y + g.h + 30, pane.w, 14);
    gfx_fill_round_rect(bar.x, bar.y, bar.w, bar.h, 4, ui_color(t->surface_alt));
    unsigned long long cum = 0;
    for (int i = 0; i < MEM_FREE && total; i++)
    {
        int x0 = bar.x + (int)(cum * (unsigned long long)bar.w / total);
        cum += part[i];
        int x1 = bar.x + (int)(cum * (unsigned long long)bar.w / total);
        if (x1 > x0)
            gfx_fill_rect(x0, bar.y, x1 - x0, bar.h, ui_color(col[i]));
    }

    int y = bar.y + bar.h + 14;
    static const int order[MEM_PARTS] = {PMM_TAG_GRAPHICS, PMM_TAG_SYSTEM, PMM_TAG_USB, PMM_TAG_STORAGE,
                                         PMM_TAG_HEAP, PMM_TAG_OTHER, MEM_FREE};
    for (int k = 0; k < MEM_PARTS; k++)
    {
        int i = order[k];
        gfx_fill_circle(pane.x + 6, y + 10, 5, ui_color(col[i]));
        ui_text_in(ui_rect(pane.x + 20, y, pane.w - 120, 22), name[i], UI_TEXT_BODY, UI_ALIGN_LEFT);
        n = 0;
        line[0] = 0;
        put_pages(line, &n, part[i]);
        ui_text_in_color(ui_rect(pane.x + pane.w - 100, y, 100, 22), line, UI_TEXT_BODY, UI_ALIGN_RIGHT, t->text);
        y += 22;
    }
}

static void draw_cpu(UiRect pane)
{
    const UiTheme *t = ui_theme();
    char line[48];
    int n = 0;

    ui_text(pane.x, pane.y, "CPU", UI_TEXT_TITLE);
    put_dec(line, &n, (unsigned)total_cpu_percent());
    put_str(line, &n, "%");
    ui_text_in(ui_rect(pane.x + pane.w - 80, pane.y, 80, 28), line, UI_TEXT_LABEL, UI_ALIGN_RIGHT);

    UiRect g = ui_rect(pane.x, pane.y + 36, pane.w, 80);
    draw_graph(g, CPU_HIST, CPU_HIST_N);
    ui_progress(ui_rect(pane.x, g.y + g.h + 10, pane.w, 12), total_cpu_percent());
    ui_text_color(pane.x, g.y + g.h + 28, "Load = average of the processors (main loop on CPU 0, user programs on the others)", UI_TEXT_SMALL,
                  t->text_dim);

    const CpuInfo *c = cpuinfo_get();
    int y = g.y + g.h + 56;
    if (c->brand[0])
        ui_text_in(ui_rect(pane.x, y, pane.w, 22), c->brand, UI_TEXT_LABEL, UI_ALIGN_LEFT);
    y += 30;

    /* label on the left, value next to it (a CPU the probe could not read shows "-") */
    const int vx = pane.x + 150;
    static const char *const label[4] = {"Base speed", "Sockets", "Cores", "Logical processors"};
    for (int i = 0; i < 4; i++)
    {
        n = 0;
        line[0] = 0;
        unsigned v = i == 1 ? (unsigned)c->sockets : i == 2 ? (unsigned)c->cores : (unsigned)c->logical;
        if (i == 0)
        {
            if (c->base_mhz >= 1000)
            {
                put_dec(line, &n, c->base_mhz / 1000);
                put_str(line, &n, ".");
                put_dec(line, &n, (c->base_mhz % 1000) / 10 / 10);
                put_dec(line, &n, (c->base_mhz % 1000) / 10 % 10);
                put_str(line, &n, " GHz");
            }
            else if (c->base_mhz)
            {
                put_dec(line, &n, c->base_mhz);
                put_str(line, &n, " MHz");
            }
            else
                put_str(line, &n, "-");
            if (c->base_estimated)
                put_str(line, &n, " (measured)");
        }
        else if (v)
            put_dec(line, &n, v);
        else
            put_str(line, &n, "-");
        ui_text_color(pane.x, y, label[i], UI_TEXT_BODY, t->text_dim);
        ui_text(vx, y, line, UI_TEXT_BODY);
        y += 24;
    }

    /* One bar for every processor the kernel started (smp.h): the boot processor's is the main loop's busy share, the others' the share
     * of their timer ticks that found them running a user thread (cpuload.h). Rows that do not fit in the pane are left out. */
    int ncpu = smp_cpu_total();
    if (ncpu > 1)
    {
        ui_text_color(pane.x, y, "Per processor", UI_TEXT_BODY, t->text_dim);
        y += 22;
        for (int i = 0; i < ncpu && y + 18 <= pane.y + pane.h; i++)
        {
            int pct = i == 0 ? cpuload_percent() : cpuload_cpu_percent(i);
            n = 0;
            line[0] = 0;
            put_str(line, &n, "CPU ");
            put_dec(line, &n, (unsigned)i);
            ui_text_color(pane.x, y, line, UI_TEXT_SMALL, smp_cpu_online(i) ? t->text : t->text_dim);
            n = 0;
            line[0] = 0;
            if (smp_cpu_online(i))
            {
                put_dec(line, &n, (unsigned)pct);
                put_str(line, &n, "%");
            }
            else
                put_str(line, &n, "off");
            ui_text_in(ui_rect(pane.x + pane.w - 50, y, 50, 16), line, UI_TEXT_SMALL, UI_ALIGN_RIGHT);
            ui_progress(ui_rect(pane.x + 60, y + 2, pane.w - 120, 12), smp_cpu_online(i) ? pct : 0);
            y += 18;
        }
    }
}

/* "512 KB/s" or "3.2 MB/s" */
static void put_rate(char *s, int *n, unsigned kbs)
{
    if (kbs < 1024)
    {
        put_dec(s, n, kbs);
        put_str(s, n, " KB/s");
        return;
    }
    put_dec(s, n, kbs / 1024);
    put_str(s, n, ".");
    put_dec(s, n, (kbs % 1024) * 10 / 1024);
    put_str(s, n, " MB/s");
}

/* total bytes as "812 KB" or "42 MB" */
static void put_size(char *s, int *n, unsigned long long bytes)
{
    if (bytes < 1024ULL * 1024)
    {
        put_dec(s, n, (unsigned)(bytes / 1024));
        put_str(s, n, " KB");
    }
    else
    {
        put_dec(s, n, (unsigned)(bytes >> 20));
        put_str(s, n, " MB");
    }
}

static void draw_disk(UiRect pane)
{
    const UiTheme *t = ui_theme();
    char line[64];
    int n = 0;

    ui_text(pane.x, pane.y, "Disk", UI_TEXT_TITLE);
    put_rate(line, &n, DISK_READ_KBS + DISK_WRITE_KBS);
    ui_text_in(ui_rect(pane.x + pane.w - 120, pane.y, 120, 28), line, UI_TEXT_LABEL, UI_ALIGN_RIGHT);

    /* the graph scales to the busiest second, but never below 1 MB/s so a few KB do not fill it */
    unsigned peak = 1024;
    for (int i = 0; i < DISK_HIST_N; i++)
        if (DISK_HIST[i] > peak)
            peak = DISK_HIST[i];
    unsigned char pct[HIST_N];
    for (int i = 0; i < DISK_HIST_N; i++)
        pct[i] = (unsigned char)((unsigned long long)DISK_HIST[i] * 100 / peak);
    UiRect g = ui_rect(pane.x, pane.y + 36, pane.w, 80);
    draw_graph(g, pct, DISK_HIST_N);
    n = 0;
    put_str(line, &n, "Graph scale: ");
    put_rate(line, &n, peak);
    ui_text_color(pane.x, g.y + g.h + 4, line, UI_TEXT_SMALL, t->text_dim);

    BlkStats st;
    blk_get_stats(&st);
    int y = g.y + g.h + 26;
    n = 0;
    put_str(line, &n, "Read ");
    put_size(line, &n, st.read_bytes);
    put_str(line, &n, "   Written ");
    put_size(line, &n, st.write_bytes);
    put_str(line, &n, " (since boot)");
    ui_text_color(pane.x, y, line, UI_TEXT_SMALL, t->text_dim);

    /* one row per disk (partitions are counted on their disk): name, model and type, then its speed */
    y += 26;
    ui_separator(pane.x, y - 4, pane.w);
    int shown = 0, disks = 0;
    for (int i = 0; i < blk_count(); i++)
    {
        const BlkDev *d = blk_get(i);
        if (d->parent >= 0 || d->removed)
            continue;
        disks++;
        if (y + DISK_ROW_H > pane.y + pane.h)
            continue; /* no room: counted for the "more" line */
        n = 0;
        put_str(line, &n, d->name);
        put_str(line, &n, d->media == BLK_MEDIA_SSD ? "  SSD" : (d->media == BLK_MEDIA_HDD ? "  HDD" : ""));
        ui_text_in(ui_rect(pane.x, y, pane.w / 2, DISK_ROW_H / 2 + 2), line, UI_TEXT_BODY, UI_ALIGN_LEFT);
        ui_text_in_color(ui_rect(pane.x, y + DISK_ROW_H / 2, pane.w / 2, DISK_ROW_H / 2), d->model[0] ? d->model : blk_kind_name(d->kind),
                         UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
        n = 0;
        put_str(line, &n, "R ");
        put_rate(line, &n, DKBS_R[i]);
        ui_text_in_color(ui_rect(pane.x + pane.w / 2, y, pane.w - pane.w / 2, DISK_ROW_H / 2 + 2), line, UI_TEXT_SMALL,
                         UI_ALIGN_RIGHT, t->text);
        n = 0;
        put_str(line, &n, "W ");
        put_rate(line, &n, DKBS_W[i]);
        ui_text_in_color(ui_rect(pane.x + pane.w / 2, y + DISK_ROW_H / 2, pane.w - pane.w / 2, DISK_ROW_H / 2), line,
                         UI_TEXT_SMALL, UI_ALIGN_RIGHT, t->text);
        y += DISK_ROW_H;
        shown++;
    }
    if (disks == 0)
        ui_text_color(pane.x, y, "No disks found", UI_TEXT_BODY, t->text_dim);
    else if (shown < disks)
    {
        n = 0;
        put_str(line, &n, "+ ");
        put_dec(line, &n, (unsigned)(disks - shown));
        put_str(line, &n, " more");
        ui_text_color(pane.x, pane.y + pane.h - 18, line, UI_TEXT_SMALL, t->text_dim);
    }
}

static void draw_performance(UiRect body)
{
    const UiTheme *t = ui_theme();
    for (int i = 0; i < PERF_COUNT; i++)
    {
        UiRect r = side_rect(body, i);
        if (i == PERF)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_s, ui_color(t->accent), 40);
        UiRgb fg = i == PERF ? t->text : t->text_dim;
        int isz = 20;
        ui_icon(PERF_ICON[i], r.x + 10 + isz / 2, r.y + r.h / 2, isz, fg);
        ui_text_in_color(ui_rect(r.x + 10 + isz + 8, r.y, r.w - (10 + isz + 8), r.h), PERF_NAME[i],
                         UI_TEXT_BODY, UI_ALIGN_LEFT, fg);
    }
    UiRect pane = pane_rect(body);
    if (PERF == PERF_MEMORY)
        draw_memory(pane);
    else if (PERF == PERF_CPU)
        draw_cpu(pane);
    else
        draw_disk(pane);
}

static void tm_draw(Window *w, UiRect body)
{
    revalidate();
    (void)w;
    draw_tabs(body);
    if (TAB == TAB_PROCESSES)
        draw_processes(body);
    else if (TAB == TAB_THREADS)
        draw_threads(body);
    else
        draw_performance(body);
}

/* Windows close between two refreshes of the list, and the memory of a window that has been closed for a while goes back to the heap:
 * before any use of PROCS, throw the list away if it names a window that is no longer open. (wm_is_open only compares addresses.) */
static void revalidate(void)
{
    for (int i = 0; i < NPROCS; i++)
        if (!wm_is_open(PROCS[i]))
        {
            reload();
            return;
        }
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
    revalidate();
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
    if (TAB != TAB_PROCESSES) /* the thread list cannot be selected */
        return 0;
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
    revalidate();
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
    unsigned long long now = timer_ms();
    if (now - LAST_MS < REFRESH_MS)
        return 0;
    unsigned long long dt = now - LAST_MS;
    LAST_MS = now;
    int old_n = NPROCS;
    int have_old = grow(&PREV, &PREV_CAP, old_n);
    for (int i = 0; have_old && i < old_n; i++)
        PREV[i] = PROCS[i];
    reload();
    take_samples(dt); /* keep sampling while the other tab is shown, so the graph has no gap */
    if (TAB == TAB_PERFORMANCE)
    {
        wm_add_damage(pane_rect(wm_content(w))); /* only the graphs and numbers change, not the tabs or the side list */
        return 2;
    }
    if (TAB == TAB_THREADS)
        return 1; /* threads start and end without the window manager knowing: redraw the list every second */
    if (old_n != NPROCS || !have_old)
        return 1;
    for (int i = 0; i < NPROCS; i++)
        if (PREV[i] != PROCS[i])
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
    int w = 760, h = 560;
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
    win->icon = TASKBAR_ICON_TASKMGR;
    TM_WIN = win;
    TAB = TAB_PROCESSES;
    SEL_ID = -1;
    PERF = PERF_MEMORY;
    LAST_MS = timer_ms();
    MEM_HIST_N = 0;
    CPU_HIST_N = 0;
    DISK_HIST_N = 0;
    take_samples(0);
    reload();
    desktop_set_status(0);

    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    wm_add_button_ex(win, "End task", UI_BTN_DANGER, ui_rect(cw - PAD - 110, ch - 8 - 36, 110, 36),
                     WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_end_task, 0);
}

const AppInfo APP_TASKMGR = {
    .name = "Task Manager",
    .desc = "Processes and performance",
    .icon = UI_ICON_TASKMGR,
    .open = taskmgr_open,
};
