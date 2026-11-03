/* HarisOS volume popup (click on the tray speaker). */
#include "audio.h"
#include "appmenu.h"
#include "startmenu.h"
#include "taskbar.h"
#include "ui.h"
#include "volmenu.h"
#include "wm.h"
#include "desktop_internal.h"

#define MENU_W 276
#define MENU_H 68
#define MARGIN 8 /* gap to the taskbar and to the screen edge */
#define PAD 12
#define TILE 40
#define PCT_W 46
#define STEP 5

static int OPEN;
static int ANCHOR_X;
static int DRAG;       /* the slider knob is held */
static int MUTE_HOVER, MUTE_PRESS;
static int SHOWN = -1; /* the volume the panel was last drawn with */
static int SAVED = 50; /* the volume before the mute button was pressed */
static unsigned LAST_BUTTONS;

/* ---- geometry ---- */
static UiRect menu_rect(void)
{
    int W = (int)gfx_width(), H = (int)gfx_height();
    int x = ANCHOR_X - MENU_W / 2;
    if (x > W - MENU_W - MARGIN)
        x = W - MENU_W - MARGIN;
    if (x < MARGIN)
        x = MARGIN;
    int y = H - TASKBAR_HEIGHT - MARGIN - MENU_H;
    if (y < 4)
        y = 4;
    return ui_rect(x, y, MENU_W, MENU_H);
}

UiRect volmenu_bounds(void)
{
    UiRect m = menu_rect();
    return ui_rect(m.x - 36, m.y - 30, m.w + 72, m.h + 80);
}

static UiRect tile_rect(void)
{
    UiRect m = menu_rect();
    return ui_rect(m.x + PAD, m.y + (m.h - TILE) / 2, TILE, TILE);
}

static UiRect slider_rect(void)
{
    UiRect m = menu_rect(), t = tile_rect();
    int x = t.x + t.w + 14;
    return ui_rect(x, m.y + (m.h - 20) / 2, m.x + m.w - PAD - PCT_W - 8 - x, 20);
}

static void dmg_all(void) { wm_add_screen_damage(volmenu_bounds()); }
static void dmg_panel(void) { wm_add_screen_damage(menu_rect()); }

