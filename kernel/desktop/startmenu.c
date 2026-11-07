/* HarisOS start menu: light translucent glass panel with search, app list and footer. */
#include "startmenu.h"
#include "app.h"
#include "appmenu.h"
#include "deskmenu.h"
#include "deskicons.h"
#include "volmenu.h"
#include "desktop.h"
#include "taskbar.h"
#include "ui.h"
#include "wm.h"
#include "desktop_internal.h"
#include "heap.h"

/* ---- look: always a light glass theme, whatever the desktop theme is; only the accent follows the desktop's ---- */
#define RGB(r, g, b) {r, g, b}
static UiTheme MENU_THEME = {
    .bg_top = RGB(150, 190, 246),
    .bg_bottom = RGB(226, 238, 252),
    .glow1 = RGB(255, 255, 255),
    .glow2 = RGB(190, 160, 255),
    .surface = RGB(250, 252, 255),
    .surface_alt = RGB(232, 237, 246),
    .titlebar = RGB(238, 242, 249),
    .border = RGB(205, 214, 230),
    .text = RGB(26, 36, 56),
    .text_dim = RGB(100, 112, 136),
    .text_on_accent = RGB(255, 255, 255),
    .accent = RGB(36, 108, 236),
    .accent_hi = RGB(76, 144, 250),
    .success = RGB(28, 158, 98),
    .warning = RGB(214, 140, 20),
    .danger = RGB(220, 62, 70),
    .radius_s = 6,
    .radius_m = 10,
    .radius_l = 14,
    .pad = 20,
    .gap = 10,
    .titlebar_h = 40,
    .shadow = 90,
    .font_small = &font_ui_12,
    .font_body = &font_ui_14,
    .font_label = &font_med_14,
    .font_title = &font_med_16,
    .font_heading = &font_bold_20,
};

#define PANEL_ALPHA 196 /* 255 = solid; lower = more see-through (the app menu's panel) */
#define PANEL_ALPHA_FROST 172 /* the start menu's panel over its blurred backdrop: the blur hides what is behind, so it can be glassier */
#define PANEL_ALPHA_PLAIN 232 /* ... when the backdrop cannot be blurred (a compositor draws the screen): nearly solid, so what is behind does not cloud the text */
#define FROST_RADIUS 8        /* the blur: three box blurs of 17 pixels, about 8 pixels of Gaussian */
#define MENU_W 340
#define MARGIN 10 /* gap to the screen edge and to the taskbar */
#define PAD 14
#define SEARCH_H 38
#define LABEL_GAP 12
#define LABEL_H 22
#define ROW_H 54
#define FOOT_H 58
#define BUILTIN_ITEMS 3
#define MAX_ITEMS (BUILTIN_ITEMS + APP_MAX)

typedef struct
{
    const char *name, *desc;
    UiIcon icon;
} MenuItem;

/* built-in desktop actions; installed apps (app.h) follow them in the list */
static const MenuItem BUILTIN[BUILTIN_ITEMS] = {
    {"Welcome", "The HarisOS start page", UI_ICON_LOGO},
    {"Hello window", "Text boxes and dialogs", UI_ICON_TERMINAL},
    {"Settings", "Theme, wallpaper and colours", UI_ICON_SETTINGS},
};

static int item_count(void) { return BUILTIN_ITEMS + app_count(); }

static MenuItem item_get(int i)
{
    if (i < BUILTIN_ITEMS)
        return BUILTIN[i];
    const AppInfo *a = app_get(i - BUILTIN_ITEMS);
    MenuItem m = {a->name, a->desc, a->icon};
    return m;
}

static void follow_accent(void)
{
    const UiTheme *t = ui_theme();
    if (t == &MENU_THEME)
        return;
    MENU_THEME.accent = t->accent;
    MENU_THEME.accent_hi = t->accent_hi;
    MENU_THEME.text_on_accent = t->text_on_accent;
}

const UiTheme *startmenu_theme(void)
{
    follow_accent();
    return &MENU_THEME;
}

/* Does the drawing area (the clip) reach r? A redraw of one row is clipped to that row: the parts far from it need not be worked out at all. */
static int clip_hits(UiRect r)
{
    GfxClip c = gfx_get_clip();
    return !(c.x0 >= r.x + r.w || c.x1 <= r.x || c.y0 >= r.y + r.h || c.y1 <= r.y);
}

/* is the whole clip inside r? */
static int clip_inside(UiRect r)
{
    GfxClip c = gfx_get_clip();
    return c.x0 >= r.x && c.y0 >= r.y && c.x1 <= r.x + r.w && c.y1 <= r.y + r.h;
}

