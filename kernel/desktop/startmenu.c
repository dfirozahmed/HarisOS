/* HarisOS start menu: light translucent glass panel with search, app list and footer. */
#include "startmenu.h"
#include "desktop.h"
#include "taskbar.h"
#include "ui.h"
#include "wm.h"
#include "desktop_internal.h"

/* ---- look: always a light glass theme, whatever the desktop theme is ---- */
#define RGB(r, g, b) {r, g, b}
static const UiTheme MENU_THEME = {
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

#define PANEL_ALPHA 196 /* 255 = solid; lower = more see-through */
#define MENU_W 340
#define MARGIN 10 /* gap to the screen edge and to the taskbar */
#define PAD 14
#define SEARCH_H 38
#define LABEL_GAP 12
#define LABEL_H 22
#define ROW_H 54
#define FOOT_H 58
#define MAX_ITEMS 3

typedef struct
{
    const char *name, *desc;
    UiIcon icon;
} MenuItem;

static const MenuItem ITEMS[MAX_ITEMS] = {
    {"Welcome", "The HarisOS start page", UI_ICON_LOGO},
    {"Hello window", "Text boxes and dialogs", UI_ICON_TERMINAL},
    {"Settings", "Switch dark / light theme", UI_ICON_SETTINGS},
};

/* ---- state ---- */
static int OPEN;
static UiEdit SEARCH;
static int VIS[MAX_ITEMS]; /* indexes into ITEMS that match the search */
static int VN;
static int SEL;        /* highlighted row (index into VIS), -1 = none */
static int PRESS = -1; /* row held down with the mouse */
static int FOOT_HOVER, FOOT_PRESS;
static unsigned LAST_DOWN;
static unsigned FILTER_SIG;

/* ---- geometry (small functions: no big structs are copied) ---- */
static int menu_h(void)
{
    return PAD + SEARCH_H + LABEL_GAP + LABEL_H + MAX_ITEMS * ROW_H + 8 + 1 + FOOT_H;
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
    return ui_rect(m.x + 8, rows_y() + i * ROW_H, m.w - 16, ROW_H - 4);
}

static int foot_y(void) { return rows_y() + MAX_ITEMS * ROW_H + 8; }

static UiRect close_btn_rect(void)
{
    UiRect m = menu_rect();
    return ui_rect(m.x + m.w - PAD - 92, foot_y() + 1 + (FOOT_H - 34) / 2, 92, 34);
}

/* ---- damage ---- */
static void dmg_all(void)
{
    UiRect m = menu_rect();
    wm_add_damage(ui_rect(m.x - 36, m.y - 30, m.w + 72, m.h + 80)); /* the shadow reaches past the panel */
}

static void dmg_row(int i)
{
    if (i >= 0 && i < VN)
        wm_add_damage(row_rect(i));
}

static void dmg_search(void)
{
    UiRect s = search_rect();
    wm_add_damage(ui_inset(s, -3));
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
    for (int i = 0; i < MAX_ITEMS; i++)
        if (has_text(ITEMS[i].name, SEARCH.text) || has_text(ITEMS[i].desc, SEARCH.text))
            VIS[VN++] = i;
    if (VN == 0)
        SEL = -1;
    else if (reset_selection || SEL < 0 || SEL >= VN)
        SEL = 0;
}

/* ---- open / close / actions ---- */
int startmenu_is_open(void) { return OPEN; }

void startmenu_close(void)
{
    if (!OPEN)
        return;
    OPEN = 0;
    SEARCH.dragging = 0;
    PRESS = -1;
    FOOT_HOVER = FOOT_PRESS = 0;
    dmg_all();
}

static void startmenu_open(void)
{
    if (OPEN)
        return;
    OPEN = 1;
    ui_edit_set_text(&SEARCH, "");
    FILTER_SIG = text_sig("");
    refilter(1);
    PRESS = -1;
    FOOT_HOVER = FOOT_PRESS = 0;
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

static void close_all_windows(void)
{
    for (int guard = 0; guard < 8; guard++)
    {
        Window *list[TASKBAR_MAX_APPS];
        int n = wm_list(list, TASKBAR_MAX_APPS);
        if (n == 0)
            break;
        for (int i = 0; i < n; i++)
            if (wm_is_open(list[i]))
                wm_close(list[i]);
    }
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
        desktop_toggle_theme();
        break;
    default:
        break;
    }
}

/* ---- keyboard ---- */
int startmenu_key(const KeyEvent *k)
{
    if (!OPEN || !k->pressed)
        return OPEN;
    switch (k->key)
    {
    case KEY_ESC:
        startmenu_close();
        return 1;
    case KEY_ENTER:
        if (SEL >= 0 && SEL < VN)
            activate(VIS[SEL]);
        return 1;
    case KEY_UP:
    case KEY_DOWN:
    case KEY_TAB:
    {
        if (VN == 0)
            return 1;
        int back = k->key == KEY_UP || (k->key == KEY_TAB && (k->mods & KMOD_SHIFT));
        int old = SEL;
        SEL = SEL < 0 ? (back ? VN - 1 : 0) : (SEL + (back ? VN - 1 : 1)) % VN;
        dmg_row(old);
        dmg_row(SEL);
        return 1;
    }
    default:
        break;
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
    if (!OPEN)
        return 0;

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
        for (int i = 0; i < VN; i++)
            if (ui_contains(row_rect(i), x, y))
                hit = i;
    int foot = over && ui_contains(close_btn_rect(), x, y);

    /* hover: the row under the pointer becomes the highlighted one */
    if (hit >= 0 && hit != SEL && PRESS < 0)
    {
        dmg_row(SEL);
        SEL = hit;
        dmg_row(SEL);
    }
    if (foot != FOOT_HOVER)
    {
        FOOT_HOVER = foot;
        wm_add_damage(close_btn_rect());
    }

    if (edge)
    {
        PRESS = hit;
        FOOT_PRESS = foot;
        dmg_row(hit);
        if (foot)
            wm_add_damage(close_btn_rect());
    }
    if (release)
    {
        int act = (PRESS >= 0 && hit == PRESS) ? VIS[PRESS] : -1;
        int do_close = FOOT_PRESS && foot;
        int old = PRESS;
        PRESS = -1;
        FOOT_PRESS = 0;
        dmg_row(old);
        wm_add_damage(close_btn_rect());
        if (do_close)
        {
            startmenu_close();
            close_all_windows();
        }
        else if (act >= 0)
            activate(act);
    }
    return over;
}

void startmenu_tick(int caret_flipped)
{
    if (OPEN && caret_flipped)
        dmg_search();
}

/* ---- drawing ---- */
void startmenu_draw(void)
{
    if (!OPEN || !gfx_ready())
        return;
    const UiTheme *prev = ui_theme();
    ui_set_theme(&MENU_THEME); /* every ui_* call below (text, icons, buttons, the search box) uses the light look */
    const UiTheme *t = &MENU_THEME;
    UiRect m = menu_rect();

    /* glass panel: soft shadow, translucent light fill, bright edge */
    gfx_shadow(m.x, m.y, m.w, m.h, 18, 30, 0, 10, 90);
    gfx_fill_round_rect_alpha(m.x, m.y, m.w, m.h, 18, ui_color(t->surface), PANEL_ALPHA);
    gfx_round_rect_alpha(m.x, m.y, m.w, m.h, 18, 1, gfx_rgb(255, 255, 255), 235);

    /* search */
    UiRect s = search_rect();
    ui_icon(UI_ICON_SEARCH, m.x + PAD + 9, s.y + s.h / 2, 16, t->text_dim);
    ui_edit_draw(s, &SEARCH, "Search apps...", 1);

    /* list */
    int ly = m.y + PAD + SEARCH_H + LABEL_GAP;
    ui_text_in_color(ui_rect(m.x + PAD + 4, ly, m.w - 2 * PAD, LABEL_H), SEARCH.len ? "Results" : "Apps",
                     UI_TEXT_SMALL, UI_ALIGN_LEFT, t->text_dim);
    if (VN == 0)
        ui_text_in_color(ui_rect(m.x + PAD, rows_y(), m.w - 2 * PAD, ROW_H * 2), "No apps found", UI_TEXT_BODY,
                         UI_ALIGN_CENTER, t->text_dim);
    for (int i = 0; i < VN; i++)
    {
        const MenuItem *it = &ITEMS[VIS[i]];
        UiRect r = row_rect(i);
        if (i == SEL)
            gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, t->radius_m, ui_color(t->accent), PRESS == i ? 56 : 30);
        UiRect tile = ui_rect(r.x + 8, r.y + (r.h - 38) / 2, 38, 38);
        gfx_fill_round_rect_alpha(tile.x, tile.y, tile.w, tile.h, 11, gfx_rgb(255, 255, 255), 215);
        gfx_round_rect_alpha(tile.x, tile.y, tile.w, tile.h, 11, 1, ui_color(t->border), 150);
        ui_icon(it->icon, tile.x + tile.w / 2, tile.y + tile.h / 2, 24, t->text);
        int tx = tile.x + tile.w + 12;
        ui_text_in(ui_rect(tx, r.y + 5, r.x + r.w - tx - 8, 22), it->name, UI_TEXT_LABEL, UI_ALIGN_LEFT);
        ui_text_in(ui_rect(tx, r.y + 26, r.x + r.w - tx - 8, 18), it->desc, UI_TEXT_SMALL, UI_ALIGN_LEFT);
    }

    /* footer: a faint line, the user, and "Close all" */
    int fy = foot_y();
    gfx_fill_rect_alpha(m.x + PAD, fy, m.w - 2 * PAD, 1, 40, 60, 110, 40);
    int cy = fy + 1 + FOOT_H / 2;
    int ax = m.x + PAD + 18;
    gfx_fill_circle(ax, cy, 17, ui_color(t->accent));
    ui_text_in_color(ui_rect(ax - 17, cy - 17, 34, 34), "H", UI_TEXT_LABEL, UI_ALIGN_CENTER, t->text_on_accent);
    ui_text_in(ui_rect(ax + 26, cy - 19, 120, 20), "Haris", UI_TEXT_LABEL, UI_ALIGN_LEFT);
    ui_text_in(ui_rect(ax + 26, cy + 1, 120, 18), "HarisOS", UI_TEXT_SMALL, UI_ALIGN_LEFT);
    ui_button(close_btn_rect(), "Close all", UI_BTN_GHOST,
              FOOT_PRESS ? UI_STATE_PRESSED : (FOOT_HOVER ? UI_STATE_HOVER : UI_STATE_NORMAL));

    ui_set_theme(prev);
}
