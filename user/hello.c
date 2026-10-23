/* HarisOS user program: the first one that is a real file. It is built as an ELF executable (see CMakeLists.txt),
 * stored on the boot disk as HELLO.ELF and started by the kernel's ELF loader (kernel/core/exec.c) - at boot as a self
 * test, or by a double click on it in the File Explorer, when its text appears in a console window.
 *
 * There is no C library: the program talks to the kernel with the "syscall" instruction (user/sys.h). It exercises what a
 * loader must get right: initialised data (.data), zero-filled data (.bss), read-only data (.rodata), and a stack, and
 * exits with 42 only if every one of them has the right content. */
#include "sys.h"

static const char MESSAGE[] = "hello from an ELF program\n";
static const unsigned SQUARES[8] = {0, 1, 4, 9, 16, 25, 36, 49}; /* .rodata */
static unsigned counter = 1000;                                   /* .data */
static unsigned scratch[8192];                                    /* .bss: 32 KiB (8 pages) that must start as zero */

/* The kernel starts a program with the stack pointer 8 mod 16, as if a call had pushed a return address, which is what
 * the C calling convention expects at the first instruction of a function. (force_align_arg_pointer is a precaution.) */
__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    sys_write(1, MESSAGE, sizeof(MESSAGE) - 1);

    int ok = 1;
    for (unsigned i = 0; i < sizeof(scratch) / sizeof(scratch[0]); i++)
        if (scratch[i] != 0)
            ok = 0; /* .bss was not zero */
    counter += 234;
    if (counter != 1234)
        ok = 0; /* .data was wrong, or is not writable */
    for (unsigned i = 0; i < 8; i++)
        scratch[i * 1024] = SQUARES[i] + counter; /* one word in each of 8 different pages of .bss */
    unsigned sum = 0;
    for (unsigned i = 0; i < 8; i++)
        sum += scratch[i * 1024];
    if (sum != 140 + 8 * 1234)
        ok = 0; /* .rodata or the writes to .bss were wrong */

    volatile char local[256]; /* the stack */
    for (int i = 0; i < 256; i++)
        local[i] = (char)i;
    for (int i = 0; i < 256; i++)
        if (local[i] != (char)i)
            ok = 0;

    sys_exit(ok ? 42 : 1);
}
