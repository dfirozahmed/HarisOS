/* HarisOS desktop: drawn entirely through the gfx layer (include/gfx.h). */
#include "desktop.h"
#include "gfx.h"
#include "taskbar.h"

static const char *STATUS_LINE;

void desktop_set_status(const char *line) { STATUS_LINE = line; }

static void desktop(void)
{
    int W = (int)gfx_width(), H = (int)gfx_height() - TASKBAR_HEIGHT; /* area above the taskbar */
    int full_h = (int)gfx_height();
    GfxColor white = gfx_rgb(245, 249, 255), blue = gfx_rgb(44, 120, 230);

    gfx_gradient_v(0, 0, W, full_h, 12, 45, 86, 40, 103, 162);

    /* soft abstract wallpaper waves */
    gfx_fill_rect(W * 56 / 100, H * 16 / 100, W * 34 / 100, H * 48 / 100, gfx_rgb(29, 104, 169));
    gfx_fill_rect(W * 62 / 100, H * 22 / 100, W * 25 / 100, H * 36 / 100, gfx_rgb(34, 126, 185));
    gfx_fill_rect(W * 68 / 100, H * 28 / 100, W * 15 / 100, H * 24 / 100, gfx_rgb(42, 148, 199));

    /* a sample welcome window */
    int ww = W * 48 / 100, wh = H * 43 / 100;
    if (ww < 360)
        ww = 360;
    if (wh < 240)
        wh = 240;
    int wx = (W - ww) / 2, wy = (H - wh) / 2 - 8;

    gfx_fill_rect_alpha(wx + 7, wy + 8, ww, wh, 0, 0, 0, 110); /* soft shadow */
    gfx_fill_rect(wx, wy, ww, wh, gfx_rgb(239, 244, 250));
    gfx_fill_rect(wx, wy, ww, 34, blue);
    gfx_draw_text(wx + 14, wy + 12, "HARISOS DESKTOP", white, 1);
    gfx_fill_circle(wx + ww - 18, wy + 17, 8, gfx_rgb(221, 75, 75)); /* close button */

    /* content area: clipped so nothing can spill out of the window */
    gfx_set_clip(wx, wy + 34, ww, wh - 34);
    gfx_draw_text(wx + 24, wy + 62, "WELCOME TO HARISOS", gfx_rgb(30, 54, 80), 2);
    gfx_draw_text(wx + 24, wy + 104, "UEFI GRAPHICS DESKTOP", gfx_rgb(48, 75, 104), 1);
    gfx_hline(wx + 24, wy + 134, ww - 48, gfx_rgb(190, 202, 216));
    gfx_draw_text(wx + 24, wy + 151, "YOUR DESKTOP IS READY", gfx_rgb(36, 113, 90), 1);
    if (STATUS_LINE)
        gfx_draw_text(wx + 24, wy + 171, STATUS_LINE, gfx_rgb(48, 75, 104), 1);
    gfx_fill_round_rect(wx + 24, wy + wh - 48, 118, 28, 6, blue);
    gfx_draw_text(wx + 39, wy + wh - 38, "START", white, 1);
    gfx_reset_clip();

    taskbar_draw();
}

void desktop_draw(void)
{
    if (gfx_ready())
        desktop();
}
