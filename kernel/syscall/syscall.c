/* HarisOS system calls: see include/syscall.h. */
#include "syscall_internal.h"

static ull COUNT;      /* atomic: the calls that run without the big kernel lock count themselves too */
static ull FAST_COUNT; /* how many of them did */

ull syscall_count(void) { return __atomic_load_n(&COUNT, __ATOMIC_RELAXED); }
ull syscall_fast_count(void) { return __atomic_load_n(&FAST_COUNT, __ATOMIC_RELAXED); }

static long long dispatch_call(SyscallFrame *f)
{
    __atomic_fetch_add(&COUNT, 1, __ATOMIC_RELAXED);
    switch (f->rax)
    {
    case SYS_WRITE:
        return sys_write(f);
    case SYS_EXIT:
        process_exit((int)f->rdi); /* does not return */
    case SYS_YIELD:
        task_yield();
#if defined(__x86_64__) && !defined(HARIS_HOST)
        /* With one processor a yield hands the processor to the other threads, which then run for a whole slice. With several, the others
         * already run on their own processors and a yield returns at once, so a program that yields to "give the others time" (a parent
         * waiting for its child to get going) would not give them any. One tick of sleep restores that meaning. */
        if (smp_cpu_total() > 1)
            task_sleep_ms(1);
#endif
        return 0;
    case SYS_READ:
        return sys_read(f);
    case SYS_OPEN:
        return sys_open(f);
    case SYS_CLOSE:
        return sys_close(f);
    case SYS_SEEK:
        return sys_seek(f);
    case SYS_BRK:
        return sys_brk(f);
    case SYS_SPAWN:
        return sys_spawn(f);
    case SYS_WAIT:
        return sys_wait(f);
    case SYS_READDIR:
        return sys_readdir(f);
    case SYS_STAT:
        return sys_stat(f);
    case SYS_CHDIR:
        return sys_chdir(f);
    case SYS_GETCWD:
        return sys_getcwd(f);
    case SYS_MKDIR:
        return sys_mkdir(f);
    case SYS_UNLINK:
        return sys_unlink(f);
    case SYS_RMDIR:
        return sys_rmdir(f);
    case SYS_RENAME:
        return sys_rename(f);
    case SYS_LOCK:
        return sys_lock(f);
    case SYS_PIPE:
        return sys_pipe(f);
    case SYS_KILL:
        return sys_kill(f);
    case SYS_SIGIGNORE:
        return sys_sigignore(f);
    case SYS_PSINFO:
        return sys_psinfo(f);
    case SYS_FORK:
        return sys_fork(f);
    case SYS_EXEC:
        return sys_exec(f);
    case SYS_GETPID:
        return sys_getpid(f);
    case SYS_DUP2:
        return sys_dup2(f);
    case SYS_SHM_MAP:
        return sys_shm_map(f);
    case SYS_SHM_UNMAP:
        return sys_shm_unmap(f);
    case SYS_SHM_UNLINK:
        return sys_shm_unlink(f);
    case SYS_SHM_SIZE:
        return sys_shm_size(f);
    case SYS_MQ_OPEN:
        return sys_mq_open(f);
    case SYS_MQ_UNLINK:
        return sys_mq_unlink(f);
    case SYS_SEM_OPEN:
        return sys_sem_open(f);
    case SYS_SEM_WAIT:
        return sys_sem_wait(f);
    case SYS_SEM_POST:
        return sys_sem_post(f);
    case SYS_SEM_VALUE:
        return sys_sem_value(f);
    case SYS_SEM_UNLINK:
        return sys_sem_unlink(f);
    case SYS_MUTEX_OPEN:
        return sys_mutex_open(f);
    case SYS_MUTEX_LOCK:
        return sys_mutex_lock(f);
    case SYS_MUTEX_UNLOCK:
        return sys_mutex_unlock(f);
    case SYS_COND_OPEN:
        return sys_cond_open(f);
    case SYS_COND_WAIT:
        return sys_cond_wait(f);
    case SYS_COND_SIGNAL:
        return sys_cond_signal(f);
    case SYS_SYNC_UNLINK:
        return sys_sync_unlink(f);
    case SYS_SETPRIO:
        return sys_setprio(f);
    case SYS_GUI_CREATE:
        return sys_gui_create(f);
    case SYS_GUI_PRESENT:
        return sys_gui_present(f);
    case SYS_GUI_EVENT:
        return sys_gui_event(f);
    case SYS_GUI_CTL:
        return sys_gui_ctl(f);
    case SYS_SOCKET:
        return sys_socket(f);
    case SYS_CONNECT:
        return sys_connect(f);
    case SYS_RESOLVE:
        return sys_resolve(f);
    case SYS_BIND:
        return sys_bind(f);
    case SYS_LISTEN:
        return sys_listen(f);
    case SYS_ACCEPT:
        return sys_accept(f);
    case SYS_WIFISCAN:
        return sys_wifiscan(f);
    case SYS_CLOCK:
        return sys_clock(f);
    case SYS_TIME:
        return sys_time(f);
    case SYS_SIGACTION:
        return sys_sigaction(f);
    case SYS_SIGRETURN:
        return signal_sigreturn(f);
    case SYS_SIGMASK:
        return sys_sigmask(f);
    default:
        return -1;
    }
}

