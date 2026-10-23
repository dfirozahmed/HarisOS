/* HarisOS user program: writes to address 0 on purpose. The kernel must end only this process (exit code -1 for its
 * parent's SYS_WAIT) and carry on. */
#include "sys.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    *(volatile int *)0 = 1;
    sys_exit(0); /* not reached */
}
