/* HarisOS boot splash: see include/bootsplash.h. Same look as the shut down scene (desktop_power.c).
 * Only the strip with the bar and the status line is redrawn on a progress step: the full scene
 * (gradient, logo, heading) is drawn once, so a step costs almost nothing. */
#include "bootsplash.h"
#include "ui.h"

#define BG_TOP_R 8
#define BG_TOP_G 14
#define BG_TOP_B 32
#define BG_BOT_R 22
#define BG_BOT_G 36
#define BG_BOT_B 74
#define STATUS_MAX 64

static UiRgb WHITE = {240, 245, 255};
static UiRgb SOFT = {170, 184, 214};

static int SHOWN;
static int PERCENT;
static char STATUS[STATUS_MAX];

/* background colour of the vertical gradient at row y */
static unsigned char grad(int top, int bottom, int y, int H)
{
    return (unsigned char)(top + (bottom - top) * y / (H > 0 ? H : 1));
}

static int bar_width(int W)
{
    int w = W - 80;
    return w > 360 ? 360 : (w < 80 ? 80 : w);
}

/* the strip under the heading: bar and status line */
static void draw_strip(void)
{
    int W = (int)gfx_width(), H = (int)gfx_height();
    int y0 = H / 2 + 36, y1 = y0 + 84;
    gfx_reset_clip();
    gfx_gradient_v(0, y0, W, y1 - y0, grad(BG_TOP_R, BG_BOT_R, y0, H), grad(BG_TOP_G, BG_BOT_G, y0, H), grad(BG_TOP_B, BG_BOT_B, y0, H),
                   grad(BG_TOP_R, BG_BOT_R, y1, H), grad(BG_TOP_G, BG_BOT_G, y1, H), grad(BG_TOP_B, BG_BOT_B, y1, H));
    int bw = bar_width(W);
    ui_progress(ui_rect(W / 2 - bw / 2, y0 + 14, bw, 8), PERCENT);
    ui_text_in_color(ui_rect(0, y0 + 36, W, 24), STATUS, UI_TEXT_BODY, UI_ALIGN_CENTER, SOFT);
}

void splash_show(void)
{
    if (!gfx_ready())
        return;
    int W = (int)gfx_width(), H = (int)gfx_height();
    gfx_cursor_show(0);
    gfx_reset_clip();
    gfx_gradient_v(0, 0, W, H, BG_TOP_R, BG_TOP_G, BG_TOP_B, BG_BOT_R, BG_BOT_G, BG_BOT_B);
    ui_icon(UI_ICON_LOGO, W / 2, H / 2 - 90, 96, WHITE);
    ui_text_in_color(ui_rect(0, H / 2 - 28, W, 40), "Welcome to HarisOS", UI_TEXT_HEADING, UI_ALIGN_CENTER, WHITE);
    SHOWN = 1;
    draw_strip();
    gfx_present();
}

void splash_progress(int percent, const char *status)
{
    if (!gfx_ready())
        return;
    PERCENT = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    int i = 0;
    for (; status && status[i] && i < STATUS_MAX - 1; i++)
        STATUS[i] = status[i];
    STATUS[i] = 0;
    if (!SHOWN)
    {
        splash_show();
        return;
    }
    draw_strip();
    gfx_present();
}
