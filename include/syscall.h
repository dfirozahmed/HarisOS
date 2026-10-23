/* ============================================================
 * HarisOS system calls: how ring 3 code asks the kernel for something
 *
 *   syscall_init();    once, after gdt_init(): turn on the SYSCALL instruction and point it at our entry code
 *
 * User code puts the call number in rax and up to three arguments in rdi, rsi, rdx, then executes "syscall". The result
 * comes back in rax. Everything except rax, rcx and r11 is preserved (rcx and r11 are used by the instruction itself to
 * save the return address and the flags); xmm0-xmm5 may be changed.
 *
 *   SYS_WRITE (1)  rdi = pointer, rsi = length (at most SYS_WRITE_MAX): send the bytes to the serial port.
 *                  Returns the length, or -1 if the buffer is not (all) readable user memory.
 *   SYS_EXIT  (2)  rdi = exit code: the process ends. Does not return.
 *   SYS_YIELD (3)  give the CPU to another thread. Returns 0.
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

#define SYS_WRITE_MAX 4096 /* one write call sends at most this many bytes */

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
