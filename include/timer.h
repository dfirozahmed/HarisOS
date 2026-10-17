/* ============================================================
 * HarisOS timer: milliseconds since boot, without interrupts.
 *
 *   timer_init();          once, early (takes about 35 ms: it measures the CPU clock)
 *   timer_ms();            time in milliseconds, keeps counting
 *
 * How: the PC's old 8254 timer (PIT, ports 0x40/0x43) ticks at a known speed
 * (1.193182 MHz). During timer_init() we count PIT ticks and CPU cycles (rdtsc)
 * for a moment, which tells us how many cycles are one millisecond. After that
 * timer_ms() only reads rdtsc, so it is fast and never wraps.
 *
 * If the PIT is missing, timer_ready() is 0 and timer_ms() returns 0.
 * ============================================================ */
#ifndef HARIS_TIMER_H
#define HARIS_TIMER_H

void timer_init(void);
int timer_ready(void);
unsigned long long timer_ms(void);

#endif
