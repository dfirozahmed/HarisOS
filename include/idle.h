/* ============================================================
 * HarisOS idle: sleep the CPU between polling passes, like an OS "idle thread".
 *
 *   idle_init();    once, after timer_init(): own GDT + IDT, LAPIC timer ticking every 1 ms
 *   idle_wait();    stop the CPU (hlt) until the next interrupt, at most about 1 ms
 *
 * Why: without it the main loop spins at 100% of a host core even when nothing happens.
 * The CPU wakes on every 1 ms LAPIC timer tick, the loop polls the devices once more and
 * sleeps again, so mouse / USB / I2C drivers stay polled and need no interrupt of their own. The network card is the one
 * exception: its receive interrupt (vector IDLE_VEC_NET, routed by ioapic.h) also ends the sleep, see e1000.c.
 *
 * The tick also calls task_tick() (task.h), which is how a preemptible kernel thread loses the CPU.
 *
 * If the local APIC cannot be used (missing, or already in x2APIC mode) idle_init() returns -1
 * and idle_wait() only does "pause": everything works as before, just without the sleep.
 * ============================================================ */
#ifndef HARIS_IDLE_H
#define HARIS_IDLE_H

int idle_init(void);
int idle_ready(void);
unsigned idle_lapic_id(void); /* the id of this processor's local APIC: where an I/O APIC line is sent */
#define IDLE_VEC_NET 0x41 /* the interrupt vector of the network card (e1000.c); 0x40 is the timer */
#define IDLE_VEC_TIMER_AP 0x42 /* the timer of the application processors (smp.h): a handler of its own, it only counts */
void idle_wait(void);
/* SMP (smp.h): send an inter-processor interrupt, `icr_low` = the low half of the interrupt command register (delivery mode, level ...)
 * to the processor with that local APIC id. 0, or -1 when the APIC is unusable or does not finish sending. */
int idle_send_ipi(unsigned apic_id, unsigned icr_low);
/* On an application processor, right after its GDT and GS base are set: the shared IDT, local APIC enabled, its own 1 ms timer
 * (vector IDLE_VEC_TIMER_AP). Interrupts stay off until the caller runs sti. */
void idle_ap_init(void);
void idle_send_self_nmi(void); /* raise a non-maskable interrupt on this CPU (a self test of the NMI stack) */

#endif
