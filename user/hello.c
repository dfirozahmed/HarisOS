/* HarisOS user program: the first one that is a real file. It is built as an ELF executable (see CMakeLists.txt),
 * stored on the boot disk as HELLO.ELF and started by the kernel's ELF loader (kernel/core/exec.c).
 *
 * There is no C library: the program talks to the kernel with the "syscall" instruction (include/syscall.h). It
 * exercises what a loader must get right: initialised data (.data), zero-filled data (.bss), read-only data (.rodata),
 * and a stack, and exits with 42 only if every one of them has the right content. */

static const char MESSAGE[] = "hello from an ELF program\n";
static const unsigned SQUARES[8] = {0, 1, 4, 9, 16, 25, 36, 49}; /* .rodata */
static unsigned counter = 1000;                                   /* .data */
static unsigned scratch[8192];                                    /* .bss: 32 KiB (8 pages) that must start as zero */

/* long long, not long: the program is compiled for the same 64-bit Windows-style target as the kernel, where long is 32 bits */
static long long syscall3(long long number, long long a, long long b, long long c)
{
    long long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}

#define SYS_WRITE 1
#define SYS_EXIT 2

__attribute__((noreturn)) static void leave(int code)
{
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;)
        ;
}

/* The kernel starts a program with a 16-byte aligned stack pointer; the C calling convention wants it to be 8 mod 16
 * at the first instruction of a function, as if a call had pushed a return address. */
__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    syscall3(SYS_WRITE, (long long)(unsigned long long)MESSAGE, sizeof(MESSAGE) - 1, 0);

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

    leave(ok ? 42 : 1);
}
