/* HarisOS CPU load: see include/cpuload.h. */
#include "cpuload.h"

static unsigned long long WIN_START, BUSY_US;
static int STARTED, PERCENT;

void cpuload_add_busy(unsigned long long us)
{
    BUSY_US += us;
}

void cpuload_poll(unsigned long long now_us)
{
    if (!STARTED)
    {
        STARTED = 1;
        WIN_START = now_us;
        BUSY_US = 0;
        return;
    }
    if (now_us < WIN_START || now_us - WIN_START < CPULOAD_WINDOW_US)
        return;
    unsigned long long len = now_us - WIN_START;
    unsigned long long p = BUSY_US * 100 / len;
    PERCENT = p > 100 ? 100 : (int)p;
    WIN_START = now_us;
    BUSY_US = 0;
}

int cpuload_percent(void)
{
    return PERCENT;
}
