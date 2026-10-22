/* ============================================================
 * HarisUI - a small, reusable UI toolkit for HarisOS
 *
 * Built only on the gfx layer (gfx.h + font.h), so it works anywhere gfx works:
 * desktop, window manager, installer, panic screen...
 *
 * ============================================================ */
#ifndef HARIS_UI_H
#define HARIS_UI_H

#include "font.h"
#include "gfx.h"
#include "keyboard.h"

/* ---- colors -------------------------------------------------- */
typedef struct
{
    unsigned char r, g, b;
} UiRgb;

UiRgb ui_rgb(unsigned char r, unsigned char g, unsigned char b);
UiRgb ui_rgb_mix(UiRgb a, UiRgb b, unsigned char t); /* t: 0 = a, 255 = b */
GfxColor ui_color(UiRgb c);                          /* to the framebuffer format */

/* ---- theme --------------------------------------------------- */
typedef struct
{
    /* wallpaper */
    UiRgb bg_top, bg_bottom, glow1, glow2;
    /* surfaces */
    UiRgb surface;     /* windows, cards, menus */
    UiRgb surface_alt; /* inputs, secondary buttons, tracks */
    UiRgb titlebar;
    UiRgb border;
    /* text */
    UiRgb text, text_dim, text_on_accent;
    /* colors with a meaning */
    UiRgb accent, accent_hi;
    UiRgb success, warning, danger;
    /* shape and spacing */
    int radius_s, radius_m, radius_l;
    int pad, gap;
    int titlebar_h;
    int shadow; /* 0..255 */
    /* typography */
    const Font *font_small, *font_body, *font_label, *font_title, *font_heading;
} UiTheme;

extern const UiTheme ui_theme_dark, ui_theme_light;
const UiTheme *ui_theme(void);
void ui_set_theme(const UiTheme *t); /* NULL = default (dark) */

/* ---- layout -------------------------------------------------- */
typedef struct
{
    int x, y, w, h;
} UiRect;

UiRect ui_rect(int x, int y, int w, int h);
UiRect ui_inset(UiRect r, int amount);
UiRect ui_center(UiRect outer, int w, int h);
/* Cut a strip off one side: returns the strip and shrinks *r. */
UiRect ui_cut_top(UiRect *r, int h);
UiRect ui_cut_bottom(UiRect *r, int h);
UiRect ui_cut_left(UiRect *r, int w);
UiRect ui_cut_right(UiRect *r, int w);
int ui_contains(UiRect r, int x, int y); /* hit testing for mouse input */
void ui_clip(UiRect r);                  /* restrict drawing to r (gfx_reset_clip() to undo) */

/* ---- text ---------------------------------------------------- */
typedef enum
{
    UI_TEXT_SMALL,  /* captions, hints */
    UI_TEXT_BODY,   /* normal text */
    UI_TEXT_DIM,    /* body size, secondary color */
    UI_TEXT_LABEL,  /* medium weight: buttons, field labels */
    UI_TEXT_TITLE,  /* section titles */
    UI_TEXT_HEADING /* large heading */
} UiTextStyle;

typedef enum
{
    UI_ALIGN_LEFT,
    UI_ALIGN_CENTER,
    UI_ALIGN_RIGHT
} UiAlign;

const Font *ui_font(UiTextStyle style);
int ui_text_width(const char *s, UiTextStyle style);
int ui_text_height(UiTextStyle style);
/* x,y = top-left of the text box. Color comes from the style. */
void ui_text(int x, int y, const char *s, UiTextStyle style);
void ui_text_color(int x, int y, const char *s, UiTextStyle style, UiRgb color);
/* Text placed inside a rectangle: aligned horizontally, centered vertically,
 * and shortened with "..." if it does not fit. */
void ui_text_in(UiRect r, const char *s, UiTextStyle style, UiAlign align);
void ui_text_in_color(UiRect r, const char *s, UiTextStyle style, UiAlign align, UiRgb color);

/* ---- surfaces ------------------------------------------------ */
void ui_background(void); /* full-screen wallpaper from the theme */
void ui_background1(void);
void ui_card(UiRect r); /* rounded panel with soft shadow and border */
/* Window with title bar. Returns the content area below the title bar. */
UiRect ui_window(UiRect r, const char *title, int active);
/* Same, with square corners and no shadow: for a window that fills the screen. */
UiRect ui_window_flat(UiRect r, const char *title, int active);
/* Look of the window buttons (close / minimize / maximize = 0 / 1 / 2) in the NEXT ui_window /
 * ui_window_flat / ui_dialog call: hovered (lighter, symbol shown) and pressed (darker, a bit smaller).
 * -1 = none. The state is used once and then cleared. */
void ui_window_button_state(int hover, int pressed);
/* Dialog frame: only a close button (no minimize / maximize). */
UiRect ui_dialog(UiRect r, const char *title, int active);
void ui_separator(int x, int y, int w);

