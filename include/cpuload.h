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

/* Per processor (SMP, smp.h). The boot processor's load is the main loop's busy share above. An application processor has no loop to measure:
 * its timer tick counts, in task.c, how many ticks it took (all) and how many of them found it running a user thread (busy). The totals
 * only grow; this module turns them into the share of the last window of CPULOAD_CPU_WINDOW_TICKS ticks (about a second).
 *
 *   cpuload_cpu_poll(cpu, busy_total, all_total);   now and then (the main loop does it every pass)
 *   cpuload_cpu_percent(cpu);                        0..100 of the last finished window, 0 before the first */
#define CPULOAD_MAX_CPUS 16
#define CPULOAD_CPU_WINDOW_TICKS 1000ULL
void cpuload_cpu_poll(int cpu, unsigned long long busy_total, unsigned long long all_total);
int cpuload_cpu_percent(int cpu);
/* The boot processor's whole load: the main loop's busy share (above) plus the share of its ticks that found it running some other thread
 * (a user process, a kernel thread: cpuload_cpu_poll(0, ...) with the busy ticks of processor 0), at most 100. */
int cpuload_boot_percent(void);

void cpuload_add_busy(unsigned long long us);
void cpuload_poll(unsigned long long now_us);
int cpuload_percent(void);

#endif