/* the panel's glass with the fill at `alpha` */
static void glass(UiRect r, int radius, int alpha)
{
    /* The shadow is a set of faint rings OUTSIDE the panel, so nothing dark shows through the
     * translucent fill (gfx_shadow() would darken the panel's own edges). */
    enum
    {
        RINGS = 14
    };
    /* a redraw inside the panel (a row, the search box) reaches neither the rings (outside the rounded corners) nor the edge line */
    int inner = clip_inside(ui_inset(r, radius + 1));
    for (int k = inner ? 0 : RINGS; k >= 1; k--)
    {
        int left = RINGS - k + 1; /* 14 near the panel ... 1 at the far edge */
        int a = 44 * left * left / (RINGS * RINGS);
        if (a < 1)
            a = 1;
        gfx_round_rect_alpha(r.x - k, r.y - k, r.w + 2 * k, r.h + 2 * k, radius + k, 1, gfx_rgb(10, 20, 50),
                             (unsigned char)a);
    }
    gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, radius, ui_color(MENU_THEME.surface), (unsigned char)alpha);
    if (!inner)
        gfx_round_rect_alpha(r.x, r.y, r.w, r.h, radius, 1, gfx_rgb(255, 255, 255), 235);
}

void startmenu_glass(UiRect r, int radius)
{
    glass(r, radius, PANEL_ALPHA);
}

/* ---- the blurred backdrop of the start menu's panel ----
 * What is behind the panel (wallpaper, desktop icons, windows) is blurred and kept, and the panel's translucent fill is drawn over it: frosted
 * glass. It needs the picture as the back buffer holds it, so it is captured when the panel is drawn on a picture that has no panel in it: the
 * first time after it opens, and whenever the draw covers the whole panel (then the desktop has just been drawn again under it). A partial redraw
 * (a row under the pointer) draws the kept blur again instead of capturing, because there the panel is already in the back buffer. */
static unsigned int *FROST_PX; /* the blurred copy: FROST_W x FROST_H pixels */
static unsigned long long FROST_N; /* pixels FROST_PX has room for */
static int FROST_OK;           /* FROST_PX holds the backdrop of the panel at FROST_X, FROST_Y */
static int FROST_X, FROST_Y, FROST_W, FROST_H;

static void frost_free(void)
{
    if (FROST_PX)
        kfree(FROST_PX);
    FROST_PX = 0;
    FROST_N = 0;
    FROST_OK = 0;
}

/* Draws the blurred backdrop under the panel m. 1 = it did, 0 = it cannot (the panel then gets the plain, nearly solid fill). */
static int frost(UiRect m)
{
    if (!gfx_backdrop_possible())
    {
        /* a compositor draws the screen: if its driver blurs the backdrop itself (desktop_comp sends it the panel), the panel is glassy */
        const GfxCompOps *co = gfx_comp();
        return co && co->backdrop;
    }
    GfxClip cl = gfx_get_clip();
    if (cl.x0 >= m.x + m.w || cl.x1 <= m.x || cl.y0 >= m.y + m.h || cl.y1 <= m.y)
        return FROST_OK; /* this draw does not reach the panel: nothing to do (and nothing is drawn of it either) */
    unsigned long long need = (unsigned long long)m.w * (unsigned long long)m.h;
    if (need > FROST_N)
    {
        frost_free();
        FROST_PX = (unsigned int *)kmalloc(need * sizeof(unsigned int));
        if (!FROST_PX)
            return 0;
        FROST_N = need;
    }
    int whole = cl.x0 <= m.x && cl.y0 <= m.y && cl.x1 >= m.x + m.w && cl.y1 >= m.y + m.h;
    if (!FROST_OK || whole || m.x != FROST_X || m.y != FROST_Y || m.w != FROST_W || m.h != FROST_H)
    {
        if (gfx_backdrop_capture(m.x, m.y, m.w, m.h, FROST_RADIUS, FROST_PX) != 0)
        {
            FROST_OK = 0;
            return 0;
        }
        FROST_OK = 1;
        FROST_X = m.x;
        FROST_Y = m.y;
        FROST_W = m.w;
        FROST_H = m.h;
    }
    gfx_backdrop_draw(FROST_PX, m.x, m.y, m.w, m.h, 18);
    return 1;
}

