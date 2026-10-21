/* ============================================================
 * HarisOS CPU load: how busy the main loop is.
 *
 * HarisOS has no scheduler or interrupts: the kernel is one polling loop (kernel.c). A pass of that
 * loop is "busy" when it handled something (a mouse or key event, a redraw) and "idle" when it only
 * polled the hardware. The loop measures the busy time of each pass and reports it here:
 *
 *   cpuload_add_busy(us);   after a pass that did work: how long it took
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
