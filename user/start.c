/* HarisOS user C library, program start for ported (ISO C / POSIX style) programs: they define main(argc, argv) instead of _start.
 * Linked only into the programs that CMake lists as ported (see HARIS_PORTED_PROGRAMS); the hand-written ones keep their own _start.
 *
 * The kernel starts a program with argc in rcx and argv in rdx (also rdi / rsi, see user/sys.h). The stack pointer is misaligned by 8 at
 * entry as after a call, and the compiler assumes it is aligned, hence force_align_arg_pointer (as in the other programs). */
#include "libc.h"

int main(int argc, char **argv);

__attribute__((noreturn, force_align_arg_pointer)) void _start(long long argc, char **argv)
{
    static char *no_args[] = {"program", 0};
    if (argc <= 0 || !argv) /* started from the File Explorer: no arguments */
    {
        argc = 1;
        argv = no_args;
    }
    exit(main((int)argc, argv));
}