/* the percentage a click at x on the slider means: the knob center runs from r.x + r.h/2 to r.x + r.w - r.h/2 (see ui_slider) */
static int percent_at(int x)
{
    UiRect s = slider_rect();
    int span = s.w - s.h;
    int p = span > 0 ? ((x - s.x - s.h / 2) * 100 + span / 2) / span : 0;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

/* ---- the volume ---- */
static void set_volume(int percent)
{
    if (audio_set_volume(percent) >= 0)
        volmenu_tick();
}

static void toggle_mute(void)
{
    int v = audio_volume();
    if (v < 0)
        return;
    if (v > 0)
    {
        SAVED = v;
        set_volume(0);
    }
    else
        set_volume(SAVED > 0 ? SAVED : 50);
}

/* ---- open / close ---- */
int volmenu_is_open(void) { return OPEN; }

void volmenu_close(void)
{
    if (!OPEN)
        return;
    dmg_all();
    OPEN = DRAG = MUTE_HOVER = MUTE_PRESS = 0;
}

void volmenu_open(int anchor_x)
{
    if (audio_volume() < 0)
        return; /* no sound hardware: nothing to set */
    startmenu_close(); /* only one menu at a time */
    appmenu_close();
    if (OPEN)
        volmenu_close();
    OPEN = 1;
    ANCHOR_X = anchor_x;
    DRAG = MUTE_HOVER = MUTE_PRESS = 0;
    SHOWN = audio_volume();
    dmg_all();
}

void volmenu_tick(void)
{
    int v = audio_volume();
    if (OPEN && v != SHOWN)
    {
        SHOWN = v;
        dmg_panel();
    }
}

/* ---- input ---- */
int volmenu_pointer(int x, int y, unsigned buttons)
{
    unsigned now = buttons & 3u;
    unsigned pressed = now & ~LAST_BUTTONS;
    int left_release = (LAST_BUTTONS & 1u) && !(now & 1u);
    LAST_BUTTONS = now;
    if (!OPEN)
        return 0;

    int valid = x >= 0;
    int over = valid && ui_contains(menu_rect(), x, y);
    if (pressed && valid && !over) /* a click away from the panel closes it and still goes through */
    {
        volmenu_close();
        return 0;
    }

    int hover = over && ui_contains(tile_rect(), x, y);
    if (hover != MUTE_HOVER)
    {
        MUTE_HOVER = hover;
        dmg_panel();
    }
    if ((pressed & 1u) && over)
    {
        if (hover)
        {
            MUTE_PRESS = 1;
            dmg_panel();
        }
        else
        {
            UiRect s = slider_rect();
            if (ui_contains(ui_rect(s.x - 6, s.y - 8, s.w + 12, s.h + 16), x, y)) /* a little slack around the thin slider */
            {
                DRAG = 1;
                set_volume(percent_at(x));
            }
        }
    }
    else if (DRAG && (now & 1u) && valid)
        set_volume(percent_at(x)); /* keeps following the pointer outside the panel too */
    if (left_release)
    {
        if (MUTE_PRESS && hover)
            toggle_mute();
        if (MUTE_PRESS)
            dmg_panel();
        MUTE_PRESS = 0;
        DRAG = 0;
    }
    return over || DRAG;
}

int volmenu_key(const KeyEvent *k)
{
    if (!OPEN || !k->pressed)
        return OPEN;
    switch (k->key)
    {
    case KEY_ESC:
    case KEY_ENTER:
        volmenu_close();
        break;
    case KEY_LEFT:
    case KEY_DOWN:
        audio_volume_step(-STEP);
        break;
    case KEY_RIGHT:
    case KEY_UP:
        audio_volume_step(STEP);
        break;
    case 'm':
        toggle_mute();
        break;
    default:
        break;
    }
    volmenu_tick();
    return 1;
}

/* ---- drawing ---- */
void volmenu_draw(void)
{
    if (!OPEN || !gfx_ready())
        return;
    int v = audio_volume();
    if (v < 0)
        return; /* the sound hardware went away: the next input removes the panel */
    SHOWN = v;
    const UiTheme *prev = ui_theme();
    ui_set_theme(startmenu_theme()); /* light glass look, whatever the desktop theme is */
    const UiTheme *t = startmenu_theme();
    UiRect m = menu_rect();
    startmenu_glass(m, 16);

    /* the speaker tile: the mute button */
    UiRect tl = tile_rect();
    gfx_fill_round_rect_alpha(tl.x, tl.y, tl.w, tl.h, 12, gfx_rgb(255, 255, 255), MUTE_PRESS ? 255 : MUTE_HOVER ? 240 : 215);
    gfx_round_rect_alpha(tl.x, tl.y, tl.w, tl.h, 12, 1, ui_color(t->border), 150);
    ui_icon(UI_ICON_VOLUME, tl.x + tl.w / 2, tl.y + tl.h / 2, 24, v == 0 ? t->text_dim : t->text);
    if (v == 0) /* muted: a slash across the speaker */
    {
        int cx = tl.x + tl.w / 2, cy = tl.y + tl.h / 2;
        gfx_line_q((cx - 10) * 16, (cy - 10) * 16, (cx + 10) * 16, (cy + 10) * 16, 3 * 16, ui_color(t->danger), 255);
    }

    ui_slider(slider_rect(), v);

    char buf[8];
    int n = 0;
    if (v >= 100)
    {
        buf[n++] = '1';
        buf[n++] = '0';
        buf[n++] = '0';
    }
    else
    {
        if (v >= 10)
            buf[n++] = (char)('0' + v / 10);
        buf[n++] = (char)('0' + v % 10);
    }
    buf[n++] = '%';
    buf[n] = 0;
    ui_text_in_color(ui_rect(m.x + m.w - PAD - PCT_W, m.y, PCT_W, m.h), buf, UI_TEXT_LABEL, UI_ALIGN_RIGHT, t->text);
    ui_set_theme(prev);
}
