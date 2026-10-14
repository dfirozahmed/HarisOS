/* HarisOS kernel core: UEFI-independent entry point. */
#include "desktop.h"

void kernel_main(void)
{
    desktop_draw();

    for (;;)
    {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("hlt");
#endif
    }
}
