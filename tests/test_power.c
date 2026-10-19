/* Host-side tests for kernel/core/power.c: the firmware reset service is asked first, with the right type.
 * The fake service "powers off" by jumping out (a real one never returns), so the hardware fallbacks
 * (port writes) never run on the host. */
#include "power.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>

static jmp_buf OUT;
static int LAST_TYPE = -1, CALLS;

static void fake_reset(unsigned type)
{
    LAST_TYPE = (int)type;
    CALLS++;
    longjmp(OUT, 1);
}

int main(void)
{
    BootInfo info = {0};
    info.reset_system = fake_reset;
    power_init(&info);

    if (setjmp(OUT) == 0)
        power_shutdown();
    assert(CALLS == 1 && LAST_TYPE == BOOT_RESET_SHUTDOWN && LAST_TYPE == 2);

    if (setjmp(OUT) == 0)
        power_restart();
    assert(CALLS == 2 && LAST_TYPE == BOOT_RESET_COLD && LAST_TYPE == 0);

    printf("power tests passed\n");
    return 0;
}
