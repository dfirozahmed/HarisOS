/* Host test for battery.c: the DSDT check (only the machine whose layout is known), the decoding of the controller's bytes, and battery_read()
 * against a pretend controller (a 256 byte array; a register told to fail answers -1, like a controller that does not answer). */
#include <stdio.h>
#include <string.h>
#include "acpi.h"
#include "../kernel/device/battery.c"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- the pretend ACPI tables ---- */
static unsigned char DSDT[64];
static int NTAB = 1;
int acpi_aml_count(void) { return NTAB; }
const unsigned char *acpi_aml(int i, unsigned *len)
{
    if (i != 0)
        return 0;
    *len = sizeof DSDT;
    return DSDT;
}

static void make_dsdt(const char *oem, const char *table)
{
    memset(DSDT, 0, sizeof DSDT);
    memcpy(DSDT, "DSDT", 4);
    memcpy(DSDT + 10, oem, 6);
    memcpy(DSDT + 16, table, 8);
}

/* ---- the pretend controller ---- */
static unsigned char EC[256];
static int EC_DEAD = -1; /* a register that does not answer (-1 = all answer) */
int battery_host_ec_read(unsigned reg) { return (int)reg == EC_DEAD ? -1 : EC[reg & 255]; }

static void put16(unsigned reg, unsigned v) { EC[reg] = (unsigned char)v; EC[reg + 1] = (unsigned char)(v >> 8); }

static void ec_set(int ac, int bay, int state, unsigned rem, unsigned full, unsigned volt)
{
    memset(EC, 0, sizeof EC);
    EC_DEAD = -1;
    EC[0x40] = (unsigned char)ac;
    EC[0xA4] = (unsigned char)bay;
    EC[0x82] = (unsigned char)state;
    put16(0x85, rem);
    put16(0x72, full);
    put16(0x87, volt);
}

int main(void)
{
    /* ---- which machine ---- */
    make_dsdt("HPQOEM", "8707    ");
    CHECK(battery_dsdt_matches(DSDT, sizeof DSDT));
    make_dsdt("HPQOEM", "8708    ");
    CHECK(!battery_dsdt_matches(DSDT, sizeof DSDT));
    make_dsdt("LENOVO", "8707    ");
    CHECK(!battery_dsdt_matches(DSDT, sizeof DSDT));
    make_dsdt("HPQOEM", "8707    ");
    DSDT[0] = 'S';
    CHECK(!battery_dsdt_matches(DSDT, sizeof DSDT)); /* not a DSDT */
    CHECK(!battery_dsdt_matches(DSDT, 20) && !battery_dsdt_matches(0, 100));

    /* ---- decoding ---- */
    BatteryRaw r;
    BatteryInfo b;
    memset(&r, 0, sizeof r);
    r.ac_byte = 1; r.bay_byte = 1; r.state = 2; r.remaining = 1350; r.full = 2700; r.voltage = 11426;
    battery_decode(&r, &b);
    CHECK(b.present && b.ac && b.charging && !b.discharging && !b.critical && b.percent == 50 && b.voltage == 11426);
    r.ac_byte = 0; r.state = 1; r.remaining = 2699;
    battery_decode(&r, &b);
    CHECK(!b.ac && !b.charging && b.discharging && b.percent == 100); /* 99.96 % rounds to 100 */
    r.remaining = 2000;
    battery_decode(&r, &b);
    CHECK(b.percent == 74); /* 74.07 */
    r.remaining = 27;
    battery_decode(&r, &b);
    CHECK(b.percent == 1);
    r.remaining = 5; r.state = 1 | 4;
    battery_decode(&r, &b);
    CHECK(b.percent == 0 && b.critical && b.discharging);
    r.remaining = 9999; r.state = 0; /* more than full (a reading in the middle of an update) */
    battery_decode(&r, &b);
    CHECK(b.percent == 100 && !b.charging && !b.discharging);
    r.remaining = 0x8000 | 1350; r.full = 0x8000 | 2700; /* the unit flag is on both */
    battery_decode(&r, &b);
    CHECK(b.percent == 50);
    r.full = 0;
    battery_decode(&r, &b);
    CHECK(b.percent == -1); /* full charge unknown: no guess */
    r.full = 2700; r.bay_byte = 0; r.state = 2;
    battery_decode(&r, &b);
    CHECK(!b.present && b.percent == -1 && !b.charging && !b.discharging); /* no battery: the state byte means nothing */
    r.state = 1; r.ac_byte = 3;
    battery_decode(&r, &b);
    CHECK(b.ac); /* only bit 0 */
    r.bay_byte = 3; r.state = 3; /* charging and discharging both claimed: charging wins */
    battery_decode(&r, &b);
    CHECK(b.charging && !b.discharging);

    /* ---- reading ---- */
    make_dsdt("HPQOEM", "8707    ");
    battery_init();
    CHECK(battery_supported());
    ec_set(1, 1, 2, 1350, 2700, 11426);
    CHECK(battery_read(&b) == 0 && b.present && b.ac && b.charging && b.percent == 50 && b.voltage == 11426);
    ec_set(0, 1, 1, 810, 2700, 12100);
    CHECK(battery_read(&b) == 0 && !b.ac && b.discharging && b.percent == 30 && b.voltage == 12100);
    ec_set(1, 0, 0, 0, 0, 0);
    CHECK(battery_read(&b) == 0 && !b.present && b.ac && b.percent == -1);
    ec_set(1, 1, 2, 1350, 2700, 11426);
    EC_DEAD = 0x85; /* the controller stops answering in the middle */
    CHECK(battery_read(&b) == -1);
    EC_DEAD = 0x72;
    CHECK(battery_read(&b) == -1);
    EC_DEAD = 0x40;
    CHECK(battery_read(&b) == -1);
    EC_DEAD = -1;
    CHECK(battery_read(&b) == 0);

    /* ---- another machine: nothing is read ---- */
    make_dsdt("ACRSYS", "ACRPRDCT");
    battery_init();
    CHECK(!battery_supported() && battery_read(&b) == -1);
    NTAB = 0;
    battery_init();
    CHECK(!battery_supported());
    NTAB = 1;

    if (!fails)
        printf("battery tests passed\n");
    return fails ? 1 : 0;
}