/* ---- state ---- */
static int OPEN;
static UiEdit SEARCH;
static int VIS[MAX_ITEMS]; /* indexes into ITEMS that match the search */
static int VN;
static int SCROLL;                 /* the first row of VIS that is shown: on a short screen not all rows fit, the wheel and the arrows scroll */
static int SEL;                    /* highlighted row (index into VIS), -1 = none */
static int PRESS = -1;             /* row held down with the mouse */
static int FOOT_HOVER, FOOT_PRESS; /* footer power buttons: 0 = none, 1 = Restart, 2 = Shut down */
static int FOOT_KEY;               /* footer power button with keyboard focus: 0 = none, 1 = Restart, 2 = Shut down */
static UiMenu CTX;                 /* the right-click menu of an app row: Open, Send to desktop */
static int CTX_APP = -1;           /* ... for this app (an index of the registry) */
static unsigned LAST_RDOWN;        /* the right button at the last event */
enum { SM_OPEN, SM_SEND };
static unsigned LAST_DOWN;
static unsigned FILTER_SIG;
/* hover highlights that fade (startmenu_tick): the row under SEL, and the two footer buttons. *_SHOWN is the level they were last drawn with. */
static UiFade ROW_F[MAX_ITEMS];
static int ROW_SHOWN[MAX_ITEMS];
static UiFade FOOT_F[2];
static int FOOT_SHOWN[2];
#define FADE_FRAME_MS 33 /* the fades are drawn at most this often (startmenu_tick): about 30 frames a second */
static unsigned long long FADE_LAST_MS;

/* ---- geometry (small functions: no big structs are copied) ---- */
/* How many app rows fit above the taskbar: on a short screen the menu would otherwise run under the taskbar and
 * hide the footer (Restart / Shut down). The rest are reached by scrolling (wheel, arrow keys) or the search box. */
static int rows_shown(void)
{
    int fixed = PAD + SEARCH_H + LABEL_GAP + LABEL_H + 8 + 1 + FOOT_H;
    int fit = ((int)gfx_height() - TASKBAR_HEIGHT - MARGIN - 4 - fixed) / ROW_H;
    int n = item_count();
    if (fit < 1)
        fit = 1;
    return n < fit ? n : fit;
}

static int menu_h(void)
{
    return PAD + SEARCH_H + LABEL_GAP + LABEL_H + rows_shown() * ROW_H + 8 + 1 + FOOT_H;
}

static UiRect menu_rect(void)
{
    int W = (int)gfx_width(), H = (int)gfx_height();
    int w = MENU_W < W - 2 * MARGIN ? MENU_W : W - 2 * MARGIN;
    int h = menu_h();
    int y = H - TASKBAR_HEIGHT - MARGIN - h;
    if (y < 4)
        y = 4;
    return ui_rect(MARGIN, y, w, h);
}

/* Everything the open menu draws, its shadow included (the same area its redraws mark). */
UiRect startmenu_bounds(void)
{
    UiRect m = menu_rect();
    return ui_rect(m.x - 36, m.y - 30, m.w + 72, m.h + 80);
}

int startmenu_panel(UiRect *panel, int *radius)
{
    if (!OPEN)
        return 0;
    *panel = menu_rect();
    *radius = 18; /* glass() */
    return 1;
}

static UiRect search_rect(void)
{
    UiRect m = menu_rect();
    return ui_rect(m.x + PAD + 26, m.y + PAD, m.w - 2 * PAD - 26, SEARCH_H);
}

static int rows_y(void)
{
    UiRect m = menu_rect();
    return m.y + PAD + SEARCH_H + LABEL_GAP + LABEL_H;
}

static UiRect row_rect(int i)
{
    UiRect m = menu_rect();
    return ui_rect(m.x + 8, rows_y() + (i - SCROLL) * ROW_H, m.w - 16, ROW_H - 4);
}

/* is row i of VIS in the part that is shown? */
static int row_on(int i)
{
    return i >= SCROLL && i < VN && i < SCROLL + rows_shown();
}

static int foot_y(void) { return rows_y() + rows_shown() * ROW_H + 8; }

/* the two power buttons in the footer: i = 0 Restart, i = 1 Shut down (the right-most) */
#define PBTN_W 40
#define PBTN_H 36
static UiRect power_btn_rect(int i)
{
    UiRect m = menu_rect();
    int x = m.x + m.w - PAD - PBTN_W - (1 - i) * (PBTN_W + 6);
    return ui_rect(x, foot_y() + 1 + (FOOT_H - PBTN_H) / 2, PBTN_W, PBTN_H);
}

static const char *power_label(int hover) { return hover == 1 ? "Restart" : "Shut down"; }

/* the little label above a hovered power button */
static UiRect power_tip_rect(int hover)
{
    UiRect m = menu_rect(), b = power_btn_rect(hover - 1);
    int w = gfx_text_w(MENU_THEME.font_small, power_label(hover)) + 22;
    int x = b.x + b.w / 2 - w / 2;
    if (x > m.x + m.w - 8 - w)
        x = m.x + m.w - 8 - w;
    return ui_rect(x, b.y - 34, w, 26);
}

/* ---- damage ---- */
static void dmg_all(void)
{
    UiRect m = menu_rect();
    wm_add_screen_damage(ui_rect(m.x - 36, m.y - 30, m.w + 72, m.h + 80)); /* the shadow reaches past the panel */
}

