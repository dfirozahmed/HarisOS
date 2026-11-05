/* System calls: clock and wall time. */
#include "syscall_internal.h"
#include "task.h"

/* SYS_CLOCK: no arguments. The milliseconds since the kernel started (timer_ms), 0 if the timer is not running. */
long long sys_clock(const SyscallFrame *f)
{
    (void)f;
    return (long long)timer_ms();
}

/* SYS_SLEEP: rdi = milliseconds (1 .. 5000). The thread sleeps; the others run. A GUI program that redraws a clock or a progress bar
 * between events has no other way to wait for a while. */
long long sys_sleep(const SyscallFrame *f)
{
    unsigned long long ms = f->rdi;
    task_sleep_ms((unsigned)(ms < 1 ? 1 : ms > 5000 ? 5000 : ms));
    return 0;
}

/* SYS_TIME: no arguments. The wall clock as Unix time (seconds since 1970-01-01), taken from the real-time clock, which is read as UTC.
 * -1 if there is no clock. The calendar date becomes a day count with the usual "days from civil" arithmetic (years from March, so the
 * leap day is the last day of a year). */
long long sys_time(const SyscallFrame *f)
{
    (void)f;
    RtcTime t;
    if (rtc_read(&t) != 0 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.year < 1970)
        return -1;
    long long y = t.year - (t.month <= 2);
    long long era = y / 400;
    long long yoe = y - era * 400;
    long long mp = (t.month + 9) % 12; /* March = 0 */
    long long doy = (153 * mp + 2) / 5 + t.day - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return days * 86400 + t.hour * 3600LL + t.minute * 60LL + t.second;
}
