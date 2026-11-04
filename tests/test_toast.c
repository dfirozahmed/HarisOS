/* Host-side tests for the desktop toast (kernel/desktop/toast.c): where it is, what it says, how long it stays, hover, click, replacing,
 * long texts. Drawing is recorded. */
#include "toast.h"
#include "taskbar.h"
#include "mouse.h"
#include "timer.h"
#include "wm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- stubs ---- */
static UiTheme THEME_STORE;
const UiTheme *ui_theme(void) { return &THEME_STORE; }
UiRect ui_rect(int x, int y, int w, int h) { UiRect r = {x, y, w, h}; return r; }
int ui_contains(UiRect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
int ui_text_width(const char *s, UiTextStyle st) { (void)st; return (int)strlen(s) * 8; }
GfxColor ui_color(UiRgb c) { return ((GfxColor)c.r << 16) | ((GfxColor)c.g << 8) | c.b; }
static int SHADOWS, FILLS, TEXTS;
static GfxColor FILL_COLOR[4];
static char LINES[4][TOAST_TEXT_MAX + 1];
static UiRect CARD;
void gfx_shadow(int x, int y, int w, int h, int r, int b, int dx, int dy, unsigned char s) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)b; (void)dx; (void)dy; (void)s; SHADOWS++; }
void gfx_fill_round_rect(int x, int y, int w, int h, int r, GfxColor c)
{
    (void)r;
    if (FILLS == 0)
        CARD = ui_rect(x, y, w, h);
    if (FILLS < 4)
        FILL_COLOR[FILLS] = c;
    FILLS++;
    (void)x; (void)y; (void)w; (void)h;
}
void gfx_round_rect(int x, int y, int w, int h, int r, int t, GfxColor c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)t; (void)c; }
void ui_text_in_color(UiRect r, const char *s, UiTextStyle st, UiAlign a, UiRgb c)
{
    (void)r; (void)st; (void)a; (void)c;
    if (TEXTS < 4)
        strncpy(LINES[TEXTS], s, TOAST_TEXT_MAX);
    TEXTS++;
}
unsigned gfx_width(void) { return 1280; }
unsigned gfx_height(void) { return 800; }
int gfx_ready(void) { return 1; }
static unsigned long long NOW;
unsigned long long timer_ms(void) { return NOW; }
static int DAMAGES;
static UiRect LAST_DAMAGE;
void wm_add_screen_damage(UiRect r) { DAMAGES++; LAST_DAMAGE = r; }

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void draw(void)
{
    SHADOWS = FILLS = TEXTS = 0;
    toast_draw();
}

