/* HarisOS start menu: light translucent glass panel with search, app list and footer. */
#include "startmenu.h"
#include "appmenu.h"
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

const UiTheme *startmenu_theme(void) { return &MENU_THEME; }

void startmenu_glass(UiRect r, int radius)
{
    /* The shadow is a set of faint rings OUTSIDE the panel, so nothing dark shows through the
     * translucent fill (gfx_shadow() would darken the panel's own edges). */
    enum
    {
        RINGS = 14
    };
    for (int k = RINGS; k >= 1; k--)
    {
        int left = RINGS - k + 1; /* 14 near the panel ... 1 at the far edge */
        int a = 44 * left * left / (RINGS * RINGS);
        if (a < 1)
            a = 1;
        gfx_round_rect_alpha(r.x - k, r.y - k, r.w + 2 * k, r.h + 2 * k, radius + k, 1, gfx_rgb(10, 20, 50),
                             (unsigned char)a);
    }
    gfx_fill_round_rect_alpha(r.x, r.y, r.w, r.h, radius, ui_color(MENU_THEME.surface), PANEL_ALPHA);
    gfx_round_rect_alpha(r.x, r.y, r.w, r.h, radius, 1, gfx_rgb(255, 255, 255), 235);
}

/* ---- state ---- */
static int OPEN;
static UiEdit SEARCH;
static int VIS[MAX_ITEMS]; /* indexes into ITEMS that match the search */
static int VN;
static int SEL;                    /* highlighted row (index into VIS), -1 = none */
static int PRESS = -1;             /* row held down with the mouse */
static int FOOT_HOVER, FOOT_PRESS; /* footer power buttons: 0 = none, 1 = Restart, 2 = Shut down */
static int FOOT_KEY;               /* footer power button with keyboard focus: 0 = none, 1 = Restart, 2 = Shut down */
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
    wm_add_damage(ui_rect(m.x - 36, m.y - 30, m.w + 72, m.h + 80)); /* the shadow reaches past the panel */
}

static void dmg_row(int i)
{
    if (i >= 0 && i < VN)
        wm_add_damage(row_rect(i));
}

static void dmg_power(int hover)
{
    if (hover <= 0)
        return;
    wm_add_damage(power_btn_rect(hover - 1));
    UiRect t = power_tip_rect(hover);
    wm_add_damage(ui_rect(t.x - 2, t.y - 2, t.w + 4, t.h + 4));
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
    FOOT_HOVER = FOOT_PRESS = FOOT_KEY = 0;
    dmg_all();
}

static void startmenu_open(void)
{
    if (OPEN)
        return;
    appmenu_close(); /* only one menu at a time */
    OPEN = 1;
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
    int foot = 0; /* which power button the pointer is on */
    if (over)
        for (int i = 0; i < 2; i++)
            if (ui_contains(power_btn_rect(i), x, y))
                foot = i + 1;

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

void startmenu_tick(int caret_flipped)
{
    if (OPEN && caret_flipped)
        dmg_search();
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
    ui_set_theme(&MENU_THEME); /* every ui_* call below (text, icons, buttons, the search box) uses the light look */
    const UiTheme *t = &MENU_THEME;
    UiRect m = menu_rect();

    /* glass panel: soft shadow, translucent light fill, bright edge */
    startmenu_glass(m, 18);

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
        if (i == SEL && !FOOT_KEY)
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
    for (int i = 0; i < 2; i++)
    {
        UiRect b = power_btn_rect(i);
        int hot = FOOT_HOVER == i + 1 || FOOT_KEY == i + 1;
        UiRgb tint = i == 1 ? t->danger : t->accent; /* Shut down turns red */
        if (hot)
            gfx_fill_round_rect_alpha(b.x, b.y, b.w, b.h, t->radius_m, ui_color(tint), FOOT_PRESS == i + 1 ? 60 : 34);
        GfxColor ic = ui_color(hot && i == 1 ? t->danger : t->text);
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

    ui_set_theme(prev);
}
