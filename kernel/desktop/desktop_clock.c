/* Taskbar clock polling and caret blink updates. */
#include "desktop.h"
#include "guiapp.h"
#include "taskbar.h"
#include "startmenu.h"
#include "ui.h"
#include "timer.h"
#include "wm.h"
#include "rtc.h"
#include "desktop_internal.h"

/* Refresh the CMOS clock periodically. The displayed clock has minute
 * precision, so reading RTC once per second is enough; redraw only when a
 * displayed field changes. A loop-count fallback keeps clock refresh working
 * on machines where PIT/TSC calibration is unavailable. */
static int refresh_taskbar_clock(void)
{
    static unsigned long long next_poll_ms;
    static unsigned fallback_polls;
    static int clock_seen;
    static int last_hour = -1, last_minute = -1;
    static int last_day = -1, last_month = -1, last_year = -1;

    if (timer_ready())
    {
        unsigned long long now = timer_ms();
        if (now < next_poll_ms)
            return 0;
        next_poll_ms = now + 1000;
    }
    else
    {
        /* Avoid CMOS port I/O on every tight-loop iteration. */
        if (++fallback_polls < 65536u)
            return 0;
        fallback_polls = 0;
    }

    RtcTime now;
    if (rtc_read(&now) != 0)
        return 0; /* keep the last valid time if the RTC is unavailable */

    int changed = !clock_seen || now.hour != last_hour || now.minute != last_minute ||
                  now.day != last_day || now.month != last_month || now.year != last_year;
    clock_seen = 1;
    last_hour = now.hour;
    last_minute = now.minute;
    last_day = now.day;
    last_month = now.month;
    last_year = now.year;
    if (changed)
        taskbar_set_time(now.hour, now.minute, now.day, now.month, now.year);
    return changed;
}

/* Called on every main-loop pass: updates the taskbar clock and makes the text
 * caret blink. Only the taskbar or small text field is redrawn when needed. */
int desktop_tick(void)
{
    int clock_changed = refresh_taskbar_clock();
    int flipped = ui_caret_update();
    int ticked = gui_app_poll(); /* windows of user programs that appeared or went away */
    ticked |= wm_tick();        /* windows that wait for time (tooltips) */
    Window *top = wm_top();
    startmenu_tick(flipped);

    if (clock_changed)
    {
        int bx, by, bw, bh;
        taskbar_bounds(&bx, &by, &bw, &bh);
        wm_add_screen_damage(ui_rect(bx, by, bw, bh));
    }
    if (flipped && top && top->caret && top->edit_count > 0)
    {
        UiRect f = top->edits[top->focus].field;
        if (f.w > 0)
            wm_add_screen_damage(f);
    }
    if (!clock_changed && !flipped && !ticked)
        return 0;
    return desktop_finish_input(clock_changed);
}
