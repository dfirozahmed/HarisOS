/* Real-time clock (CMOS) reader, x86 only. */
#ifndef HARIS_RTC_H
#define HARIS_RTC_H

typedef struct
{
    int year, month, day, hour, minute, second;
} RtcTime;

/* Returns 0 on success, -1 if unavailable. The CMOS clock usually holds UTC
 * (or local time, depending on firmware settings). */
int rtc_read(RtcTime *out);

#endif