static void dmg_row(int i)
{
    if (row_on(i))
        wm_add_screen_damage(row_rect(i));
}

static void dmg_power(int hover)
{
    if (hover <= 0)
        return;
    wm_add_screen_damage(power_btn_rect(hover - 1));
    UiRect t = power_tip_rect(hover);
    wm_add_screen_damage(ui_rect(t.x - 2, t.y - 2, t.w + 4, t.h + 4));
}

static void dmg_search(void)
{
    UiRect s = search_rect();
    wm_add_screen_damage(ui_inset(s, -3));
}

/* ---- search filter ---- */
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int has_text(const char *hay, const char *needle)
{
    if (!*needle)
        return 1;
    for (; *hay; hay++)
    {
        int i = 0;
        while (needle[i] && hay[i] && lower(hay[i]) == lower(needle[i]))
            i++;
        if (!needle[i])
            return 1;
    }
    return 0;
}

static unsigned text_sig(const char *s)
{
    unsigned h = 5381;
    for (; *s; s++)
        h = h * 33u + (unsigned char)lower(*s);
    return h;
}

static void refilter(int reset_selection)
{
    VN = 0;
    for (int i = 0; i < item_count(); i++)
    {
        MenuItem it = item_get(i);
        if (has_text(it.name, SEARCH.text) || has_text(it.desc, SEARCH.text))
            VIS[VN++] = i;
    }
    if (VN == 0)
        SEL = -1;
    else if (reset_selection || SEL < 0 || SEL >= VN)
        SEL = 0;
    int most = VN - rows_shown(); /* the last position that still fills the rows */
    SCROLL = reset_selection || most < 0 ? 0 : (SCROLL > most ? most : SCROLL);
}

/* scroll so that row i is shown; 1 = the list moved */
static int scroll_to(int i)
{
    int old = SCROLL, rows = rows_shown();
    if (i < 0 || i >= VN)
        return 0;
    if (i < SCROLL)
        SCROLL = i;
    else if (i >= SCROLL + rows)
        SCROLL = i - rows + 1;
    return SCROLL != old;
}

void startmenu_wheel(int steps)
{
    if (!OPEN || CTX.open)
        return;
    int most = VN - rows_shown(), old = SCROLL; /* (positive steps = down) */
    SCROLL += steps;
    if (SCROLL > most)
        SCROLL = most;
    if (SCROLL < 0)
        SCROLL = 0;
    if (SCROLL != old)
        dmg_all();
}

/* ---- open / close / actions ---- */
int startmenu_is_open(void) { return OPEN; }

void startmenu_close(void)
{
    if (!OPEN)
        return;
    OPEN = 0;
    frost_free(); /* the blurred backdrop is made again at the next opening */
    ui_menu_close(&CTX);
    SEARCH.dragging = 0;
    PRESS = -1;
    FOOT_HOVER = FOOT_PRESS = FOOT_KEY = 0;
    dmg_all();
}

static void startmenu_open(void)
{
    if (OPEN)
        return;
    appmenu_close(); /* only one menu at a time */
    volmenu_close();
    deskmenu_close();
    OPEN = 1;
    FROST_OK = 0;
    for (int i = 0; i < MAX_ITEMS; i++)
    {
        ui_fade_reset(&ROW_F[i]);
        ROW_SHOWN[i] = 0;
    }
    for (int i = 0; i < 2; i++)
    {
        ui_fade_reset(&FOOT_F[i]);
        FOOT_SHOWN[i] = 0;
    }
    ui_menu_close(&CTX);
    ui_edit_set_text(&SEARCH, "");
    FILTER_SIG = text_sig("");
    refilter(1);
    PRESS = -1;
    FOOT_HOVER = FOOT_PRESS = FOOT_KEY = 0;
    ui_caret_restart();
    dmg_all();
}

void startmenu_toggle(void)
{
    if (OPEN)
        startmenu_close();
    else
        startmenu_open();
}

static void activate(int item)
{
    startmenu_close();
    switch (item)
    {
    case 0:
        if (!WELCOME)
            welcome_create();
        else
            wm_activate(WELCOME);
        break;
    case 1:
        open_hello_window();
        break;
    case 2:
        desktop_open_settings();
        break;
    default:
        if (item >= BUILTIN_ITEMS)
        {
            const AppInfo *a = app_get(item - BUILTIN_ITEMS);
            if (a && a->open)
                a->open();
        }
        break;
    }
}

/* the right-click menu's commands */
static void ctx_run(int id)
{
    int app = CTX_APP;
    startmenu_close();
    if (id == SM_SEND)
        deskicons_send_app(app);
    else if (id == SM_OPEN)
    {
        const AppInfo *a = app_get(app);
        if (a && a->open)
            a->open();
    }
}

