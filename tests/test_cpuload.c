/* Host test for cpuload.c: busy share of 1-second windows. */
#include <stdio.h>
#include "cpuload.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static unsigned long long NOW_US;
unsigned long long timer_us(void) { return NOW_US; }

int main(void)
{
    CHECK(cpuload_percent() == 0);

    cpuload_poll(5000000);            /* first call only starts the window */
    cpuload_add_busy(100000);
    cpuload_poll(5500000);            /* window not over yet */
    CHECK(cpuload_percent() == 0);
    cpuload_poll(6000000);            /* 100 ms busy of 1 s */
    CHECK(cpuload_percent() == 10);

    cpuload_poll(7000000);            /* nothing happened */
    CHECK(cpuload_percent() == 0);

    cpuload_add_busy(5000000);        /* more busy than the window is long: clamped */
    cpuload_poll(8000000);
    CHECK(cpuload_percent() == 100);

    cpuload_add_busy(250000);
    cpuload_poll(9500000);            /* a late poll: 250 ms of 1.5 s */
    CHECK(cpuload_percent() == 16);

    cpuload_poll(1000);               /* clock went backwards: ignored */
    CHECK(cpuload_percent() == 16);

    cpuload_note_idle(300); cpuload_note_idle(200);   /* halted inside a pass: taken once, then gone */
    CHECK(cpuload_take_idle() == 500 && cpuload_take_idle() == 0);

    {   /* sections: the length less the time halted inside it */
        unsigned long long us[CPULOAD_SECTIONS], n[CPULOAD_SECTIONS];
        NOW_US = 1000; cpuload_section_begin(); cpuload_note_idle(300); NOW_US = 1900; cpuload_section_end(2);
        NOW_US = 2000; cpuload_section_begin(); NOW_US = 2100; cpuload_section_end(2);
        cpuload_sections_take(us, n);
        CHECK(us[2] == 600 + 100 && n[2] == 2 && us[0] == 0);
        cpuload_sections_take(us, n);
        CHECK(us[2] == 0 && n[2] == 0);
    }

    if (fails == 0)
        printf("cpuload tests passed\n");
    return fails ? 1 : 0;
}
