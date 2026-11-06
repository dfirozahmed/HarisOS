/* Display settings app: brightness slider (software dimming, see gfx_set_brightness) and the
 * display adapter / screen size. Left/Right (or Down/Up) change the brightness by 5, Home/End jump to the ends.
 * An app uses only public headers (ui.h, wm.h, desktop.h, display.h, gfx.h) - never desktop_internal.h. */
#include "app.h"
#include "desktop.h"
#include "display.h"
#include "gfx.h"
#include "look.h"
#include "pci.h"
#include "screensleep.h"
#include "taskbar.h"
#include "ui.h"
#include "wm.h"

#define MARGIN 24
#define KNOB_H 22
#define STEP 5

/* "Turn off the screen after": the choices in seconds (0 = never), the < and > buttons step through them */
static const int OFF_CHOICE[] = {0, 60, 120, 300, 600, 900, 1800};
#define N_OFF ((int)(sizeof OFF_CHOICE / sizeof OFF_CHOICE[0]))

static Window *DS_WIN;
static int DRAGGING; /* the left button went down on the slider and is still held */

/* appends decimal v to s at *n (s has room) */
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

/* The slider track area inside the window's content area. */
static UiRect slider_rect(UiRect body)
{
    return ui_rect(body.x + MARGIN, body.y + 78, body.w - 2 * MARGIN, KNOB_H);
}

/* Brightness from a pointer x on the slider (the knob center spans the track minus one knob width). */
static int percent_at(UiRect s, int x)
{
    int span = s.w - s.h;
    int p = GFX_BRIGHTNESS_MIN + (x - (s.x + s.h / 2)) * (GFX_BRIGHTNESS_MAX - GFX_BRIGHTNESS_MIN) / (span > 0 ? span : 1);
    if (p < GFX_BRIGHTNESS_MIN)
        p = GFX_BRIGHTNESS_MIN;
    if (p > GFX_BRIGHTNESS_MAX)
        p = GFX_BRIGHTNESS_MAX;
    return p;
}

/* Slider position 0..100 for the current brightness (the slider starts at the minimum brightness). */
static int slider_pos(void)
{
    return (gfx_brightness() - GFX_BRIGHTNESS_MIN) * 100 / (GFX_BRIGHTNESS_MAX - GFX_BRIGHTNESS_MIN);
}

static void ds_draw(Window *w, UiRect body)
{
    (void)w;
    const UiTheme *t = ui_theme();
    ui_text(body.x + MARGIN, body.y + 22, "Brightness", UI_TEXT_TITLE);

    char pct[8], line_off[24];
    int n = 0;
    put_dec(pct, &n, (unsigned)gfx_brightness());
    put_str(pct, &n, "%");
    ui_text_in(ui_rect(body.x + body.w - MARGIN - 80, body.y + 22, 80, 28), pct, UI_TEXT_LABEL, UI_ALIGN_RIGHT);

    ui_slider(slider_rect(body), slider_pos());
    if (!gfx_double_buffered())
        ui_text_in_color(ui_rect(body.x + MARGIN, body.y + 108, body.w - 2 * MARGIN, 22),
                         "Needs double buffering (not available now)", UI_TEXT_SMALL, UI_ALIGN_LEFT, t->warning);
    else
        ui_text_in_color(ui_rect(body.x + MARGIN, body.y + 108, body.w - 2 * MARGIN, 22),
                         "Ctrl+Alt+Up / Down works anywhere", UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);

    /* the screen goes off after a time without input (screensleep.h): the panel's backlight, so only where the driver has one */
    ui_text(body.x + MARGIN, body.y + 150, "Turn off the screen", UI_TEXT_TITLE);
    if (screensleep_available())
    {
        int sec = look_screenoff();
        n = 0;
        line_off[0] = 0;
        if (sec <= 0)
            put_str(line_off, &n, "Never");
        else
        {
            put_str(line_off, &n, "After ");
            put_dec(line_off, &n, (unsigned)(sec >= 60 ? sec / 60 : sec));
            put_str(line_off, &n, sec >= 60 ? (sec == 60 ? " minute" : " minutes") : " seconds");
        }
        ui_text_in(ui_rect(body.x + MARGIN, body.y + 182, body.w - 2 * MARGIN - 90, 28), line_off, UI_TEXT_BODY, UI_ALIGN_LEFT);
    }
    else
        ui_text_in_color(ui_rect(body.x + MARGIN, body.y + 182, body.w - 2 * MARGIN, 28),
                         "Not available here (needs the panel backlight driver)", UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);

    /* screen + adapter */
    int y = body.y + 220;
    ui_text(body.x + MARGIN, y, "Display", UI_TEXT_TITLE);
    char line[64];
    n = 0;
    put_str(line, &n, "Resolution: ");
    put_dec(line, &n, gfx_width());
    put_str(line, &n, " x ");
    put_dec(line, &n, gfx_height());
    ui_text(body.x + MARGIN, y + 34, line, UI_TEXT_BODY);

    const DisplayAdapter *a = 0;
    for (int i = 0; i < display_count(); i++)
        if (display_get(i)->active || !a)
            a = display_get(i);
    if (a)
    {
        n = 0;
        put_str(line, &n, "Adapter: ");
        put_str(line, &n, pci_vendor_name(a->vendor_id));
        ui_text(body.x + MARGIN, y + 60, line, UI_TEXT_BODY);
        ui_text_color(body.x + MARGIN, y + 86, display_driver_name(a->kind), UI_TEXT_SMALL, t->text_dim);
    }
    else
        ui_text_color(body.x + MARGIN, y + 60, "No display adapter found on PCI", UI_TEXT_BODY, t->text_dim);
}

