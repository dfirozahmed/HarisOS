/* Compositor: shows the surfaces of user programs (kernel/core/gui.c) as desktop windows. See include/guiapp.h.
 *
 * A program asks for a surface with SYS_GUI_CREATE; gui_app_poll() (called from the main loop) opens an ordinary window for it. The window
 * only copies the program's pixels to the screen - when the program said they changed (SYS_GUI_PRESENT) - and turns its own keys and
 * mouse into events for the program. The close button asks the program (GUI_EV_CLOSE); the window goes when the program destroys its
 * surface or ends. No window code ever runs inside a system call: the calls set flags in gui.c and this notices them on its next pass. */
#include "gui.h"
#include "guiapp.h"
#include "heap.h"
#include "taskbar.h"
#include "ui.h"
#include "wm.h"

typedef struct
{
    Window *win; /* the window of this surface slot, NULL = none yet */
    int id;      /* its Window.id: a pointer alone could be a different window by now */
    int h;       /* the handle it was opened for */
    int left;    /* is the left button down (a hover event does not say) */
} Slot;
static Slot *SL; /* per surface slot: a heap block of SL_CAP entries, grown to follow the table of gui.c */
static int SL_CAP;
static int OPENED;            /* windows opened so far: each new one starts a bit lower and further right */

static int handle_of(const Window *w) { return (int)(unsigned long long)w->user; }

/* The left-button flag of the window's surface (a spare one if the surface is already gone). */
static int *left_of(const Window *w)
{
    static int spare;
    int slot = gui_slot(handle_of(w));
    return slot >= 0 && slot < SL_CAP ? &SL[slot].left : &spare;
}

static void draw(Window *w, UiRect body)
{
    int h = handle_of(w), sw = 0, sh = 0;
    GfxColor back = ui_color(ui_rgb(24, 26, 34));
    if (!gui_size(h, &sw, &sh))
    {
        gfx_fill_rect(body.x, body.y, body.w, body.h, back); /* the program is gone, the window follows on the next pass */
        return;
    }
    /* a maximized window is bigger than the picture: the margin right and below it is plain */
    if (body.w > sw)
        gfx_fill_rect(body.x + sw, body.y, body.w - sw, body.h, back);
    if (body.h > sh)
        gfx_fill_rect(body.x, body.y + sh, sw < body.w ? sw : body.w, body.h - sh, back);

    /* only the rows and columns inside the clip rectangle (a damaged part of the window) are copied */
    GfxClip c = gfx_get_clip();
    int y0 = c.y0 - body.y, y1 = c.y1 - body.y, x0 = c.x0 - body.x, x1 = c.x1 - body.x;
    if (y0 < 0)
        y0 = 0;
    if (x0 < 0)
        x0 = 0;
    if (y1 > sh)
        y1 = sh;
    if (y1 > body.h)
        y1 = body.h;
    if (x1 > sw)
        x1 = sw;
    if (x1 > body.w)
        x1 = body.w;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1;)
        {
            int n = 0;
            const unsigned *run = gui_pixels(h, x, y, &n); /* the segment's pages are not next to each other: one run per page */
            if (!run || n <= 0)
                break;
            if (n > x1 - x)
                n = x1 - x;
            gfx_put_pixels(body.x + x, body.y + y, run, n);
            x += n;
        }
}

static void post(Window *w, GuiEvent *ev)
{
    gui_post(handle_of(w), ev);
}

static void mouse_event(Window *w, int type, int sx, int sy, int buttons)
{
    UiRect c = wm_content(w);
    GuiEvent ev = {type, sx - c.x, sy - c.y, buttons, 0, 0, 0, 0};
    post(w, &ev);
}

static void on_key(Window *w, const KeyEvent *k)
{
    GuiEvent ev = {GUI_EV_KEY, 0, 0, 0, k->key, k->ch, k->pressed, (int)k->mods};
    post(w, &ev);
}

static int on_press(Window *w, int x, int y)
{
    *left_of(w) = 1;
    mouse_event(w, GUI_EV_MOUSE_DOWN, x, y, 1);
    return 0;
}

static int on_drag(Window *w, int x, int y, unsigned buttons)
{
    int left = (buttons & 1) != 0;
    *left_of(w) = left;
    mouse_event(w, left ? GUI_EV_MOUSE_MOVE : GUI_EV_MOUSE_UP, x, y, left ? 1 : 0);
    return 0;
}

static int on_rpress(Window *w, int x, int y)
{
    mouse_event(w, GUI_EV_MOUSE_DOWN, x, y, 2);
    return 0;
}