/* Every system call goes through here: after the work, a signal handler that is due runs before the process sees the result (usersig.h).
 * SYS_SIGRETURN is the one that comes back from a handler: it is not interrupted by the next one at once (the timer tick will do it). */
long long syscall_dispatch(SyscallFrame *f)
{
    /* The stack pointer may be below the mapped part of the stack (a big frame was made with one subtraction and nothing touched yet): map
     * it down to there now, so that the pointers the call checks (a buffer in that frame) are valid. */
    process_stack_grow(f->user_rsp);
    long long r = dispatch_call(f);
    if (f->rax != SYS_SIGRETURN && signal_deliverable(process_current()))
        r = signal_syscall_return(f, r);
    return r;
}


#if defined(__x86_64__) && !defined(HARIS_HOST)

#include "bkl.h"
#include "percpu.h"

typedef unsigned long long u64;

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE 1ULL

#define SYSCALL_STR_(x) #x
#define SYSCALL_STR(x) SYSCALL_STR_(x)

/* The system calls that run without the big kernel lock (syscall_entry decides by the call number before it takes it): only SYS_CLOCK for
 * now, which reads the processor's time stamp counter and constants that never change after boot. Not for anything that reaches the
 * process or thread tables, a device or a user pointer: those need the lock (bkl.h). Interrupts are off, the stack is this thread's kernel
 * stack. Returns the result. */
long long syscall_fast(SyscallFrame *f);
long long syscall_fast(SyscallFrame *f)
{
    (void)f;
    __atomic_fetch_add(&COUNT, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&FAST_COUNT, 1, __ATOMIC_RELAXED);
    return (long long)timer_ms();
}

/* RFLAGS bits cleared on entry: IF (no interrupts until we are back), TF, DF (the C code expects it clear), NT, AC. */
#define SYSCALL_FLAG_MASK 0x44700ULL

/* The state of a system call in progress (the user's RSP, the kernel stack to switch to, the exit-through-IRETQ request) is kept in the
 * per-CPU record of the processor that runs it (percpu.h, reached through GS), because two processors can be in a system call at once. */

/* A return from a signal handler that came in at a timer tick must restore rcx and r11 too, which SYSRET cannot: the exit then goes
 * through IRETQ (signal.c asks with syscall_request_iret; the exit code below looks at the flag). */
void syscall_request_iret(u64 rip, u64 rflags)
{
    PerCpu *c = this_cpu();
    c->iret_rip = rip;
    c->iret_rflags = rflags;
    c->use_iret = 1;
}

/* The code the CPU jumps to on SYSCALL. At this point RSP is still the user's. Frame layout = SyscallFrame.
 *
 * Alignment: the stack top is 16-aligned and the frame is 10 qwords (80 bytes), so RSP is 16-aligned after the pushes;
 * 32 more bytes are the shadow space of the Microsoft x64 call convention (arguments in rcx).
 *
 * Before SYSRET the user RIP in rcx must be a canonical address: a non-canonical one makes SYSRET fault in ring 0
 * while RSP is already the user's. The user RIP is the address after the SYSCALL instruction, which is canonical unless
 * the instruction sits at the very end of the lower half; such a process is ended instead. */
