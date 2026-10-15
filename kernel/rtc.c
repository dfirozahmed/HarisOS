/* CMOS real-time clock: index port 0x70, data port 0x71. */
#include "rtc.h"

#if defined(__x86_64__) || defined(__i386__)

static void outb(unsigned short port, unsigned char v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static unsigned char inb(unsigned short port)
{
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static unsigned char cmos(unsigned char reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd(int v) { return (v & 0x0F) + (v >> 4) * 10; }

static void read_raw(unsigned char r[6])
{
    for (int i = 0; i < 200000 && (cmos(0x0A) & 0x80); i++) {} /* wait: update in progress */
    r[0] = cmos(0x00); r[1] = cmos(0x02); r[2] = cmos(0x04);
    r[3] = cmos(0x07); r[4] = cmos(0x08); r[5] = cmos(0x09);
}

int rtc_read(RtcTime *out)
{
    unsigned char a[6], b[6];
    if (!out)
        return -1;
    /* Read twice until two reads agree, so we never mix values across an update. */
    read_raw(a);
    for (int tries = 0; tries < 5; tries++)
    {
        read_raw(b);
        int same = 1;
        for (int i = 0; i < 6; i++)
            if (a[i] != b[i])
                same = 0;
        if (same)
            break;
        for (int i = 0; i < 6; i++)
            a[i] = b[i];
    }

    unsigned char regb = cmos(0x0B);
    int pm = (a[2] & 0x80) != 0;
    int sec = a[0], min = a[1], hour = a[2] & 0x7F, day = a[3], mon = a[4], year = a[5];
    if (!(regb & 0x04)) /* values are BCD */
    {
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year);
    }
    if (!(regb & 0x02) && pm) /* 12-hour mode, PM flag */
        hour = (hour % 12) + 12;
    else if (!(regb & 0x02) && hour == 12)
        hour = 0;

    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59)
        return -1;
    out->year = 2000 + year;
    out->month = mon; out->day = day;
    out->hour = hour; out->minute = min; out->second = sec;
    return 0;
}

#else

int rtc_read(RtcTime *out)
{
    (void)out;
    return -1;
}

#endif
