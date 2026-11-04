/* Host-side tests for the desktop right-click menu (kernel/desktop/deskmenu.c) on top of the real UiMenu (kernel/ui/ui_menu.c):
 * opening, the other menus closing, hover, press / release, a click away, keys, and staying on the screen above the taskbar. */
#include "ui_internal.h"
#include "deskmenu.h"
#include "taskbar.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- stubs ---- */
static UiTheme THEME_STORE;
const UiTheme *THEME = &THEME_STORE;
UiRect ui_rect(int x, int y, int w, int h) { UiRect r = {x, y, w, h}; return r; }
int ui_contains(UiRect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
int ui_text_width(const char *s, UiTextStyle st) { (void)st; return (int)strlen(s) * 8; }
GfxColor ui_color(UiRgb c) { (void)c; return 0; }
static int DRAWN_LABELS;
void gfx_shadow(int x, int y, int w, int h, int r, int b, int dx, int dy, unsigned char s) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)b; (void)dx; (void)dy; (void)s; }
void gfx_fill_round_rect(int x, int y, int w, int h, int r, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)c; }
void gfx_round_rect(int x, int y, int w, int h, int r, int t, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)t; (void)c; }
void gfx_fill_round_rect_alpha(int x, int y, int w, int h, int r, GfxColor c, unsigned char a) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)c; (void)a; }
void ui_separator(int x, int y, int w) { (void)x; (void)y; (void)w; }
void ui_icon(UiIcon i, int cx, int cy, int size, UiRgb t) { (void)i; (void)cx; (void)cy; (void)size; (void)t; }
void ui_text_in_color(UiRect r, const char *s, UiTextStyle st, UiAlign a, UiRgb c) { (void)r; (void)s; (void)st; (void)a; (void)c; DRAWN_LABELS++; }
unsigned gfx_width(void) { return 1280; }
unsigned gfx_height(void) { return 800; }
int gfx_ready(void) { return 1; }

static int STARTMENU_CLOSES, APPMENU_CLOSES, VOLMENU_CLOSES, DAMAGES, INVALIDATES, SETTINGS_OPENS, ICON_OPENED = -1, ARRANGES;
static char OPENED_APP[64];
static UiRect LAST_DAMAGE;
void startmenu_close(void) { STARTMENU_CLOSES++; }
void appmenu_close(void) { APPMENU_CLOSES++; }
void volmenu_close(void) { VOLMENU_CLOSES++; }
void wm_add_screen_damage(UiRect r) { DAMAGES++; LAST_DAMAGE = r; }
void wm_invalidate(void) { INVALIDATES++; }
void desktop_open_settings(void) { SETTINGS_OPENS++; }
void desktop_open_app(const char *name) { strncpy(OPENED_APP, name, sizeof OPENED_APP - 1); }
void deskicons_open(int icon) { ICON_OPENED = icon; }
void deskicons_arrange(void) { ARRANGES++; }

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

enum { L = 1, R = 2 }; /* buttons */

/* click the left button on (x, y): press, then release */
static int click(int x, int y)
{
    deskmenu_pointer(x, y, 0);
    deskmenu_pointer(x, y, L);
    return deskmenu_pointer(x, y, 0);
}

static void reset(void)
{
    STARTMENU_CLOSES = APPMENU_CLOSES = VOLMENU_CLOSES = DAMAGES = INVALIDATES = SETTINGS_OPENS = ARRANGES = 0;
    ICON_OPENED = -1;
    OPENED_APP[0] = 0;
    deskmenu_close();
    deskmenu_pointer(0, 0, 0); /* the button memory: nothing held */
    DAMAGES = 0;
}