/* ---- controls ------------------------------------------------ */
typedef enum
{
    UI_STATE_NORMAL,
    UI_STATE_HOVER,
    UI_STATE_PRESSED,
    UI_STATE_DISABLED
} UiState;
typedef enum
{
    UI_BTN_PRIMARY,
    UI_BTN_SECONDARY,
    UI_BTN_DANGER,
    UI_BTN_GHOST
} UiButtonKind;

void ui_button(UiRect r, const char *label, UiButtonKind kind, UiState state);
/* Same, with an icon (a UiIcon value, or -1 for none) before the label. An empty label centers the icon. */
void ui_button_icon(UiRect r, const char *label, int icon, UiButtonKind kind, UiState state);

struct UiButton;
typedef void (*UiButtonFn)(struct UiButton *btn, void *user);

typedef struct UiButton
{
    UiRect r;
    const char *label;
    UiButtonKind kind;
    int enabled;         /* 0 = greyed out, no events */
    UiButtonFn on_click; /* mouse released on the button (press + release both inside) */
    UiButtonFn on_press; /* mouse went down on the button   (may be NULL) */
    UiButtonFn on_hover; /* pointer entered the button      (may be NULL) */
    UiButtonFn on_leave; /* pointer left the button         (may be NULL) */
    void *user;          /* your own data, passed to every callback */
    int icon;            /* UiIcon + 1 drawn before the label, 0 = no icon */
} UiButton;

/* Static initializer: UiButton b = UI_BUTTON("Save", UI_BTN_PRIMARY, on_save, my_data); */
#define UI_BUTTON(label_, kind_, click_, user_) \
    {{0, 0, 0, 0}, (label_), (kind_), 1, (click_), 0, 0, 0, (user_)}

#define UI_MAX_BUTTONS 16
typedef struct
{
    UiButton *items[UI_MAX_BUTTONS];
    int count;
    int hover;   /* index under the pointer, -1 = none */
    int pressed; /* index being held down, -1 = none */
    unsigned last_buttons;
} UiButtonSet;

void ui_buttons_init(UiButtonSet *s);
int ui_buttons_add(UiButtonSet *s, UiButton *b); /* 0 = ok, -1 = set is full */
void ui_buttons_clear(UiButtonSet *s);           /* remove all (call before re-layout) */
void ui_buttons_draw(const UiButtonSet *s);
/* Feed the mouse state. Fires on_hover / on_leave / on_press / on_click.
 * Returns 1 if a button's look changed (hover/press), so you should redraw. */
int ui_buttons_pointer(UiButtonSet *s, int x, int y, unsigned buttons);
/* Same, but says how much to redraw: 0 = nothing changed, 1 = redraw the whole window (a click or another
 * callback ran, which may have changed anything), 2 = only a hover / press highlight changed: redraw *dirty. */
int ui_buttons_pointer_dirty(UiButtonSet *s, int x, int y, unsigned buttons, UiRect *dirty);
/* Union of all button rectangles (to clear before a partial redraw). */
UiRect ui_buttons_bounds(const UiButtonSet *s);

void ui_progress(UiRect r, int percent);                /* 0..100 */
void ui_slider(UiRect r, int percent);                  /* track + knob; r.h of 20 looks best; 0..100 */
void ui_toggle(UiRect r, int on);                      /* switch; 40x22 looks best */
int ui_badge(int x, int y, const char *s, UiRgb color); /* small pill; returns its width */
void ui_text_field(UiRect r, const char *text, const char *placeholder, int focused);

/* ---- single-line text input ----------------------------------
 *
 *   UiEdit e = {0};                       // empty
 *   if (ui_edit_key(&e, &key_event)) ...  // feed key events; 1 = text, cursor or selection changed
 *   ui_edit_draw(rect, &e, "Type here...", focused);
 *   ui_edit_mouse(&e, x, y, buttons);     // click / drag / double-click (after a press inside the field)
 *
 * Keys handled:
 *   typing, Backspace, Delete, Left/Right, Home/End
 *   Shift+Left/Right/Home/End   extend the selection
 *   Ctrl+Left/Right             jump a word        (with Shift: select a word)
 *   Ctrl+Backspace/Delete       delete the previous / next word
 *   Ctrl+A                      select all
 *   Ctrl+C / Ctrl+X / Ctrl+V    copy / cut / paste (one shared clipboard; Ctrl+Insert, Shift+Delete,
 *                               Shift+Insert work too)
 *   Ctrl+Z / Ctrl+Y             undo / redo (Ctrl+Shift+Z = redo)
 *   Typing or pasting over a selection replaces it.
 * Enter, Tab, Escape and other shortcuts are NOT handled: they are left for your code.
 *
 * Mouse (ui_edit_mouse): press places the caret, dragging selects, double-click selects a
 * word, triple-click selects everything.
 *
 * Options (set e.flags, e.max): UI_EDIT_PASSWORD shows '*' and blocks copy / cut;
 * UI_EDIT_READONLY allows moving, selecting and copying but no changes;
 * UI_EDIT_NUMERIC accepts only the digits 0-9. e.max = 0 means UI_EDIT_MAX. */
