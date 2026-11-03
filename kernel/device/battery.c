/* Battery level from the ACPI embedded controller. See include/battery.h for where the numbers come from and why only one machine is supported. */
#include "acpi.h"
#include "battery.h"
#include "io.h"

#ifdef HARIS_HOST
/* the host test plays the controller: this is its answer for one register (-1 = it does not answer) */
int battery_host_ec_read(unsigned reg);
#endif

typedef unsigned char u8;
typedef unsigned short u16;

/* ---- the machine ---- */
#define EC_AC 0x40
#define EC_BAY 0xA4
#define EC_STATE 0x82
#define EC_REMAINING 0x85
#define EC_VOLTAGE 0x87
#define EC_FULL 0x72

/* the ACPI table header: signature 0..3, length 4..7, revision 8, checksum 9, OEM id 10..15, OEM table id 16..23 */
int battery_dsdt_matches(const unsigned char *t, unsigned len)
{
    static const char OEM[6] = {'H', 'P', 'Q', 'O', 'E', 'M'};
    static const char TABLE[4] = {'8', '7', '0', '7'};
    if (!t || len < 36 || t[0] != 'D' || t[1] != 'S' || t[2] != 'D' || t[3] != 'T')
        return 0;
    for (int i = 0; i < 6; i++)
        if (t[10 + i] != (u8)OEM[i])
            return 0;
    for (int i = 0; i < 4; i++)
        if (t[16 + i] != (u8)TABLE[i])
            return 0;
    return 1;
}

/* ---- decoding (pure) ---- */
void battery_decode(const BatteryRaw *r, BatteryInfo *o)
{
    o->present = r->bay_byte & 1;
    o->ac = r->ac_byte & 1;
    o->charging = o->present && (r->state & 2);
    o->discharging = o->present && (r->state & 1) && !o->charging;
    o->critical = o->present && (r->state & 4);
    o->voltage = r->voltage;
    /* a capacity with bit 15 set is in another unit (the byte code tests it); both numbers use the same one, so the bit is dropped */
    unsigned full = r->full & 0x7FFFu, rem = r->remaining & 0x7FFFu;
    if (!o->present || full == 0)
        o->percent = -1;
    else
    {
        if (rem > full)
            rem = full;
        o->percent = (int)((rem * 100u + full / 2u) / full);
    }
}

#ifndef HARIS_HOST
/* ---- the controller: ACPI chapter 12.3 ----
 * Status port 0x66: bit 0 = output buffer full (a byte waits for us at 0x62), bit 1 = input buffer full (it has not taken our last byte).
 * A read: wait until the input buffer is empty, write 0x80 to the command port, wait, write the register number to the data port, wait until
 * the output buffer is full, read the byte from the data port. */
#define EC_DATA 0x62
#define EC_CMD 0x66
#define EC_OBF 0x01
#define EC_IBF 0x02
#define EC_RD 0x80

static int ec_wait(u8 mask, u8 want)
{
    for (unsigned i = 0; i < 20000; i++) /* each pass is about a microsecond: up to some milliseconds */
    {
        if ((io_in8(EC_CMD) & mask) == want)
            return 1;
        io_in8(0x80);
    }
    return 0;
}

static int ec_read(unsigned reg)
{
    if (io_in8(EC_CMD) & EC_OBF)
        io_in8(EC_DATA); /* an old byte nobody took: it would be taken for the answer */
    if (!ec_wait(EC_IBF, 0))
        return -1;
    io_out8(EC_CMD, EC_RD);
    if (!ec_wait(EC_IBF, 0))
        return -1;
    io_out8(EC_DATA, (u8)reg);
    if (!ec_wait(EC_OBF, EC_OBF))
        return -1;
    return io_in8(EC_DATA);
}
#else
static int ec_read(unsigned reg) { return battery_host_ec_read(reg); }
#endif

static int OK;

void battery_init(void)
{
    OK = 0;
    for (int i = 0; i < acpi_aml_count(); i++)
    {
        unsigned len;
        const unsigned char *t = acpi_aml(i, &len);
        if (battery_dsdt_matches(t, len))
            OK = 1;
    }
#ifndef HARIS_HOST
    if (OK && io_in8(EC_CMD) == 0xFF) /* nothing answers at the controller's port */
        OK = 0;
#endif
}

int battery_supported(void) { return OK; }

static int rd16(unsigned reg)
{
    int lo = ec_read(reg), hi = ec_read(reg + 1);
    return lo < 0 || hi < 0 ? -1 : lo | hi << 8;
}

int battery_ac(void)
{
    if (!OK)
        return -1;
    int v = ec_read(EC_AC);
    return v < 0 ? -1 : v & 1;
}

int battery_read(BatteryInfo *o)
{
    if (!OK)
        return -1;
    BatteryRaw r;
    int ac = ec_read(EC_AC), bay = ec_read(EC_BAY);
    if (ac < 0 || bay < 0)
        return -1;
    r.ac_byte = (u8)ac;
    r.bay_byte = (u8)bay;
    r.state = 0;
    r.remaining = r.voltage = r.full = 0;
    if (bay & 1) /* no battery in the bay: the other registers mean nothing */
    {
        int st = ec_read(EC_STATE), rem = rd16(EC_REMAINING), volt = rd16(EC_VOLTAGE), full = rd16(EC_FULL);
        if (st < 0 || rem < 0 || volt < 0 || full < 0)
            return -1;
        r.state = (u8)st;
        r.remaining = (u16)rem;
        r.voltage = (u16)volt;
        r.full = (u16)full;
    }
    battery_decode(&r, o);
    return 0;
}