int main(void)
{
    THEME_STORE.accent_hi = (UiRgb){1, 0, 0};
    THEME_STORE.success = (UiRgb){2, 0, 0};
    THEME_STORE.warning = (UiRgb){3, 0, 0};
    THEME_STORE.danger = (UiRgb){4, 0, 0};
    THEME_STORE.radius_m = 12;

    /* nothing shown: nothing happens */
    CHECK(!toast_is_open());
    CHECK(toast_tick() == 0);
    CHECK(toast_pointer(1100, 700, 0) == 0 && toast_pointer(1100, 700, MOUSE_LEFT) == 0);
    toast_pointer(1100, 700, 0);
    draw();
    CHECK(SHADOWS == 0 && FILLS == 0 && TEXTS == 0);
    DAMAGES = 0;
    toast_dismiss();
    CHECK(DAMAGES == 0);
    toast_show(TOAST_INFO, 0);
    toast_show(TOAST_INFO, "");
    CHECK(!toast_is_open() && DAMAGES == 0);

    /* a short text: a small card in the bottom right corner, above the taskbar, its area marked */
    NOW = 1000;
    toast_show(TOAST_INFO, "Saved");
    CHECK(toast_is_open());
    CHECK(DAMAGES == 1);
    draw();
    CHECK(SHADOWS == 1 && TEXTS == 1 && strcmp(LINES[0], "Saved") == 0);
    CHECK(CARD.w == 200 && CARD.h == 2 * 14 + 20);                     /* the minimum width; one line */
    CHECK(CARD.x + CARD.w == 1280 - 16);                               /* 16 px from the right edge */
    CHECK(CARD.y + CARD.h == 800 - TASKBAR_HEIGHT - 12);               /* 12 px above the taskbar */
    UiRect b = toast_bounds();
    CHECK(b.x < CARD.x && b.y < CARD.y && b.x + b.w > CARD.x + CARD.w + 14 && b.y + b.h > CARD.y + CARD.h + 19); /* the shadow is inside the redraw area */
    CHECK(LAST_DAMAGE.x == b.x && LAST_DAMAGE.y == b.y && LAST_DAMAGE.w == b.w && LAST_DAMAGE.h == b.h);
    CHECK(FILL_COLOR[1] == 0x010000); /* the bar: the accent for information */
    int in_x = CARD.x + 20, in_y = CARD.y + 20;
    CHECK(toast_pointer(in_x, in_y, 0) == 1);
    CHECK(toast_pointer(CARD.x - 5, in_y, 0) == 0 && toast_pointer(in_x, CARD.y - 5, 0) == 0);
    CHECK(toast_pointer(-1, -1, 0) == 0); /* the pointer left the window */

    /* the colour of the bar says what kind it is */
    toast_show(TOAST_SUCCESS, "Copied");
    draw();
    CHECK(FILL_COLOR[1] == 0x020000);
    toast_show(TOAST_WARNING, "Careful");
    draw();
    CHECK(FILL_COLOR[1] == 0x030000);
    toast_show(TOAST_ERROR, "Failed");
    draw();
    CHECK(FILL_COLOR[1] == 0x040000);
    toast_dismiss();

    /* time: information and success 3.5 s, warnings and errors 6 s; going away marks its area and asks for a redraw once */
    NOW = 10000;
    toast_show(TOAST_INFO, "Saved");
    DAMAGES = 0;
    NOW = 13499;
    CHECK(toast_tick() == 0 && toast_is_open() && DAMAGES == 0);
    NOW = 13500;
    CHECK(toast_tick() == 1 && !toast_is_open() && DAMAGES == 1);
    CHECK(toast_tick() == 0);
    NOW = 20000;
    toast_show(TOAST_ERROR, "Failed");
    NOW = 25999;
    CHECK(toast_tick() == 0 && toast_is_open());
    NOW = 26000;
    CHECK(toast_tick() == 1 && !toast_is_open());
    NOW = 30000;
    toast_show(TOAST_SUCCESS, "Done");
    NOW = 33500;
    CHECK(toast_tick() == 1);

    /* under the pointer it stays (at least 1.5 s more each time), and goes 1.5 s after the pointer has left */
    NOW = 40000;
    toast_show(TOAST_INFO, "Saved");
    draw();
    in_x = CARD.x + 20;
    in_y = CARD.y + 20;
    NOW = 43000;
    toast_pointer(in_x, in_y, 0);
    NOW = 43600; /* past its time (43500) */
    CHECK(toast_tick() == 0 && toast_is_open());
    NOW = 45000;
    CHECK(toast_tick() == 0 && toast_is_open()); /* still under the pointer: its time is pushed to 46500 */
    toast_pointer(1, 1, 0);                       /* the pointer leaves */
    NOW = 46499;
    CHECK(toast_tick() == 0 && toast_is_open());  /* it stays 1.5 s after the last tick that found the pointer on it */
    NOW = 46500;
    CHECK(toast_tick() == 1 && !toast_is_open());

    /* a click on it dismisses it and is not passed on; a click elsewhere, or a button already held when the pointer comes in, does not */
    NOW = 50000;
    toast_show(TOAST_WARNING, "Careful");
    draw();
    in_x = CARD.x + 20;
    in_y = CARD.y + 20;
    toast_pointer(500, 300, 0);
    CHECK(toast_pointer(500, 300, MOUSE_LEFT) == 0 && toast_is_open()); /* pressed elsewhere */
    CHECK(toast_pointer(in_x, in_y, MOUSE_LEFT) == 1 && toast_is_open()); /* the button was already down: dragged in, no click */
    toast_pointer(in_x, in_y, 0);
    DAMAGES = 0;
    CHECK(toast_pointer(in_x, in_y, MOUSE_LEFT) == 1 && !toast_is_open()); /* a click */
    CHECK(DAMAGES == 1);
    CHECK(toast_pointer(in_x, in_y, 0) == 0); /* the release finds nothing there */

    /* a new one replaces the one on the screen, and its time starts again */
    NOW = 60000;
    toast_show(TOAST_INFO, "First");
    NOW = 62000;
    DAMAGES = 0;
    toast_show(TOAST_INFO, "Second");
    CHECK(DAMAGES == 2); /* the old area and the new */
    draw();
    CHECK(TEXTS == 1 && strcmp(LINES[0], "Second") == 0);
    NOW = 65499;
    CHECK(toast_tick() == 0);
    NOW = 65500;
    CHECK(toast_tick() == 1);

    /* a text that does not fit in one line goes on two, split at a space */
    const char *long_text = "Icon places not saved - no drive can be written to. Unlock one now.";
    toast_show(TOAST_WARNING, long_text);
    draw();
    CHECK(TEXTS == 2);
    CHECK(CARD.w == 460 && CARD.h == 2 * 14 + 2 * 20);
    CHECK(LINES[0][strlen(LINES[0]) - 1] != ' ' && LINES[1][0] != ' ' && LINES[1][0] != 0);
    char joined[TOAST_TEXT_MAX + 2];
    strcpy(joined, LINES[0]);
    strcat(joined, " ");
    strcat(joined, LINES[1]);
    CHECK(strcmp(joined, long_text) == 0);
    CHECK((int)strlen(LINES[0]) * 8 <= 460 - 2 * 14 - 4 - 6); /* the first line fits */
    CHECK(CARD.y + CARD.h == 800 - TASKBAR_HEIGHT - 12);      /* a taller card still sits on the same line above the taskbar */
    toast_dismiss();

    /* a long text with no space to split at stays on one line (the draw shortens it) */
    char blob[100];
    memset(blob, 'x', sizeof blob - 1);
    blob[sizeof blob - 1] = 0;
    toast_show(TOAST_INFO, blob);
    draw();
    CHECK(TEXTS == 1 && CARD.w == 460 && CARD.h == 2 * 14 + 20);
    toast_dismiss();

    /* a text longer than the buffer is cut, not overrun */
    char huge[400];
    memset(huge, 'y', sizeof huge - 1);
    huge[sizeof huge - 1] = 0;
    for (int i = 9; i < (int)sizeof huge - 1; i += 10)
        huge[i] = ' ';
    toast_show(TOAST_ERROR, huge);
    draw();
    CHECK(toast_is_open() && TEXTS >= 1);
    CHECK(strlen(LINES[0]) + (TEXTS > 1 ? strlen(LINES[1]) + 1 : 0) <= TOAST_TEXT_MAX);
    toast_dismiss();

    /* the text is copied: the caller's buffer may change */
    char buf[16];
    strcpy(buf, "Kept");
    toast_show(TOAST_INFO, buf);
    strcpy(buf, "Changed");
    draw();
    CHECK(strcmp(LINES[0], "Kept") == 0);
    toast_dismiss();

    printf("toast tests passed\n");
    return 0;
}
