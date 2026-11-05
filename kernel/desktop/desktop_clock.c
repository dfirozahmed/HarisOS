/* Taskbar clock polling and caret blink updates. */
#include "audio.h"
#include "battery.h"
#include "net.h"
#include "desktop.h"
#include "guiapp.h"
#include "taskbar.h"
#include "volmenu.h"
#include "appmenu.h"
#include "deskmenu.h"
#include "toast.h"
#include "deskfiles.h"
#include "startmenu.h"
#include "ui.h"
#include "timer.h"
#include "cpuload.h"
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

/* The Wi-Fi state for the tray icon, asked twice a second (the driver keeps it up to date; this only copies it). */
static int refresh_wifi(void)
{
    static unsigned long long next_ms;
    static unsigned fallback;
    if (timer_ready())
    {
        unsigned long long now = timer_ms();
        if (now < next_ms)
            return 0;
        next_ms = now + 500;
    }
    else if (++fallback < 4096u)
        return 0;
    else
        fallback = 0;
    NetWifiStatus st;
    net_wifi_status(&st);
    return taskbar_set_wifi(!st.present ? -1 : st.state);
}

/* The battery for the tray icon. The mains cable is looked at 5 times a second (one byte from the embedded controller), so that plugging it in or
 * out shows at once, and every change of it reads everything again straight away; otherwise the whole reading (a handful of bytes) is once a second.
 * A machine whose controller layout is not known has no level: the icon stays plain. */
static int refresh_battery(void)
{
    static unsigned long long next_ac_ms, next_full_ms;
    static unsigned fallback;
    static int last_ac = -2;
    if (!battery_supported())
        return 0;
    int full = 0;
    if (timer_ready())
    {
        unsigned long long now = timer_ms();
        if (now < next_ac_ms)
            return 0;
        next_ac_ms = now + 200;
        int ac = battery_ac();
        if (ac != last_ac || now >= next_full_ms)
            full = 1;
        last_ac = ac;
        if (full)
            next_full_ms = now + 1000;
    }
    else if (++fallback < 4096u)
        return 0;
    else
    {
        fallback = 0;
        full = 1;
    }
    if (!full)
        return 0;
    BatteryInfo b;
    if (battery_read(&b) != 0)
        return taskbar_set_battery(-1, 0, 0);
    if (!b.present) /* no battery in the bay: the cable may still be plugged in */
        return taskbar_set_battery(-1, 0, b.ac);
    return taskbar_set_battery(b.percent, b.charging, b.ac);
}

/* Called on every main-loop pass: updates the taskbar clock and makes the text
 * caret blink. Only the taskbar or small text field is redrawn when needed. */
int desktop_tick(void)
{
    unsigned long long now_ms = timer_ms();
    /* each part timed on its own (cpuload.h sections 12 ..): where the pass's time goes when nothing happens */
    cpuload_section_begin();
    int clock_changed = refresh_taskbar_clock();
    clock_changed |= taskbar_set_volume(audio_volume());
    cpuload_section_end(12);
    cpuload_section_begin();
    clock_changed |= refresh_wifi();
    cpuload_section_end(13);
    cpuload_section_begin();
    clock_changed |= refresh_battery();
    cpuload_section_end(14);
    cpuload_section_begin();
    int flipped = ui_caret_update();
    cpuload_section_end(15);
    cpuload_section_begin();
    int ticked = volmenu_tick(); /* the volume may have been changed by a program (SYS_AUDIO_VOLUME); the panel slides open */
    cpuload_section_end(16);
    cpuload_section_begin();
    ticked |= gui_app_poll(); /* windows of user programs that appeared or went away */
    cpuload_section_end(17);
    cpuload_section_begin();
    ticked |= wm_tick();        /* windows that wait for time (tooltips) */
    ticked |= toast_tick();     /* a toast whose time is up */
    cpuload_section_end(18);
    cpuload_section_begin();
    ticked |= deskfiles_tick(); /* the Desktop folder has other files than a second ago */
    cpuload_section_end(19);
    Window *top = wm_top();
    cpuload_section_begin();
    ticked |= startmenu_tick(flipped, now_ms);
    ticked |= appmenu_tick(now_ms);
    ticked |= deskmenu_tick(now_ms);
    cpuload_section_end(20);
    cpuload_section_begin();
    ticked |= wm_comp_anim_tick(now_ms); /* a window opening / closing / minimizing */
    ticked |= desktop_comp_anim_tick(now_ms); /* the start menu sliding up */
    UiRect glow;
    if (taskbar_tick(now_ms, &glow)) /* a taskbar button's hover tint moved one step */
    {
        wm_add_screen_damage(glow);
        ticked = 1;
    }
    cpuload_section_end(21);

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
    cpuload_section_begin();
    int shown = desktop_finish_input(clock_changed);
    cpuload_section_end(22); /* the frame, when there is one (the compose too) */
    return shown;
}