int main(void)
{
    /* closed: it takes nothing, draws nothing */
    CHECK(!deskmenu_is_open());
    CHECK(deskmenu_pointer(100, 100, 0) == 0 && deskmenu_pointer(100, 100, L) == 0 && deskmenu_pointer(100, 100, 0) == 0);
    KeyEvent k = {KEY_DOWN, 0, 1, 0};
    CHECK(deskmenu_key(&k) == 0);
    deskmenu_draw();
    CHECK(DRAWN_LABELS == 0);

    /* open: the other menus close, the area is marked, the menu draws its three entries */
    reset();
    deskmenu_open(100, 100);
    CHECK(deskmenu_is_open());
    CHECK(STARTMENU_CLOSES == 1 && APPMENU_CLOSES == 1 && VOLMENU_CLOSES == 1);
    CHECK(DAMAGES >= 1);
    UiRect b = deskmenu_bounds();
    CHECK(b.x < 100 && b.y < 100 && b.w > 180 && b.h > 100);
    deskmenu_draw();
    CHECK(DRAWN_LABELS == 4); /* Personalize, Display settings, Arrange icons, Refresh */
    CHECK(deskmenu_pointer(110, 110, 0) == 1);   /* over the menu */
    CHECK(deskmenu_pointer(900, 600, 0) == 0);   /* not over it */

    /* rows: Personalize y+21, Display settings y+51, Arrange icons y+81, (separator), Refresh y+120 */
    reset();
    deskmenu_open(100, 100);
    DAMAGES = 0;
    deskmenu_pointer(140, 121, 0); /* hover Personalize */
    CHECK(DAMAGES >= 1);           /* the highlight changed: the menu is drawn again */
    DAMAGES = 0;
    deskmenu_pointer(141, 122, 0); /* same item */
    CHECK(DAMAGES == 0);
    click(140, 121);
    CHECK(SETTINGS_OPENS == 1 && !deskmenu_is_open());
    CHECK(DAMAGES >= 1); /* the closed menu's area is cleared */

    reset();
    deskmenu_open(100, 100);
    click(140, 151);
    CHECK(strcmp(OPENED_APP, "Display Settings") == 0 && !deskmenu_is_open());

    reset();
    deskmenu_open(100, 100);
    click(140, 220);
    CHECK(INVALIDATES == 1 && !deskmenu_is_open());

    reset();
    deskmenu_open(100, 100);
    click(140, 181);
    CHECK(ARRANGES == 1 && !deskmenu_is_open() && ICON_OPENED == -1);

    /* on a desktop icon: "Open" comes first and opens that icon */
    reset();
    deskmenu_open_icon(100, 100, 3);
    CHECK(deskmenu_is_open());
    DRAWN_LABELS = 0;
    deskmenu_draw();
    CHECK(DRAWN_LABELS == 5); /* Open and the four of the bare desktop */
    click(140, 121);
    CHECK(ICON_OPENED == 3 && !deskmenu_is_open() && SETTINGS_OPENS == 0);
    reset();
    deskmenu_open_icon(100, 100, 3);
    click(140, 160); /* Open is 106..136, the separator 136..145, so Personalize is 145..175 */
    CHECK(SETTINGS_OPENS == 1 && ICON_OPENED == -1);
    reset();
    deskmenu_open(100, 100); /* the bare desktop: no Open, even after an icon's menu */
    DRAWN_LABELS = 0;
    deskmenu_draw();
    CHECK(DRAWN_LABELS == 4);

    /* the separator takes the press but runs nothing, and the menu stays */
    reset();
    deskmenu_open(100, 100);
    click(140, 200);
    CHECK(deskmenu_is_open() && SETTINGS_OPENS == 0 && INVALIDATES == 0 && OPENED_APP[0] == 0);

    /* press on one item, release on another: nothing runs, the menu stays */
    deskmenu_pointer(140, 121, 0);
    deskmenu_pointer(140, 121, L);
    deskmenu_pointer(140, 151, 0);
    CHECK(deskmenu_is_open() && SETTINGS_OPENS == 0 && OPENED_APP[0] == 0);

    /* a click away closes it and says it did not take the click, so the click goes on to what is under it */
    reset();
    deskmenu_open(100, 100);
    deskmenu_pointer(600, 500, 0);
    CHECK(deskmenu_pointer(600, 500, L) == 0 && !deskmenu_is_open());
    CHECK(DAMAGES >= 1);
    deskmenu_pointer(600, 500, 0);

    /* a right click away closes it too; a right click on the menu does not */
    reset();
    deskmenu_open(100, 100);
    deskmenu_pointer(140, 121, R);
    deskmenu_pointer(140, 121, 0);
    CHECK(deskmenu_is_open());
    deskmenu_pointer(600, 500, R);
    CHECK(!deskmenu_is_open());
    deskmenu_pointer(600, 500, 0);

    /* the pointer left the window (x < 0): the highlight goes, nothing else happens */
    reset();
    deskmenu_open(100, 100);
    deskmenu_pointer(140, 121, 0);
    CHECK(deskmenu_pointer(-1, -1, 0) == 0 && deskmenu_is_open());

    /* keys: Down, Down, Enter = Display settings; Esc closes; keys while closed are not taken */
    reset();
    deskmenu_open(100, 100);
    k.key = KEY_DOWN;
    CHECK(deskmenu_key(&k) == 1);
    CHECK(deskmenu_key(&k) == 1);
    k.key = KEY_ENTER;
    CHECK(deskmenu_key(&k) == 1);
    CHECK(strcmp(OPENED_APP, "Display Settings") == 0 && !deskmenu_is_open());
    CHECK(deskmenu_key(&k) == 0);
    deskmenu_open(100, 100);
    k.key = KEY_ESC;
    CHECK(deskmenu_key(&k) == 1 && !deskmenu_is_open());
    CHECK(SETTINGS_OPENS == 0 && INVALIDATES == 0);

    /* opening again replaces the old one (a click away followed by a right click elsewhere) */
    reset();
    deskmenu_open(100, 100);
    deskmenu_open(300, 300);
    CHECK(deskmenu_is_open());
    CHECK(deskmenu_pointer(320, 321, 0) == 1);
    CHECK(deskmenu_pointer(110, 110, 0) == 0); /* the first place no longer belongs to it */

    /* near the edges: the menu stays on the screen and above the taskbar */
    reset();
    deskmenu_open(1275, 795);
    b = deskmenu_bounds();
    UiRect inner = {b.x + 16, b.y + 16, b.w - 32, b.h - 40}; /* the panel inside its shadow margin */
    CHECK(inner.x + inner.w <= 1280 && inner.y + inner.h <= 800 - TASKBAR_HEIGHT);
    deskmenu_open(-30, -30);
    b = deskmenu_bounds();
    CHECK(b.x + 16 >= 0 && b.y + 16 >= 0);

    printf("deskmenu tests passed\n");
    return 0;
}