#define UI_EDIT_MAX 260
#define UI_EDIT_PASSWORD 1
#define UI_EDIT_READONLY 2
#define UI_EDIT_NUMERIC 4
typedef struct
{
    char text[UI_EDIT_MAX + 1];
    int len;
    int cursor;   /* 0..len: where typing goes */
    int anchor;   /* the other end of the selection; anchor == cursor means no selection */
    int flags;    /* UI_EDIT_* options */
    int max;      /* longest text allowed; 0 = UI_EDIT_MAX */
    UiRect field; /* where it was last drawn (so only the caret area can be redrawn) */

    /* ---- internal state: leave alone ---- */
    int scroll;   /* first visible character */
    int dragging; /* left mouse button went down on this field */
    int clicks;   /* 1, 2, 3: consecutive clicks (double / triple click) */
    int click_x;
    unsigned long long click_t;
    int group;    /* 1 while consecutive typing is one undo step */
    char undo_text[UI_EDIT_MAX + 1], redo_text[UI_EDIT_MAX + 1];
    int undo_len, undo_cur, undo_valid;
    int redo_len, redo_cur, redo_valid;
} UiEdit;

void ui_edit_clear(UiEdit *e);
int ui_edit_key(UiEdit *e, const KeyEvent *k);
void ui_edit_draw(UiRect r, UiEdit *e, const char *placeholder, int focused);

/* Mouse: call with the pointer position and button state. The first call with the left button
 * down must be a press inside e->field (decide that yourself, e.g. in a window's on_press);
 * keep calling while the button is held (to select by dragging) and once more when it is released.
 * Returns 1 if the caret or selection changed. */
int ui_edit_mouse(UiEdit *e, int x, int y, unsigned buttons);

/* Helpers */
void ui_edit_set_text(UiEdit *e, const char *s);    /* replace the text (cursor at the end, undo cleared) */
void ui_edit_select_all(UiEdit *e);
int ui_edit_has_selection(const UiEdit *e);
int ui_edit_selection(const UiEdit *e, int *from, int *to); /* returns 1 and the range [from,to) if any */
int ui_edit_insert(UiEdit *e, const char *s);        /* type a string at the cursor; returns how many characters went in */

/* Shared clipboard (plain text) */
/* The clipboard keeps up to 16 MiB (in the kernel heap). set: 1 = copied, 0 = too long / no memory (the old text stays). */
int ui_clipboard_set(const char *s, int len);
int ui_clipboard_get(char *out, int cap);            /* copies up to cap-1 chars, returns the length */
const char *ui_clipboard_text(int *len);             /* the whole text without copying it (valid until the next set) */

/* ---- blinking caret ----------------------------------------------
 * The caret of a focused UiEdit blinks (visible 530 ms, hidden 530 ms). ui_edit_draw()
 * only reads a flag, so drawing never depends on the clock. Something must flip that flag:
 *   ui_caret_restart()   caret visible right now and the blink starts over (call on every key press)
 *   ui_caret_update()    look at the clock; returns 1 if the caret just appeared or disappeared,
 *                        so the field has to be redrawn. Call it often (every main-loop pass).
 * Needs timer_init(); without a working timer the caret simply stays visible. */
void ui_caret_restart(void);
int ui_caret_update(void);
int ui_caret_visible(void);

/* ---- icons --------------------------------------------------- */
typedef enum
{
    UI_ICON_LOGO, /* HarisOS "H" */
    UI_ICON_FILES,
    UI_ICON_TERMINAL,
    UI_ICON_SETTINGS,
    UI_ICON_BROWSER,
    UI_ICON_SEARCH,
    UI_ICON_WIFI,
    UI_ICON_VOLUME,
    UI_ICON_BATTERY,
    UI_ICON_CLOSE,
    UI_ICON_CHECK,
    UI_ICON_ARROW_LEFT, /* back */
    UI_ICON_ARROW_RIGHT, /* forward */
    UI_ICON_ARROW_UP,   /* up one level */
    UI_ICON_FOLDER_PLUS, /* new folder */
    UI_ICON_CUT,
    UI_ICON_COPY,
    UI_ICON_PASTE,
    UI_ICON_RENAME,
    UI_ICON_FILE_PLUS, /* new file */
    UI_ICON_TRASH,     /* delete */
    UI_ICON_REFRESH,
    UI_ICON_SORT,
    UI_ICON_TASKMGR, /* performance graph */
    UI_ICON_DISPLAY, /* monitor on a stand */
    UI_ICON_DEVMGR,  /* hardware chip */
    UI_ICON_LIST,    /* rows with bullets */
    UI_ICON_CHART,   /* line graph */
    UI_ICON_MEMORY,  /* RAM module */
    UI_ICON_CPU,     /* processor, outline */
    UI_ICON_DISK,    /* drive */
    UI_ICON_NOTEPAD  /* page with text lines */
} UiIcon;

/* Draw a vector icon centered at (cx, cy), `size` pixels wide. App icons
 * (files, terminal, browser, logo) have their own colors; the others use `tint`. */
void ui_icon(UiIcon icon, int cx, int cy, int size, UiRgb tint);
#endif