/* ---- keyboard ---- */
int startmenu_key(const KeyEvent *k)
{
    if (!OPEN || !k->pressed)
        return OPEN;
    if (CTX.open) /* the menu has the keyboard: Up / Down / Enter / Esc */
    {
        int id = -1;
        int r = ui_menu_key(&CTX, k, &id);
        if (r)
            dmg_all();
        if (r == 2)
            ctx_run(id);
        return 1;
    }
    switch (k->key)
    {
    case KEY_ESC:
        startmenu_close();
        return 1;
    case KEY_ENTER:
        if (FOOT_KEY)
        {
            int power = FOOT_KEY;
            startmenu_close();
            desktop_power_action(power == 1); /* does not come back when it works */
        }
        else if (SEL >= 0 && SEL < VN)
            activate(VIS[SEL]);
        return 1;
    case KEY_LEFT:
    case KEY_RIGHT:
        if (FOOT_KEY) /* between Restart and Shut down; in the search box the arrows move the caret */
        {
            int old = FOOT_KEY;
            FOOT_KEY = k->key == KEY_LEFT ? 1 : 2;
            if (old != FOOT_KEY)
            {
                dmg_power(old);
                dmg_power(FOOT_KEY);
            }
            return 1;
        }
        break;
    case KEY_UP:
    case KEY_DOWN:
    case KEY_TAB:
    {
        /* one focus ring: the result rows, then Restart, then Shut down */
        int total = VN + 2;
        int back = k->key == KEY_UP || (k->key == KEY_TAB && (k->mods & KMOD_SHIFT));
        int pos = FOOT_KEY ? VN + FOOT_KEY - 1 : (SEL >= 0 ? SEL : (back ? 0 : -1));
        pos = (pos + (back ? total - 1 : 1)) % total;
        int oldsel = SEL, oldfoot = FOOT_KEY;
        FOOT_KEY = pos >= VN ? pos - VN + 1 : 0;
        if (!FOOT_KEY)
            SEL = pos;
        if (!FOOT_KEY && scroll_to(SEL))
            dmg_all();
        dmg_row(oldsel);
        dmg_row(SEL);
        dmg_power(oldfoot);
        dmg_power(FOOT_KEY);
        return 1;
    }
    default:
        break;
    }
    if (FOOT_KEY) /* typing goes back to the search box */
    {
        int old = FOOT_KEY;
        FOOT_KEY = 0;
        dmg_power(old);
        dmg_row(SEL);
    }
    if (ui_edit_key(&SEARCH, k))
    {
        unsigned sig = text_sig(SEARCH.text);
        refilter(sig != FILTER_SIG);
        FILTER_SIG = sig;
        dmg_all(); /* the list may have changed; the search box shows the caret / selection */
    }
    return 1;
}

