/* ============================================================
 * HarisOS signal handlers: running a function of the program when SIGINT or SIGTERM arrives
 *
 * A program that installs a handler (SYS_SIGACTION: the handler and a small entry stub of its own, both in its own code) is not ended
 * by that signal. The kernel marks the signal pending (process.c) and runs the handler the next time the process is about to return to
 * ring 3: at the end of a system call, or at a timer tick that interrupted its code (so a program that only computes is reached too). A
 * system call that sleeps (a read, a wait, a semaphore) is woken and returns -1 first.
 *
 * What delivery does: it pushes a SigFrame (the registers that would be lost, the old signal mask and the 96 bytes of xmm0-5) on the
 * user stack below the red zone, blocks the signal, and makes the process continue at its entry stub with rdx = the handler and
 * r8 = the signal number and the stack as if the stub had been called (rsp = 8 mod 16, [rsp] = 0). The stub calls handler(sig) (Microsoft
 * x64: argument in rcx) and then SYS_SIGRETURN. SYS_SIGRETURN reads the SigFrame back (all of it is checked: the address, the magic
 * number, rip and rsp in the user half; the flags are masked to the arithmetic bits, DF and TF, IF set) and goes back to where the
 * process was, with every register as it was. For a signal that came in at a system call that is a normal return; for one that came in at
 * a timer tick the return is an IRETQ, because rcx and r11 (which SYSRET uses) also have to come back.
 *
 * Not saved: the x87 stack and MXCSR (a handler is expected to leave them as it found them), xmm6-xmm15, rbx, rbp, rdi, rsi, r12-r15 (the
 * handler is a normal function and keeps those).
 * ============================================================ */
#ifndef HARIS_USERSIG_H
#define HARIS_USERSIG_H

#include "process.h"
#include "syscall.h"

/* What the timer interrupt stub pushes: r11 is at the lowest address (the last pushed), then the five words the CPU pushed. The 96
 * bytes of xmm0-5 that the stub saved are just below this frame. */
typedef struct
{
    unsigned long long r11, r10, r9, r8, rdx, rcx, rax;
    unsigned long long rip, cs, rflags, rsp, ss;
} IrqFrame;

#define SIGSEGV_EXIT (128 + 11) /* the code of a process ended because its signal frame could not be built or read */

/* The signals that can have a handler (SIGINT, SIGTERM) */
#define SIG_HANDLER_MASK ((1u << SIGINT) | (1u << SIGTERM))

/* Is there a signal for this process that is pending, not blocked, and has a handler? */
int signal_deliverable(const Process *p);

/* At the end of a system call, with the result it is about to return: if a handler has to run, the frame is changed so that the
 * process continues at its entry stub. Returns the value for rax. */
long long signal_syscall_return(SyscallFrame *f, long long result);

/* In the timer interrupt: the same for a process that was interrupted in ring 3. */
void signal_irq(IrqFrame *f);

/* SYS_SIGRETURN: back to the registers the SigFrame holds. Returns the value for rax. */
long long signal_sigreturn(SyscallFrame *f);

#endif