static int UNSAVED; /* the brightness changed since THEME.CFG was last written */

static void flush_brightness(void)
{
    if (UNSAVED)
    {
        UNSAVED = 0;
        look_save_brightness();
    }
}

static int set_percent(int p)
{
    if (p == gfx_brightness())
        return 0;
    gfx_set_brightness(p);
    UNSAVED = 1;
    return 1;
}

static int ds_press(Window *w, int x, int y)
{
    UiRect s = slider_rect(wm_content(w));
    if (x < s.x || x >= s.x + s.w || y < s.y - 8 || y >= s.y + s.h + 8)
        return 0;
    DRAGGING = 1;
    return set_percent(percent_at(s, x));
}

static int ds_drag(Window *w, int x, int y, unsigned buttons)
{
    (void)y;
    if (!(buttons & 1))
    {
        DRAGGING = 0;
        flush_brightness();
        return 0;
    }
    return DRAGGING ? set_percent(percent_at(slider_rect(wm_content(w)), x)) : 0;
}

static void ds_key(Window *w, const KeyEvent *k)
{
    if (!k->pressed)
        return;
    switch (k->key)
    {
    case KEY_LEFT:
    case KEY_DOWN:
        set_percent(gfx_brightness() - STEP);
        break;
    case KEY_RIGHT:
    case KEY_UP:
        set_percent(gfx_brightness() + STEP);
        break;
    case KEY_HOME:
        set_percent(GFX_BRIGHTNESS_MIN);
        break;
    case KEY_END:
        set_percent(GFX_BRIGHTNESS_MAX);
        break;
    case KEY_ESC:
        wm_close(w);
        break;
    default:
        break;
    }
    flush_brightness(); /* a key press is a finished change (no-op after ESC closed the window: ds_close flushed) */
}

static void on_reset(UiButton *b, void *user)
{
    (void)b;
    (void)user;
    set_percent(GFX_BRIGHTNESS_MAX);
    flush_brightness();
}

/* the next / previous choice for the time before the screen goes off */
static void step_off(int dir)
{
    int cur = look_screenoff(), i = 0;
    if (!screensleep_available())
        return;
    for (int k = 0; k < N_OFF; k++)
        if (OFF_CHOICE[k] <= cur)
            i = k; /* the nearest choice at or below the stored time */
    i += dir;
    if (i < 0)
        i = 0;
    if (i >= N_OFF)
        i = N_OFF - 1;
    look_set_screenoff(OFF_CHOICE[i]);
}
static void on_off_prev(UiButton *b, void *user) { (void)b; (void)user; step_off(-1); }
static void on_off_next(UiButton *b, void *user) { (void)b; (void)user; step_off(1); }

static void ds_close(Window *w)
{
    (void)w;
    DS_WIN = 0;
    DRAGGING = 0;
    flush_brightness(); /* the button may have been released outside the window, without a final on_drag */
}

static void display_app_open(void)
{
    if (DS_WIN)
    {
        wm_activate(DS_WIN);
        return;
    }
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = 460, h = 420;
    if (w > W - 20)
        w = W - 20;
    if (h > H - TASKBAR_HEIGHT - 8)
        h = H - TASKBAR_HEIGHT - 8;
    int x = (W - w) / 2, y = (H - TASKBAR_HEIGHT - h) / 2;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;

    Window *win = wm_create("Display Settings", x, y, w, h, ds_draw, 0);
    if (!win)
    {
        desktop_set_status("Window limit reached - close one first.");
        wm_invalidate();
        return;
    }
    win->on_key = ds_key;
    win->on_press = ds_press;
    win->on_drag = ds_drag;
    win->on_close = ds_close;
    win->icon = TASKBAR_ICON_DISPLAY;
    wm_set_resizable(win, 0);
    DS_WIN = win;
    desktop_set_status(0);

    int cw = w - 2, ch = h - ui_theme()->titlebar_h - 2;
    wm_add_button_ex(win, "<", UI_BTN_SECONDARY, ui_rect(cw - MARGIN - 80, 176, 36, 34), WM_ANCHOR_RIGHT, on_off_prev, 0);
    wm_add_button_ex(win, ">", UI_BTN_SECONDARY, ui_rect(cw - MARGIN - 36, 176, 36, 34), WM_ANCHOR_RIGHT, on_off_next, 0);
    wm_add_button_ex(win, "Reset", UI_BTN_SECONDARY, ui_rect(cw - MARGIN - 96, ch - 16 - 34, 96, 34),
                     WM_ANCHOR_RIGHT | WM_ANCHOR_BOTTOM, on_reset, 0);
}

const AppInfo APP_DISPLAY = {
    .name = "Display Settings",
    .desc = "Brightness and adapter",
    .icon = UI_ICON_DISPLAY,
    .open = display_app_open,
};