/* ---- mouse ---- */
int startmenu_pointer(int x, int y, unsigned buttons)
{
    unsigned down = buttons & 1u;
    int edge = down && !LAST_DOWN, release = !down && LAST_DOWN;
    LAST_DOWN = down;
    int redge = (buttons & 2u) && !LAST_RDOWN; /* the right button went down just now */
    LAST_RDOWN = (buttons & 2u) != 0;
    if (!OPEN)
        return 0;

    if (CTX.open) /* the right-click menu of an app: it takes the mouse while it is open */
    {
        int vx = x >= 0;
        int over_ctx = vx && ui_contains(ui_menu_rect(&CTX), x, y);
        if (redge) /* a right click closes it, and may open another one below */
        {
            ui_menu_close(&CTX);
            dmg_all();
        }
        else if (edge && !over_ctx) /* a click away only closes it */
        {
            ui_menu_close(&CTX);
            dmg_all();
            return 1;
        }
        else
        {
            if (ui_menu_hover(&CTX, vx ? x : -1, y))
                dmg_all();
            if (edge && over_ctx)
            {
                ui_menu_press(&CTX, x, y);
                dmg_all();
            }
            if (CTX.press >= 0) /* held on an item: it runs on release */
            {
                int id = -1;
                int r = ui_menu_release(&CTX, x, y, buttons, &id);
                if (r)
                    dmg_all();
                if (r == 2)
                    ctx_run(id);
            }
            return 1;
        }
    }

    int H = (int)gfx_height();
    int valid = x >= 0;
    int over = valid && ui_contains(menu_rect(), x, y);

    /* a click away from the menu closes it (a click on the taskbar is left to the taskbar) */
    if (edge && valid && !over && y < H - TASKBAR_HEIGHT)
    {
        startmenu_close();
        return 0;
    }

    /* search box: click puts the caret, dragging selects, double-click selects a word */
    if (edge && over && ui_contains(search_rect(), x, y))
    {
        ui_edit_mouse(&SEARCH, x, y, buttons);
        ui_caret_restart();
        dmg_search();
        return 1;
    }
    if (SEARCH.dragging)
    {
        if (ui_edit_mouse(&SEARCH, x, y, buttons))
        {
            ui_caret_restart();
            dmg_search();
        }
        return over;
    }

    int hit = -1;
    if (over)
        for (int i = SCROLL; i < VN && i < SCROLL + rows_shown(); i++)
            if (ui_contains(row_rect(i), x, y))
                hit = i;
    int foot = 0; /* which power button the pointer is on */
    if (over)
        for (int i = 0; i < 2; i++)
            if (ui_contains(power_btn_rect(i), x, y))
                foot = i + 1;

    /* right click on an app row: Open, Send to desktop (the built-in rows - Welcome, Hello window, Settings - are not apps) */
    if (redge && hit >= 0 && VIS[hit] >= BUILTIN_ITEMS)
    {
        CTX_APP = VIS[hit] - BUILTIN_ITEMS;
        ui_menu_clear(&CTX);
        ui_menu_add(&CTX, SM_OPEN, "Open", 0, 0);
        ui_menu_add(&CTX, SM_SEND, "Send to desktop", 0, 0);
        ui_menu_open(&CTX, x, y, menu_rect());
        dmg_row(SEL);
        SEL = hit;
        dmg_row(SEL);
        dmg_all();
        return 1;
    }

    /* hover: the row under the pointer becomes the highlighted one */
    if (hit >= 0 && (hit != SEL || FOOT_KEY) && PRESS < 0)
    {
        dmg_power(FOOT_KEY);
        FOOT_KEY = 0;
        dmg_row(SEL);
        SEL = hit;
        dmg_row(SEL);
    }
    if (foot != FOOT_HOVER)
    {
        dmg_power(FOOT_HOVER);
        FOOT_HOVER = foot;
        dmg_power(foot);
    }

    if (edge)
    {
        PRESS = hit;
        FOOT_PRESS = foot;
        dmg_row(hit);
        dmg_power(foot);
    }
    if (release)
    {
        int act = (PRESS >= 0 && hit == PRESS) ? VIS[PRESS] : -1;
        int power = (FOOT_PRESS && foot == FOOT_PRESS) ? FOOT_PRESS : 0; /* released on the same button */
        int old = PRESS;
        PRESS = -1;
        FOOT_PRESS = 0;
        dmg_row(old);
        dmg_power(foot);
        if (power)
        {
            startmenu_close();
            desktop_power_action(power == 1); /* does not come back when it works */
        }
        else if (act >= 0)
            activate(act);
    }
    return over;
}

int startmenu_tick(int caret_flipped, unsigned long long now_ms)
{
    if (!OPEN)
        return 0;
    int redraw = 0;
    if (caret_flipped)
    {
        dmg_search();
        redraw = 1;
    }
    UiRect cd;
    if (ui_menu_tick(&CTX, now_ms, &cd)) /* the right-click menu of a row */
    {
        wm_add_screen_damage(cd);
        redraw = 1;
    }
    /* A fade changes its level almost every millisecond (255 steps in 130 ms), and each change is a frame: with the GPU compositor every frame
     * blurs the panel's whole backdrop again (about 250 frames a second while the pointer moves over the rows). The levels are shown at most
     * every FADE_FRAME_MS (about 30 frames a second); a fade that has arrived is shown at once, so it never stops short of its end. */
    int frame_due = now_ms - FADE_LAST_MS >= FADE_FRAME_MS;
    int shown_any = 0;
    for (int i = 0; i < MAX_ITEMS; i++) /* rows past the end of the list fade out at once, so they start from nothing when they come back */
    {
        int on = i < VN && i == SEL && !FOOT_KEY;
        if (i >= VN && ROW_SHOWN[i] == 0 && ROW_F[i].target == 0)
            continue;
        if (i >= VN || !on) /* a row the pointer left goes out at once: only the row it entered fades, so a quick sweep over many rows is not many fades at a time */
            ui_fade_reset(&ROW_F[i]);
        else
            ui_fade_to(&ROW_F[i], 255, UI_FADE_MS, now_ms);
        int lv = ui_fade_level(&ROW_F[i], now_ms);
        if (lv != ROW_SHOWN[i] && (frame_due || lv == ROW_F[i].target))
        {
            ROW_SHOWN[i] = lv;
            dmg_row(i);
            redraw = shown_any = 1;
        }
    }
    for (int i = 0; i < 2; i++)
    {
        ui_fade_to(&FOOT_F[i], FOOT_HOVER == i + 1 || FOOT_KEY == i + 1 ? 255 : 0, UI_FADE_MS, now_ms);
        int lv = ui_fade_level(&FOOT_F[i], now_ms);
        if (lv != FOOT_SHOWN[i] && (frame_due || lv == FOOT_F[i].target))
        {
            FOOT_SHOWN[i] = lv;
            wm_add_screen_damage(power_btn_rect(i));
            redraw = shown_any = 1;
        }
    }
    if (shown_any)
        FADE_LAST_MS = now_ms;
    return redraw;
}

