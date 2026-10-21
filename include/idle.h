/* ============================================================
 * HarisOS idle: sleep the CPU between polling passes, like an OS "idle thread".
 *
 *   idle_init();    once, after timer_init(): own GDT + IDT, LAPIC timer ticking every 1 ms
 *   idle_wait();    stop the CPU (hlt) until the next interrupt, at most about 1 ms
 *
 * Why: without it the main loop spins at 100% of a host core even when nothing happens.
 * The CPU wakes on every 1 ms LAPIC timer tick, the loop polls the devices once more and
 * sleeps again, so mouse / USB / I2C drivers stay polled and need no interrupt of their own.
 *
 * If the local APIC cannot be used (missing, or already in x2APIC mode) idle_init() returns -1
 * and idle_wait() only does "pause": everything works as before, just without the sleep.
 * ============================================================ */
#ifndef HARIS_IDLE_H
#define HARIS_IDLE_H

int idle_init(void);
int idle_ready(void);
void idle_wait(void);

#endif
