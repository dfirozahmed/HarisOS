/* ============================================================
 * ACPI tables, only as far as the drivers need them: the AML byte code of the DSDT and all
 * SSDTs (that is where the firmware describes devices such as I2C touchpads).
 *
 *   acpi_init(rsdp)       once, with BootInfo.acpi_rsdp
 *   acpi_aml_count()      how many AML tables were found
 *   acpi_aml(i, &len)     table i (header included) and its length
 *
 * ACPI memory is identity mapped under UEFI, so physical addresses are used as pointers.
 * ============================================================ */
#ifndef HARIS_ACPI_H
#define HARIS_ACPI_H

void acpi_init(unsigned long long rsdp);

/* Logical processors the firmware says are enabled (MADT, the "APIC" table), with their APIC ids: the ids encode
 * which core and which socket each one is in (see cpuinfo.h). 0 when there is no MADT. */
#define ACPI_MAX_CPUS 256
int acpi_cpu_count(void);
unsigned acpi_cpu_apic_id(int i);
/* I/O APICs (MADT type 1): the chips that turn device interrupt lines into interrupts of a processor (see ioapic.h). Each serves
 * a run of "global system interrupt" numbers starting at its GSI base. 0 when there is no MADT. */
#define ACPI_MAX_IOAPICS 8
int acpi_ioapic_count(void);
unsigned acpi_ioapic_addr(int i);     /* MMIO address of I/O APIC i, 0 if there is none */
unsigned acpi_ioapic_gsi_base(int i);
int acpi_aml_count(void);
const unsigned char *acpi_aml(int i, unsigned *len);

#endif
