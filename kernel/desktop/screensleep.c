/* Screen off after a time without input: see include/screensleep.h. */
#include "screensleep.h"
#include "gfx.h"
#include "look.h"
#include "timer.h"

static int ASLEEP;
static unsigned long long LAST_MS; /* time of the last input */

int screensleep_available(void) { return gfx_screen_sleep_supported(); }
int screensleep_asleep(void) { return ASLEEP; }

int screensleep_activity(void)
{
    LAST_MS = timer_ms();
    if (!ASLEEP)
        return 0;
    ASLEEP = 0;
    (void)gfx_screen_sleep(0);
    return 1;
}

void screensleep_tick(void)
{
    int sec = look_screenoff();
    if (!LAST_MS)
        LAST_MS = timer_ms() ? timer_ms() : 1; /* the time counts from the first look, not from power-on */
    if (!LAST_MS)
        LAST_MS = timer_ms() ? timer_ms() : 1; /* the time counts from the first look, not from power-on */
    if (ASLEEP || sec <= 0 || !gfx_screen_sleep_supported())
        return;
    if (timer_ms() - LAST_MS >= (unsigned long long)sec * 1000ull)
    {
        if (gfx_screen_sleep(1) == 0)
            ASLEEP = 1;
    }
}