/* ---- drawing ---- */
/* sin / cos of 15 degree steps (x 1024). A step k points k * 15 degrees clockwise from the top. */
static const short SIN15[24] = {0, 265, 512, 724, 887, 989, 1024, 989, 887, 724, 512, 265,
                                0, -265, -512, -724, -887, -989, -1024, -989, -887, -724, -512, -265};
static const short COS15[24] = {1024, 989, 887, 724, 512, 265, 0, -265, -512, -724, -887, -989,
                                -1024, -989, -887, -724, -512, -265, 0, 265, 512, 724, 887, 989};

/* point k (0..23) on a circle of radius r pixels around (cx, cy), in Q4 */
static void circle_pt(int cx, int cy, int r, int k, int *xq, int *yq)
{
    *xq = cx * 16 + r * 16 * SIN15[k % 24] / 1024;
    *yq = cy * 16 - r * 16 * COS15[k % 24] / 1024;
}

static void arc(int cx, int cy, int r, int k0, int k1, GfxColor c)
{
    for (int k = k0; k < k1; k++)
    {
        int x0, y0, x1, y1;
        circle_pt(cx, cy, r, k, &x0, &y0);
        circle_pt(cx, cy, r, k + 1, &x1, &y1);
        gfx_line_q(x0, y0, x1, y1, 34, c, 255);
    }
}

/* the power symbol: a ring open at the top with a short bar */
static void icon_power(int cx, int cy, GfxColor c)
{
    arc(cx, cy, 7, 3, 21, c);
    gfx_line_q(cx * 16, (cy - 9) * 16, cx * 16, (cy - 1) * 16, 34, c, 255);
}

/* restart: an almost closed ring with an arrow head at its end */
static void icon_restart(int cx, int cy, GfxColor c)
{
    arc(cx, cy, 7, 4, 23, c);
    int px, py, k = 23;
    circle_pt(cx, cy, 7, k, &px, &py);
    int tx = COS15[k] * 80 / 1024, ty = SIN15[k] * 80 / 1024;  /* along the travel direction, 5 px */
    int rx = SIN15[k] * 64 / 1024, ry = -COS15[k] * 64 / 1024; /* across it (outwards), 4 px */
    int pts[6] = {px + tx, py + ty, px + rx - tx / 5, py + ry - ty / 5, px - rx - tx / 5, py - ry - ty / 5};
    gfx_fill_poly_q(pts, 3, c, 255);
}

