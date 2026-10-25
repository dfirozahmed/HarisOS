/* HarisOS user program: ring 3 code may load a selector into GS, which sets the GS base to 0 (the GDT descriptor's base). The kernel
 * keeps its per-CPU record in a GS base too (include/percpu.h), so every entry from ring 3 must swap it in (SWAPGS) and every return
 * swap it out. This program clobbers its own GS base, makes system calls and sits through timer interrupts in ring 3; the kernel
 * checks afterwards (smp_bsp_gs_check) that its own record is still found. Exits with 0 if the system calls kept working. */
#include "sys.h"

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    __asm__ volatile("movl $0x1B, %%eax\n\tmovl %%eax, %%gs" : : : "eax"); /* user data selector: GS base = 0 */
    print("gstest: GS base cleared\n");
    for (int round = 0; round < 3; round++)
    {
        for (volatile unsigned i = 0; i < 20000000u; i++)
            ; /* long enough for several 1 ms timer interrupts while the user GS is active */
        sys_yield();
    }
    print("gstest: ok\n");
    sys_exit(0);
}
