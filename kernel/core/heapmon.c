/* HarisOS heap monitor: see include/heapmon.h. */
#include "heapmon.h"
#include "heap.h"
#include "serial.h"
#include "task.h"

#define PERIOD_MS 1000

static volatile unsigned CHECKS;
static volatile int LAST;

static void heapmon_thread(void *arg)
{
    (void)arg;
    for (;;)
    {
        task_sleep_ms(PERIOD_MS); /* the main loop only wakes us when the time is up (task_others_ready) */
        int r = heap_check();     /* takes the heap's lock for the whole walk */
        LAST = r;
        CHECKS++;
        if (r != 0)
        {
            serial_puts("HARISOS: kernel heap is corrupted (heap_check ");
            serial_putd(r);
            serial_puts(")\n");
            __builtin_trap(); /* the crash screen shows #UD; the serial line above says why */
        }
        if (CHECKS == 1)
            serial_puts("HARISOS: heap check ok\n");
    }
}

int heapmon_start(void) { return task_create_flags("HEAPCHK", heapmon_thread, 0, TASK_PREEMPTIBLE) < 0 ? -1 : 0; }

unsigned heapmon_checks(void) { return CHECKS; }

int heapmon_last_result(void) { return LAST; }