void startmenu_draw(void)
{
    if (!OPEN || !gfx_ready())
        return;
    const UiTheme *prev = ui_theme();
    follow_accent();
    ui_set_theme(&MENU_THEME); /* every ui_* call below (text, icons, buttons, the search box) uses the light look */
    const UiTheme *t = &MENU_THEME;
    UiRect m = menu_rect();

    /* glass panel: the backdrop blurred, then a soft shadow, a translucent light fill and a bright edge over it */
    int frosted = frost(m);
    glass(m, 18, frosted ? PANEL_ALPHA_FROST : PANEL_ALPHA_PLAIN);

    /* search */
    UiRect s = search_rect();
    if (clip_hits(ui_rect(m.x + PAD, s.y - 4, m.w - 2 * PAD, s.h + 8)))
    {
        ui_icon(UI_ICON_SEARCH, m.x + PAD + 9, s.y + s.h / 2, 16, t->text_dim);
        ui_edit_draw(s, &SEARCH, "Search apps...", 1);
    }

    /* list */
    int ly = m.y + PAD + SEARCH_H + LABEL_GAP;
    if (clip_hits(ui_rect(m.x + PAD + 4, ly, m.w - 2 * PAD, LABEL_H)))
        ui_text_in_color(ui_rect(m.x + PAD + 4, ly, m.w - 2 * PAD, LABEL_H), SEARCH.len ? "Results" : "Apps",
                         UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    if (VN == 0)
        ui_text_in_color(ui_rect(m.x + PAD, rows_y(), m.w - 2 * PAD, ROW_H * 2), "No apps found", UI_TEXT_BODY,
                         UI_ALIGN_CENTER, t->text_dim);
    for (int i = SCROLL; i < VN && i < SCROLL + rows_shown(); i++)
    {
        UiRect r = row_rect(i);
        if (!clip_hits(r))
            continue; /* this redraw is somewhere else (a row's hover is one row) */
        MenuItem item = item_get(VIS[i]);
        const MenuItem *it = &item;
        if (ROW_SHOWN[i] > 0) /* the highlight fades in and out (startmenu_tick) */
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_m, ui_color(t->accent),
                                      (unsigned char)((PRESS == i ? 56 : 30) * ROW_SHOWN[i] / 255));
        UiRect tile = ui_rect(r.x + 8, r.y + (r.h - 38) / 2, 38, 38);
        gfx_fill_round_rect_alpha(tile.x, tile.y, tile.w, tile.h, 11, gfx_rgb(255, 255, 255), 215);
        gfx_round_rect_alpha(tile.x, tile.y, tile.w, tile.h, 11, 1, ui_color(t->border), 150);
        ui_icon(it->icon, tile.x + tile.w / 2, tile.y + tile.h / 2, 24, t->text);
        int tx = tile.x + tile.w + 12;
        ui_text_in(ui_rect(tx, r.y + 5, r.x + r.w - tx - 8, 22), it->name, UI_TEXT_LABEL, UI_ALIGN_LEFT);
        ui_text_in(ui_rect(tx, r.y + 26, r.x + r.w - tx - 8, 18), it->desc, UI_TEXT_SMALL, UI_ALIGN_LEFT);
    }

    if (VN > rows_shown()) /* more rows than fit: a thin scroll bar at the right edge */
    {
        int rows = rows_shown(), th = rows * ROW_H - 4;
        int bh = th * rows / VN, most = VN - rows;
        int by = rows_y() + (th - bh) * SCROLL / most;
        gfx_fill_round_rect_alpha(m.x + m.w - 8, rows_y(), 3, th, 1, gfx_rgb(40, 60, 110), 30);
        gfx_fill_round_rect_alpha(m.x + m.w - 8, by, 3, bh, 1, gfx_rgb(40, 60, 110), 130);
    }

    /* footer: a faint line, the user, and "Close all" */
    int fy = foot_y();
    if (!clip_hits(ui_rect(m.x, fy - 40, m.w, FOOT_H + 42))) /* (the tip above a power button reaches up to 34 px over the footer) */
    {
        ui_menu_draw(&CTX);
        ui_set_theme(prev);
        return;
    }
    gfx_fill_rect_alpha(m.x + PAD, fy, m.w - 2 * PAD, 1, 40, 60, 110, 40);
    int cy = fy + 1 + FOOT_H / 2;
    int ax = m.x + PAD + 18;
    gfx_fill_circle(ax, cy, 17, ui_color(t->accent));
    ui_text_in_color(ui_rect(ax - 17, cy - 17, 34, 34), "H", UI_TEXT_LABEL, UI_ALIGN_CENTER, t->text_on_accent);
    ui_text_in(ui_rect(ax + 26, cy - 19, 120, 20), "Haris", UI_TEXT_LABEL, UI_ALIGN_LEFT);
    ui_text_in(ui_rect(ax + 26, cy + 1, 120, 18), "HarisOS", UI_TEXT_SMALL, UI_ALIGN_LEFT);
    for (int i = 0; i < 2; i++)
    {
        UiRect b = power_btn_rect(i);
        UiRgb tint = i == 1 ? t->danger : t->accent; /* Shut down turns red */
        /* always-visible backing: on the translucent glass a bare thin icon is hard to see */
        gfx_fill_round_rect_alpha(b.x, b.y, b.w, b.h, t->radius_m, gfx_rgb(255, 255, 255), 215);
        gfx_round_rect_alpha(b.x, b.y, b.w, b.h, t->radius_m, 1, ui_color(t->border), 170);
        if (FOOT_SHOWN[i] > 0)
            gfx_fill_round_rect_alpha(b.x, b.y, b.w, b.h, t->radius_m, ui_color(tint),
                                      (unsigned char)((FOOT_PRESS == i + 1 ? 90 : 55) * FOOT_SHOWN[i] / 255));
        GfxColor ic = ui_color(i == 1 ? t->danger : t->text); /* Shut down is always red */
        if (i == 0)
            icon_restart(b.x + b.w / 2, b.y + b.h / 2, ic);
        else
            icon_power(b.x + b.w / 2, b.y + b.h / 2, ic);
    }
    int tip = FOOT_HOVER ? FOOT_HOVER : FOOT_KEY;
    if (tip > 0) /* what the hovered / focused button does */
    {
        UiRect tp = power_tip_rect(tip);
        gfx_fill_round_rect_alpha(tp.x, tp.y, tp.w, tp.h, 9, gfx_rgb(24, 32, 54), 232);
        ui_text_in_color(tp, power_label(tip), UI_TEXT_SMALL, UI_ALIGN_CENTER, ui_rgb(255, 255, 255));
    }

    ui_menu_draw(&CTX); /* the right-click menu, over the rows (still in the light look) */

    ui_set_theme(prev);
}