static int on_hover(Window *w, int x, int y)
{
    if (x < 0) /* the pointer left the window */
        return 0;
    UiRect c = wm_content(w);
    if (x >= c.x && y >= c.y && x < c.x + c.w && y < c.y + c.h) /* (the title bar is not the program's) */
        mouse_event(w, GUI_EV_MOUSE_MOVE, x, y, *left_of(w));
    return 0;
}

static int on_wheel(Window *w, int steps)
{
    GuiEvent ev = {GUI_EV_WHEEL, steps, 0, 0, 0, 0, 0, 0};
    post(w, &ev);
    return 0;
}

/* The user wants the window closed: the program is asked first (it may have unsaved work). If it did not even read the previous
 * request it is stuck, and the window closes without it - the surface is destroyed and the program's next call says so. */
static int on_close_request(Window *w) { return gui_request_close(handle_of(w)); }

static void on_close(Window *w) { gui_destroy(handle_of(w)); }

static int on_tick(Window *w)
{
    int h = handle_of(w), changed = 0, x, y, sw, sh;
    if (gui_take_title_changed(h))
    {
        const char *t = gui_title(h);
        int n = 0;
        for (; t[n] && n < WM_TITLE_MAX - 1; n++)
            w->title[n] = t[n];
        w->title[n] = 0;
        changed = 1; /* the whole window is redrawn: the title bar shows it */
    }
    if (gui_take_dirty(h, &x, &y, &sw, &sh))
    {
        UiRect c = wm_content(w);
        if (x < c.w && y < c.h) /* (the window cannot be resized, but the clip below keeps a smaller one safe) */
        {
            if (x + sw > c.w)
                sw = c.w - x;
            if (y + sh > c.h)
                sh = c.h - y;
            wm_add_damage(ui_rect(c.x + x, c.y + y, sw, sh));
            if (!changed)
                changed = 2;
        }
    }
    return changed;
}

static int open_window(int slot, int h)
{
    int sw, sh;
    if (!gui_size(h, &sw, &sh))
        return 0;
    int tb = ui_theme()->titlebar_h;
    int ow = sw + 2, oh = sh + tb + 2; /* the content area is the window minus its frame (see wm_content) */
    int W = (int)gfx_width(), H = (int)gfx_height();
    int step = (OPENED++ % 5) * 28;
    int x = (W - ow) / 2 - 80 + step, y = (H - TASKBAR_HEIGHT - oh) / 2 - 40 + step;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    Window *win = wm_create(gui_title(h), x, y, ow, oh, draw, (void *)(unsigned long long)h);
    if (!win)
        return 0;
    wm_set_resizable(win, 0); /* the picture has a fixed size */
    win->on_key = on_key;
    win->on_press = on_press;
    win->on_drag = on_drag;
    win->on_rpress = on_rpress;
    win->on_hover = on_hover;
    win->on_wheel = on_wheel;
    win->on_tick = on_tick;
    win->on_close_request = on_close_request;
    win->on_close = on_close;
    win->icon = TASKBAR_ICON_LOGO;
    SL[slot].win = win;
    SL[slot].id = win->id;
    SL[slot].h = h;
    SL[slot].left = 0;
    return 1;
}

/* Keep one Slot per slot of the surface table (which grows). The new entries are empty. 0 = out of memory. */
static int follow_table(void)
{
    int n = gui_slot_count();
    if (n <= SL_CAP)
        return 1;
    Slot *t = krealloc(SL, (unsigned long long)n * sizeof(Slot));
    if (!t)
        return 0;
    for (int i = SL_CAP; i < n; i++)
    {
        t[i].win = 0;
        t[i].id = 0;
        t[i].h = -1;
        t[i].left = 0;
    }
    SL = t;
    SL_CAP = n;
    return 1;
}

int gui_app_poll(void)
{
    int changed = 0;
    if (!follow_table())
        return 0; /* (out of memory: try again on the next pass) */
    for (int slot = 0; slot < SL_CAP; slot++)
    {
        int h = gui_handle_at(slot);
        Slot *s = &SL[slot];
        if (s->win && (h != s->h || !wm_is_open(s->win) || s->win->id != s->id))
        {
            /* the surface is gone (or the user closed the window): close what is left of the pair */
            if (wm_is_open(s->win) && s->win->id == s->id)
                wm_close(s->win);
            s->win = 0;
            changed = 1;
        }
        if (h >= 0 && !s->win)
        {
            if (open_window(slot, h))
                changed = 1;
            else
                gui_destroy(h); /* no free window: the program is told by its next call */
        }
    }
    return changed;
}
