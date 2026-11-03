/* ============================================================
 * HarisOS battery: charge level and charging state for the taskbar
 *
 *   battery_init();          once, after acpi_init(): finds out whether this machine is one whose embedded controller layout is known
 *   battery_supported();     1 if battery_read() can say something
 *   battery_read(&b);        0 = b is filled in, -1 = not supported / no battery / the controller did not answer
 *
 * How: a laptop's battery is read by the embedded controller (EC), a small chip on the board. The firmware's ACPI byte code (the DSDT)
 * describes where in the EC's 256 bytes the numbers live (an "OperationRegion" of the type EmbeddedControl and a "Field" that gives names to
 * the bytes). Windows and Linux run that byte code. This kernel has no interpreter for it, so the offsets of ONE machine were read out of its
 * DSDT by hand and are used here, only when the DSDT says it belongs to that machine (HPQOEM / 8707: the HP Laptop 15s-eq1xxx):
 *
 *   0x40 bit 0  SW2S  the AC adapter is plugged in (the _PSR method of the AC device returns it)
 *   0xA4 bit 0  MBTS  a battery is in the bay (the _STA method of the battery device answers 0x1F or 0x0F by it)
 *   0x82        MBST  battery state as ACPI's _BST has it: bit 0 discharging, bit 1 charging, bit 2 critical
 *   0x85 (16)   MBRM  remaining capacity
 *   0x87 (16)   MBCV  voltage in mV
 *   0x72 (16)   BFCC  full charge capacity (the _BIF / _BIX methods read it)
 * The charge in percent is MBRM / BFCC (the same unit, whatever it is; bit 15 of a capacity is a unit flag in the byte code and is dropped from both). The values are only read: nothing is ever written to the controller.
 * The controller is talked to the standard ACPI way: command port 0x66, data port 0x62, command 0x80 = read one byte (ACPI spec, chapter 12).
 *
 * The decoding is pure (battery_decode) and host-tested (tests/test_battery.c).
 * ============================================================ */
#ifndef HARIS_BATTERY_H
#define HARIS_BATTERY_H

typedef struct
{
    int present;       /* a battery is in the bay */
    int ac;            /* the AC adapter is plugged in */
    int charging;      /* the battery takes charge */
    int discharging;   /* the battery gives charge */
    int critical;      /* the controller says the charge is critically low */
    int percent;       /* 0 .. 100, -1 when it cannot be worked out */
    unsigned voltage;  /* mV */
} BatteryInfo;

/* The bytes of the controller that battery_decode() needs, as read. */
typedef struct
{
    unsigned char ac_byte;   /* 0x40 */
    unsigned char bay_byte;  /* 0xA4 */
    unsigned char state;     /* 0x82 */
    unsigned short remaining; /* 0x85, 0x86 */
    unsigned short voltage;  /* 0x87, 0x88 */
    unsigned short full;     /* 0x72, 0x73 */
} BatteryRaw;

void battery_decode(const BatteryRaw *raw, BatteryInfo *out);

/* Is this DSDT (the whole table, header first) the one of the machine whose layout is known? */
int battery_dsdt_matches(const unsigned char *dsdt, unsigned len);

void battery_init(void);
int battery_supported(void);
int battery_read(BatteryInfo *out);
/* Only the AC adapter: 1 plugged in, 0 not, -1 not supported / the controller did not answer. One byte from the controller, so it can be asked often
 * (a plug that goes in or out should show at once, the rest of battery_read() can wait a second). */
int battery_ac(void);

#endif
