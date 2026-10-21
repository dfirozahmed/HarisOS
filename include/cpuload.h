/* ============================================================
 * HarisOS CPU load: how busy the main loop is.
 *
 * HarisOS has no scheduler: the kernel is one polling loop (kernel.c). Like an OS idle thread, the
 * loop halts the CPU (idle.h) when a pass found nothing to do. Busy time is everything except that
 * halt: the loop measures the length of each pass (polling and redraws) and reports it here:
 *
 *   cpuload_add_busy(us);   after every pass: how long it took (the halt is not part of a pass)
 *   cpuload_poll(now_us);   on every pass: closes a 1-second window when it is over
 *   cpuload_percent();      busy share of the last finished window, 0..100
 *
 * The time is passed in (timer_us()), so the module has no hardware access and is tested on the host.
 * ============================================================ */
#ifndef HARIS_CPULOAD_H
#define HARIS_CPULOAD_H

#define CPULOAD_WINDOW_US 1000000ULL

void cpuload_add_busy(unsigned long long us);
void cpuload_poll(unsigned long long now_us);
int cpuload_percent(void);

#endif
