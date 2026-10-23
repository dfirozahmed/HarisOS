/* HarisOS GUI surfaces: see include/gui.h. A table of surfaces, their pixel segments and event queues. No window code. */
#include "gui.h"
#include "heap.h"
#include "shm.h"
#include "task.h"

typedef unsigned long long u64;

typedef struct
{
    unsigned gen; /* part of the handle: changes every time the slot is destroyed */
    int owner;    /* pid of the client */
    int w, h;
    int seg;      /* shm segment of the pixels; the surface holds one reference */
    char title[GUI_TITLE_MAX];
    int title_changed;
    int has_dirty;
    int dx0, dy0, dx1, dy1; /* changed rectangle, [x0, x1) x [y0, y1) */
    GuiEvent ev[GUI_EVENTS];
    int head, count;
    int close_pending; /* a GUI_EV_CLOSE is queued and the client has not read it yet */
    int waiter;        /* thread to wake, -1 = none */
} Surface;

static Surface *S[GUI_MAX]; /* made with kmalloc when a window is created, freed when it is destroyed */
static unsigned NEXT_GEN = 1;

/* The handle is gen * 256 + slot; generations start at 1, so a valid handle is never 0 and -1 is never valid. */
static Surface *get(int hnd)
{
    if (hnd < 0)
        return 0;
    unsigned slot = (unsigned)hnd & 255u, gen = (unsigned)hnd >> 8;
    if (slot >= GUI_MAX || !S[slot] || S[slot]->gen != gen)
        return 0;
    return S[slot];
}

static int title_ok(const char *t)
{
    int n = 0;
    for (; t[n]; n++)
        if (n >= GUI_TITLE_MAX - 1 || (unsigned char)t[n] < 32 || (unsigned char)t[n] >= 127)
            return 0;
    return n > 0;
}

static void copy_title(char *dst, const char *src)
{
    int n = 0;
    for (; src[n]; n++)
        dst[n] = src[n];
    dst[n] = 0;
}

static void wake(Surface *s)
{
    if (s->waiter >= 0)
    {
        task_wake(s->waiter);
        s->waiter = -1;
    }
}

/* "gui.<generation>": the segment's name only lives for a moment (it is unlinked as soon as the surface holds it). */
static void seg_name(char *out, unsigned gen)
{
    char digits[12];
    int n = 0, k = 0;
    out[k++] = 'g';
    out[k++] = 'u';
    out[k++] = 'i';
    out[k++] = '.';
    do
    {
        digits[n++] = (char)('0' + gen % 10);
        gen /= 10;
    } while (gen && n < 11);
    while (n)
        out[k++] = digits[--n];
    out[k] = 0;
}

int gui_create(const char *title, int w, int h, int pid, int *seg)
{
    if (!title || !title_ok(title) || w < GUI_W_MIN || w > GUI_W_MAX || h < GUI_H_MIN || h > GUI_H_MAX)
        return -1;
    int slot = 0;
    while (slot < GUI_MAX && S[slot])
        slot++;
    if (slot == GUI_MAX)
        return -1;
    Surface *s = kmalloc(sizeof(Surface));
    if (!s)
        return -1;
    unsigned gen = NEXT_GEN++;
    if (NEXT_GEN > 0x7FFFFF)
        NEXT_GEN = 1;
    char name[SHM_NAME_MAX];
    seg_name(name, gen);
    int sg = shm_open(name, (u64)w * (u64)h * 4, SHM_CREATE | SHM_EXCL);
    if (sg < 0)
    {
        kfree(s);
        return -1;
    }
    shm_ref(sg);      /* the compositor's own mapping: keeps the pixels alive ... */
    shm_unlink(name); /* ... and the name is not needed again (the client is given the segment, not the name) */

    S[slot] = s;
    s->gen = gen;
    s->owner = pid;
    s->w = w;
    s->h = h;
    s->seg = sg;
    copy_title(s->title, title);
    s->title_changed = 0;
    s->has_dirty = 1; /* the first picture is shown as soon as the window exists */
    s->dx0 = s->dy0 = 0;
    s->dx1 = w;
    s->dy1 = h;
    s->head = s->count = 0;
    s->close_pending = 0;
    s->waiter = -1;
    if (seg)
        *seg = sg;
    return (int)(gen << 8) | slot;
}

void gui_destroy(int hnd)
{
    Surface *s = get(hnd);
    if (!s)
        return;
    wake(s); /* a client waiting for events now finds the handle invalid */
    shm_unref(s->seg);
    S[(unsigned)hnd & 255u] = 0;
    kfree(s);
}

void gui_release_owner(int pid)
{
    for (int i = 0; i < GUI_MAX; i++)
        if (S[i] && S[i]->owner == pid)
            gui_destroy((int)(S[i]->gen << 8) | i);
}

int gui_valid(int hnd) { return get(hnd) != 0; }

int gui_slot(int hnd) { return get(hnd) ? (int)((unsigned)hnd & 255u) : -1; }