__asm__(".text\n"
        ".p2align 4\n"
        ".globl syscall_entry\n"
        "syscall_entry:\n"
        "    swapgs\n" /* SYSCALL leaves GS alone: swap in the kernel's per-CPU base (percpu.h); the user's waits in IA32_KERNEL_GS_BASE */
        /* A call that touches no kernel state but the clock does not need the big kernel lock (syscall_fast): the processor flags that in its
         * own record (gs:72) so that the exits below leave the lock alone. */
        "    cmpq $" SYSCALL_STR(SYS_CLOCK) ", %rax\n"
        "    jne 10f\n"
        "    movq $1, %gs:72\n"
        "    jmp 11f\n"
        "10:\n"
        BKL_ASM_LOCK /* from here on this processor is the one in the kernel (bkl.h); no register is touched: rax, rcx and r11 are live */
        "11:\n"
        "    movq %rsp, %gs:32\n" /* per-CPU (percpu.h): another processor may be in its own SYSCALL at the same time */
        "    movq %gs:24, %rsp\n"
        "    pushq %gs:32\n"
        "    pushq %r11\n"
        "    pushq %rcx\n"
        "    pushq %rax\n"
        "    pushq %rdi\n"
        "    pushq %rsi\n"
        "    pushq %rdx\n"
        "    pushq %r10\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    movq %rsp, %rcx\n"
        "    subq $128, %rsp\n" /* 32 shadow space + 96 for xmm0-5: the C code may change them, the program expects them back */
        "    movups %xmm0, 32(%rsp)\n"
        "    movups %xmm1, 48(%rsp)\n"
        "    movups %xmm2, 64(%rsp)\n"
        "    movups %xmm3, 80(%rsp)\n"
        "    movups %xmm4, 96(%rsp)\n"
        "    movups %xmm5, 112(%rsp)\n"
        "    cld\n"
        "    cmpq $0, %gs:72\n"
        "    jne 12f\n"
        "    call syscall_dispatch\n"
        "    jmp 13f\n"
        "12: call syscall_fast\n"
        "13:\n"
        "    movups 32(%rsp), %xmm0\n"
        "    movups 48(%rsp), %xmm1\n"
        "    movups 64(%rsp), %xmm2\n"
        "    movups 80(%rsp), %xmm3\n"
        "    movups 96(%rsp), %xmm4\n"
        "    movups 112(%rsp), %xmm5\n"
        "    addq $128, %rsp\n"
        "    movq %rax, 48(%rsp)\n" /* the result goes into the saved rax */
        "    cmpq $0, %gs:40\n"
        "    jne 4f\n"
        "    movq 56(%rsp), %rcx\n"
        "    movq %rcx, %rdx\n"
        "    sarq $47, %rdx\n"
        "    jz 1f\n"
        "    cmpq $-1, %rdx\n"
        "    jne 2f\n"
        "1:  popq %r9\n"
        "    popq %r8\n"
        "    popq %r10\n"
        "    popq %rdx\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rax\n"
        "    popq %rcx\n"
        "    popq %r11\n"
        "    popq %rsp\n" /* the user's stack again */
        "    cmpq $0, %gs:72\n"
        "    jne 14f\n"
        BKL_ASM_UNLOCK
        "    jmp 15f\n"
        "14: movq $0, %gs:72\n" /* a call that did not take the lock has nothing to give back */
        "15:\n"
        "    swapgs\n"
        "    sysretq\n"
        "2:  cmpq $0, %gs:72\n" /* a program that ends here has to be ended by a processor that holds the lock */
        "    je 16f\n"
        "    movq $0, %gs:72\n"
        BKL_ASM_LOCK
        "16: movq $-1, %rcx\n"
        "    subq $32, %rsp\n"
        "    call process_exit\n"
        "3:  hlt\n"
        "    jmp 3b\n"
        /* the return from a signal handler that interrupted the program at a timer tick: every register from the frame (rcx and r11 as the
         * program had them), then IRETQ with the address and flags signal.c chose (it checked that rip is in the user half) */
        "4:  movq $0, %gs:40\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %r10\n"
        "    popq %rdx\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rax\n"
        "    popq %rcx\n"
        "    popq %r11\n"
        "    pushq $0x1B\n" /* SS: the user's stack segment ... */
        "    pushq 8(%rsp)\n" /* ... RSP: the saved user stack pointer is where the pops stopped */
        "    pushq %gs:56\n" /* RFLAGS */
        "    pushq $0x23\n" /* CS */
        "    pushq %gs:48\n" /* RIP */
        BKL_ASM_UNLOCK
        "    swapgs\n"
        "    iretq\n");

void syscall_entry(void);

static u64 rdmsr(unsigned msr)
{
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void wrmsr(unsigned msr, u64 v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((unsigned)v), "d"((unsigned)(v >> 32)));
}

void syscall_init(void)
{
    /* SYSCALL loads CS from STAR[47:32] (SS = +8: our kernel code and data). SYSRET to 64-bit code loads SS from
     * STAR[63:48] + 8 and CS from + 16, and it loads those numbers as they are: the real CPU does NOT turn the RPL
     * bits into 3 by itself (an emulator may, which hid this once). User code would then run with CS = 0x20 / SS = 0x18,
     * RPL 0, and the first interrupt would push those; IRETQ refuses a frame whose SS and CS have different RPL
     * (#GP). So the base carries the RPL: 0x13, giving SS = 0x1B and CS = 0x23 (SEL_SYSRET_BASE in segments.h). */
    u64 star = ((u64)SEL_SYSRET_BASE << 48) | ((u64)SEL_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    wrmsr(MSR_SFMASK, SYSCALL_FLAG_MASK);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void syscall_set_kernel_stack(u64 top)
{
    this_cpu()->kstack = top;
    gdt_set_kernel_stack(top); /* the same stack for interrupts that arrive while ring 3 code runs */
}

#else

/* Host tests: no instruction, no registers; the stack is just remembered. */
static unsigned long long KSTACK;
unsigned long long syscall_iret_rip, syscall_iret_rflags; /* what the last syscall_request_iret asked for (the test looks) */
int syscall_iret_requests;
void syscall_request_iret(unsigned long long rip, unsigned long long rflags)
{
    syscall_iret_rip = rip;
    syscall_iret_rflags = rflags;
    syscall_iret_requests++;
}
void syscall_init(void) {}
void syscall_set_kernel_stack(unsigned long long top)
{
    KSTACK = top;
    gdt_set_kernel_stack(top);
}

#endif
