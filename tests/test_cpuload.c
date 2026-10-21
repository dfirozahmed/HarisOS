/* Host test for cpuload.c: busy share of 1-second windows. */
#include <stdio.h>
#include "cpuload.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

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

    if (fails == 0)
        printf("cpuload tests passed\n");
    return fails ? 1 : 0;
}