int gui_owner(int hnd)
{
    Surface *s = get(hnd);
    return s ? s->owner : -1;
}

int gui_size(int hnd, int *w, int *h)
{
    Surface *s = get(hnd);
    if (!s)
        return 0;
    if (w)
        *w = s->w;
    if (h)
        *h = s->h;
    return 1;
}

const char *gui_title(int hnd)
{
    Surface *s = get(hnd);
    return s ? s->title : "";
}

int gui_set_title(int hnd, const char *title)
{
    Surface *s = get(hnd);
    if (!s || !title || !title_ok(title))
        return -1;
    copy_title(s->title, title);
    s->title_changed = 1;
    return 0;
}

int gui_present(int hnd, int x, int y, int w, int h)
{
    Surface *s = get(hnd);
    if (!s)
        return -1;
    if (w <= 0 || h <= 0) /* no rectangle: everything */
    {
        x = y = 0;
        w = s->w;
        h = s->h;
    }
    /* Clip in 64 bits: x + w of two huge ints must not wrap around. */
    long long x0 = x, y0 = y, x1 = (long long)x + w, y1 = (long long)y + h;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > s->w)
        x1 = s->w;
    if (y1 > s->h)
        y1 = s->h;
    if (x0 >= x1 || y0 >= y1)
        return 0; /* nothing of it is inside the surface */
    if (s->has_dirty)
    {
        if (x0 > s->dx0)
            x0 = s->dx0;
        if (y0 > s->dy0)
            y0 = s->dy0;
        if (x1 < s->dx1)
            x1 = s->dx1;
        if (y1 < s->dy1)
            y1 = s->dy1;
    }
    s->has_dirty = 1;
    s->dx0 = (int)x0;
    s->dy0 = (int)y0;
    s->dx1 = (int)x1;
    s->dy1 = (int)y1;
    return 0;
}

int gui_next_event(int hnd, GuiEvent *out)
{
    Surface *s = get(hnd);
    if (!s)
        return -1;
    if (s->count == 0)
        return 0;
    *out = s->ev[s->head];
    s->head = (s->head + 1) % GUI_EVENTS;
    s->count--;
    if (out->type == GUI_EV_CLOSE)
        s->close_pending = 0;
    return 1;
}

void gui_wait(int hnd, int task_id)
{
    Surface *s = get(hnd);
    if (s)
        s->waiter = task_id;
}

int gui_post(int hnd, const GuiEvent *ev)
{
    Surface *s = get(hnd);
    if (!s)
        return -1;
    if (ev->type == GUI_EV_MOUSE_MOVE && s->count > 0)
    {
        GuiEvent *last = &s->ev[(s->head + s->count - 1) % GUI_EVENTS];
        if (last->type == GUI_EV_MOUSE_MOVE)
        {
            *last = *ev; /* the client only needs to know where the pointer is now */
            return 1;
        }
    }
    if (s->count == GUI_EVENTS)
    {
        if (ev->type != GUI_EV_CLOSE)
            return 0;
        /* A close request must not be lost: it takes the place of the oldest event. */
        s->head = (s->head + 1) % GUI_EVENTS;
        s->count--;
    }
    s->ev[(s->head + s->count) % GUI_EVENTS] = *ev;
    s->count++;
    wake(s);
    return 1;
}

int gui_request_close(int hnd)
{
    Surface *s = get(hnd);
    if (!s || s->close_pending)
        return 0;
    GuiEvent ev = {GUI_EV_CLOSE, 0, 0, 0, 0, 0, 0, 0};
    gui_post(hnd, &ev);
    s->close_pending = 1;
    return 1;
}

int gui_handle_at(int slot)
{
    if (slot < 0 || slot >= GUI_MAX || !S[slot])
        return -1;
    return (int)(S[slot]->gen << 8) | slot;
}

int gui_take_dirty(int hnd, int *x, int *y, int *w, int *h)
{
    Surface *s = get(hnd);
    if (!s || !s->has_dirty)
        return 0;
    s->has_dirty = 0;
    *x = s->dx0;
    *y = s->dy0;
    *w = s->dx1 - s->dx0;
    *h = s->dy1 - s->dy0;
    return 1;
}

int gui_take_title_changed(int hnd)
{
    Surface *s = get(hnd);
    if (!s || !s->title_changed)
        return 0;
    s->title_changed = 0;
    return 1;
}

const unsigned *gui_pixels(int hnd, int x, int y, int *n)
{
    Surface *s = get(hnd);
    if (!s || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return 0;
    u64 off = ((u64)y * (u64)s->w + (u64)x) * 4;
    u64 page = shm_page(s->seg, (unsigned)(off / 4096));
    if (!page)
        return 0;
    u64 in_page = off % 4096;
    int run = (int)((4096 - in_page) / 4);
    if (run > s->w - x)
        run = s->w - x;
    *n = run;
    return (const unsigned *)(page + in_page);
}

int gui_in_use(void)
{
    int n = 0;
    for (int i = 0; i < GUI_MAX; i++)
        n += S[i] ? 1 : 0;
    return n;
}
