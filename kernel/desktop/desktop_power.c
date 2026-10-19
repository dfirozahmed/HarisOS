/* HarisOS: shut down / restart with a full-screen "Shutting down..." scene. */
#include "desktop.h"
#include "power.h"
#include "timer.h"
#include "ui.h"
#include "desktop_internal.h"

static UiRgb WHITE = {240, 245, 255};
static UiRgb SOFT = {170, 184, 214};

/* spinner: eight dots on a circle, the "head" dot is solid and the ones behind it fade out */
static void scene(const char *line, int head, const char *hint)
{
    int W = (int)gfx_width(), H = (int)gfx_height();
    gfx_cursor_show(0);
    gfx_reset_clip();
    gfx_gradient_v(0, 0, W, H, 8, 14, 32, 22, 36, 74);
    int cx = W / 2, cy = H / 2 - 28;
    if (head >= 0)
    {
        static const short DX[8] = {0, 71, 100, 71, 0, -71, -100, -71}; /* 100 * sin(45deg * k) */
        static const short DY[8] = {-100, -71, 0, 71, 100, 71, 0, -71};
        for (int k = 0; k < 8; k++)
        {
            int behind = (head - k + 8) % 8; /* 0 = head */
            int alpha = 255 - behind * 30;
            gfx_fill_circle_q((cx + DX[k] * 20 / 100) * 16, (cy + DY[k] * 20 / 100) * 16, 3 * 16 + 8,
                              gfx_rgb(WHITE.r, WHITE.g, WHITE.b), (unsigned char)alpha);
        }
    }
    ui_text_in_color(ui_rect(0, cy + 44, W, 40), line, UI_TEXT_HEADING, UI_ALIGN_CENTER, WHITE);
    if (hint)
        ui_text_in_color(ui_rect(0, cy + 88, W, 24), hint, UI_TEXT_BODY, UI_ALIGN_CENTER, SOFT);
    gfx_present();
}

static void wait_ms(unsigned long long ms)
{
    if (timer_ready())
    {
        unsigned long long t0 = timer_ms();
        while (timer_ms() - t0 < ms)
        {
        }
    }
    else
        for (volatile unsigned long i = 0; i < ms * 20000UL; i++)
        {
        }
}

static void halt_forever(void)
{
    for (;;)
    {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("cli; hlt");
#endif
    }
}

void desktop_power_action(int restart)
{
    const char *line = restart ? "Restarting..." : "Shutting down...";
    for (int frame = 0; frame < 8; frame++) /* about 0.7 s: long enough to see, short enough not to annoy */
    {
        scene(line, frame % 8, 0);
        wait_ms(90);
    }

    if (restart)
        power_restart();
    else
        power_shutdown();

    /* Still here: this computer would not switch off / restart by itself. */
    if (restart)
        scene("Could not restart.", -1, "Press the reset button on the computer.");
    else
        scene("It is now safe to turn off your computer.", -1, "HarisOS could not power it off by itself.");
    halt_forever();
}
