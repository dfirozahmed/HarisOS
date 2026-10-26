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

/* ---- per processor ---- */
static unsigned long long CPU_BUSY0[CPULOAD_MAX_CPUS], CPU_ALL0[CPULOAD_MAX_CPUS]; /* the totals at the start of the window */
static int CPU_STARTED[CPULOAD_MAX_CPUS], CPU_PERCENT[CPULOAD_MAX_CPUS];

void cpuload_cpu_poll(int cpu, unsigned long long busy_total, unsigned long long all_total)
{
    if (cpu < 0 || cpu >= CPULOAD_MAX_CPUS)
        return;
    if (!CPU_STARTED[cpu] || all_total < CPU_ALL0[cpu] || busy_total < CPU_BUSY0[cpu])
    {
        CPU_STARTED[cpu] = 1; /* the first call, or the counters went backwards: start over */
        CPU_BUSY0[cpu] = busy_total;
        CPU_ALL0[cpu] = all_total;
        return;
    }
    unsigned long long all = all_total - CPU_ALL0[cpu];
    if (all < CPULOAD_CPU_WINDOW_TICKS)
        return;
    unsigned long long p = (busy_total - CPU_BUSY0[cpu]) * 100 / all;
    CPU_PERCENT[cpu] = p > 100 ? 100 : (int)p;
    CPU_BUSY0[cpu] = busy_total;
    CPU_ALL0[cpu] = all_total;
}

int cpuload_cpu_percent(int cpu)
{
    return cpu >= 0 && cpu < CPULOAD_MAX_CPUS ? CPU_PERCENT[cpu] : 0;
}

int cpuload_boot_percent(void)
{
    int p = PERCENT + cpuload_cpu_percent(0);
    return p > 100 ? 100 : p;
}
