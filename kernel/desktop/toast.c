/* HarisOS desktop toast: see include/toast.h. */
#include "toast.h"
#include "mouse.h"
#include "taskbar.h"
#include "timer.h"
#include "wm.h"

#define MAX_W 460         /* the widest it gets; a longer text goes on a second line */
#define MIN_W 200
#define PAD 14
#define BAR 4             /* the coloured bar at its left */
#define LINE_H 20
#define EDGE 16           /* from the right edge of the screen */
#define GAP 12            /* above the taskbar */
#define SHORT_MS 3500     /* information and success */
#define LONG_MS 6000      /* warnings and errors: there is more to read, and it matters more */
#define HOVER_MS 1500     /* a toast under the pointer stays at least this much longer */

static char TEXT[TOAST_TEXT_MAX + 1];
static char LINE[2][TOAST_TEXT_MAX + 1]; /* the text as one or two lines */
static int NLINES;
static ToastKind KIND;
static int OPEN;
static int WIDTH, HEIGHT;
static unsigned long long EXPIRE;
static int HOVER;
static unsigned LAST_BTN;

static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

/* the text on one or two lines, and how big the toast is */
static void layout(void)
{
    int inner = MAX_W - 2 * PAD - BAR - 6; /* the text's room */
    int tw = ui_text_width(TEXT, UI_TEXT_BODY);
    int n = slen(TEXT);
    NLINES = 1;
    for (int i = 0; i <= n; i++)
        LINE[0][i] = TEXT[i];
    LINE[1][0] = 0;
    if (tw > inner)
    {
        int cut = -1; /* the last space after which the first line still fits */
        char probe[TOAST_TEXT_MAX + 1];
        for (int k = 0; k < n; k++)
        {
            if (TEXT[k] != ' ')
                continue;
            for (int j = 0; j < k; j++)
                probe[j] = TEXT[j];
            probe[k] = 0;
            if (ui_text_width(probe, UI_TEXT_BODY) <= inner)
                cut = k;
        }
        if (cut > 0)
        {
            LINE[0][cut] = 0;
            for (int j = cut + 1; j <= n; j++)
                LINE[1][j - cut - 1] = TEXT[j];
            NLINES = 2;
        }
        /* no space that fits: one line, shortened with "..." when it is drawn */
        tw = inner;
    }
    WIDTH = tw + 2 * PAD + BAR + 6;
    if (NLINES == 2)
        WIDTH = MAX_W;
    if (WIDTH < MIN_W)
        WIDTH = MIN_W;
    HEIGHT = 2 * PAD + NLINES * LINE_H;
}

static UiRect rect(void)
{
    int W = (int)gfx_width(), H = (int)gfx_height();
    return ui_rect(W - WIDTH - EDGE, H - TASKBAR_HEIGHT - HEIGHT - GAP, WIDTH, HEIGHT);
}

int toast_is_open(void) { return OPEN; }

UiRect toast_bounds(void)
{
    UiRect r = rect();
    return ui_rect(r.x - 16, r.y - 16, r.w + 32, r.h + 16 + 24); /* the shadow: blur 14, moved 5 down */
}

static void damage(void)
{
    wm_add_screen_damage(toast_bounds());
}

void toast_dismiss(void)
{
    if (!OPEN)
        return;
    damage();
    OPEN = 0;
    HOVER = 0;
}

void toast_show(ToastKind kind, const char *text)
{
    if (!text || !text[0])
        return;
    if (OPEN)
        damage(); /* the old one's area (it may have another size) */
    int n = 0;
    while (text[n] && n < TOAST_TEXT_MAX)
    {
        TEXT[n] = text[n];
        n++;
    }
    TEXT[n] = 0;
    KIND = kind;
    layout();
    OPEN = 1;
    HOVER = 0;
    EXPIRE = timer_ms() + (kind == TOAST_WARNING || kind == TOAST_ERROR ? LONG_MS : SHORT_MS);
    damage();
}

int toast_tick(void)
{
    if (!OPEN)
        return 0;
    unsigned long long now = timer_ms();
    if (HOVER && EXPIRE < now + HOVER_MS) /* being read: it stays */
        EXPIRE = now + HOVER_MS;
    if (now < EXPIRE)
        return 0;
    toast_dismiss();
    return 1;
}

int toast_pointer(int x, int y, unsigned buttons)
{
    unsigned now = buttons & MOUSE_LEFT;
    int pressed = now && !(LAST_BTN & MOUSE_LEFT);
    LAST_BTN = now;
    if (!OPEN)
        return 0;
    int over = x >= 0 && ui_contains(rect(), x, y);
    HOVER = over;
    if (over && pressed)
        toast_dismiss(); /* the click is the toast's: it is not passed on */
    return over;
}

void toast_draw(void)
{
    if (!OPEN || !gfx_ready())
        return;
    const UiTheme *t = ui_theme();
    UiRect r = rect();
    UiRgb c = KIND == TOAST_SUCCESS ? t->success : KIND == TOAST_WARNING ? t->warning : KIND == TOAST_ERROR ? t->danger : t->accent_hi;
    gfx_shadow(r.x, r.y, r.w, r.h, t->radius_m, 14, 0, 5, 120);
    gfx_fill_round_rect(r.x, r.y, r.w, r.h, t->radius_m, ui_color(t->surface));
    gfx_round_rect(r.x, r.y, r.w, r.h, t->radius_m, 1, ui_color(t->border));
    gfx_fill_round_rect(r.x + 7, r.y + 9, BAR, r.h - 18, BAR / 2, ui_color(c));
    int tx = r.x + PAD + BAR + 6;
    for (int i = 0; i < NLINES; i++)
        ui_text_in_color(ui_rect(tx, r.y + PAD + i * LINE_H, r.w - (tx - r.x) - PAD, LINE_H), LINE[i], UI_TEXT_BODY, UI_ALIGN_LEFT, t->text);
}
