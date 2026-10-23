/* ============================================================
 * HarisOS system calls: how ring 3 code asks the kernel for something
 *
 *   syscall_init();    once, after gdt_init(): turn on the SYSCALL instruction and point it at our entry code
 *
 * User code puts the call number in rax and up to three arguments in rdi, rsi, rdx, then executes "syscall". The result
 * comes back in rax. Everything except rax, rcx and r11 is preserved (rcx and r11 are used by the instruction itself to
 * save the return address and the flags). The floating point and SSE registers are the process's own and are kept.
 *
 *   SYS_WRITE (1)  rdi = fd (1 or 2: the console), rsi = pointer, rdx = length (at most SYS_WRITE_MAX): print the bytes.
 *                  In the program's console window; with no window, on the serial port. Returns the length, or -1 if
 *                  the fd is not 1 or 2 or the buffer is not (all) readable user memory.
 *   SYS_EXIT  (2)  rdi = exit code: the process ends. Does not return.
 *   SYS_YIELD (3)  give the CPU to another thread. Returns 0.
 *   SYS_READ  (4)  rdi = fd, rsi = pointer, rdx = length: read into the buffer. Returns the number of bytes (0 = end).
 *                  fd 0 is the console: it waits for the user to type a line (and press Enter), then returns that line,
 *                  newline included, at most one line per call; 0 when the window is closed or there is no window.
 *                  A file descriptor from SYS_OPEN reads the file from its current position and moves it; a call may
 *                  return fewer bytes than asked (at most SYS_READ_MAX at once). -1 for a bad fd or buffer.
 *   SYS_OPEN  (5)  rdi = pointer to a NUL-terminated path, rsi = flags (0 = read only, the only kind). Returns a file
 *                  descriptor (3 and up) or -1. A path without '/' is a file name looked up in the top folder of every
 *                  drive ("HELLO.ELF"); a path starting with '/' is a VFS path ("/RAM Disk/Documents/readme.txt").
 *                  At most PROCESS_MAX_FILES files are open at once.
 *   SYS_CLOSE (6)  rdi = fd. Returns 0, or -1 if it is not open. (Closing 0, 1 or 2 does nothing.)
 *   SYS_SEEK  (7)  rdi = fd, rsi = offset (signed), rdx = whence (0 = from the start, 1 = from the current position,
 *                  2 = from the end of the file). Returns the new position, or -1 (bad fd, bad whence, negative result).
 *   SYS_BRK   (8)  rdi = new program break (the end of the heap), or 0. The heap starts at the first page after the
 *                  program's last segment and may grow to PROCESS_HEAP_MAX; new pages are zeroed and not executable.
 *                  0 returns the current break. A value outside heap_base ... heap_limit returns -1 and changes
 *                  nothing; otherwise the break becomes rdi and that is returned. Lowering the break does not give
 *                  pages back (they stay mapped, not cleared again, until the process ends). malloc is built on this.
 *   anything else  returns -1.
 *
 * What the CPU does on SYSCALL: RIP goes to LSTAR, CS / SS are loaded from STAR (kernel code / data), the return address
 * is saved in rcx, the flags in r11, and flags are masked with SFMASK (interrupts off). It does NOT change the stack
 * and does NOT save anything else, so the entry code (in syscall.c) first moves to the thread's kernel stack, then
 * saves the registers, and leaves with SYSRET. System calls run with interrupts off, so a call is never preempted and
 * needs no locks; they must therefore be short.
 *
 * The one rule that matters: nothing from ring 3 is trusted. Every pointer is checked (paging_user_range_ok) before the
 * kernel reads or writes through it.
 * ============================================================ */
#ifndef HARIS_SYSCALL_H
#define HARIS_SYSCALL_H

#define SYS_WRITE 1
#define SYS_EXIT 2
#define SYS_YIELD 3
#define SYS_READ 4
#define SYS_OPEN 5
#define SYS_CLOSE 6
#define SYS_SEEK 7
#define SYS_BRK 8

#define SYS_WRITE_MAX 4096 /* one write call sends at most this many bytes */
#define SYS_READ_MAX 65536 /* one read call returns at most this many bytes */

/* What the entry code saves on the kernel stack, lowest address first (the push order in syscall.c). */
typedef struct
{
    unsigned long long r9, r8, r10, rdx, rsi, rdi;
    unsigned long long rax; /* call number in, result out */
    unsigned long long rcx; /* user RIP: where SYSRET goes */
    unsigned long long r11; /* user RFLAGS */
    unsigned long long user_rsp;
} SyscallFrame;

void syscall_init(void);

/* The stack the entry code switches to (and the CPU, for interrupts from ring 3): the top of the running process
 * thread's kernel stack. Set by the scheduler on every switch to a process thread. */
void syscall_set_kernel_stack(unsigned long long top);

/* The work of a system call, called by the entry code with the saved registers. Returns the value for rax. */
long long syscall_dispatch(SyscallFrame *f);

unsigned long long syscall_count(void); /* calls made since boot */

#endif
