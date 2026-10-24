/* ============================================================
 * HarisOS I/O APIC: how a device's interrupt wire becomes an interrupt of a processor.
 *
 *   ioapic_init();                        once, after acpi_init(): find the chips (MADT, else the usual address) and mask every line
 *   ioapic_present();                     1 if there is at least one
 *   ioapic_gsi_count();                   the lines are numbered 0 .. count-1 ("global system interrupts", GSI)
 *   ioapic_route(gsi, vector, apic_id);   line gsi now interrupts the processor with that local APIC id, with that vector
 *   ioapic_mask(gsi);                     the line is silent again
 *
 * A PCI device raises its INTA..INTD wire; the board connects the wires to I/O APIC lines in a way only the firmware tables
 * (ACPI _PRT) describe. This code does not read them: a driver that wants an interrupt finds the line by trying (see
 * e1000.c, net_irq_init). PCI wires are level triggered and active low, which is what ioapic_route sets up: the line stays
 * asserted until the device is told to let go, so the handler must clear the device's cause register before the EOI.
 * ============================================================ */
#ifndef HARIS_IOAPIC_H
#define HARIS_IOAPIC_H

#define IOAPIC_DEFAULT_ADDR 0xFEC00000u /* where the first one is, when the MADT does not say */

/* The low 32 bits of a redirection entry (host-tested): vector in bits 0..7, delivery mode "fixed" (0), physical destination (0),
 * polarity bit 13 (1 = active low), trigger bit 15 (1 = level), mask bit 16. */
unsigned ioapic_entry(unsigned vector, int level, int active_low, int masked);

void ioapic_init(void);
int ioapic_present(void);
unsigned ioapic_gsi_count(void);
int ioapic_route(unsigned gsi, unsigned vector, unsigned apic_id);
void ioapic_mask(unsigned gsi);

#endif
