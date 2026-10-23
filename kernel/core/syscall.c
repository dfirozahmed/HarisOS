/* HarisOS system calls: see include/syscall.h. */
#include "syscall.h"
#include "aspace.h"
#include "gdt.h"
#include "process.h"
#include "serial.h"
#include "task.h"
#include "uaccess.h"

static unsigned long long COUNT;

unsigned long long syscall_count(void) { return COUNT; }

long long syscall_dispatch(SyscallFrame *f)
{
    COUNT++;
    switch (f->rax)
    {
    case SYS_WRITE:
    {
        unsigned long long ptr = f->rdi, len = f->rsi;
        if (len > SYS_WRITE_MAX)
            return -1;
        if (len == 0)
            return 0;
        /* The pointer comes from ring 3, so it is checked before it is used: every page of the buffer must be mapped,
         * user accessible. (Interrupts are off for the whole call, so nothing can unmap it between check and use; and
         * the process's own space is the one loaded, so the pointer can be used as it is.) */
        const AddressSpace *as = task_address_space();
        if (!as || !aspace_user_ok(as, ptr, len, 0))
            return -1;
        const volatile char *p = (const volatile char *)ptr;
        user_access_begin(); /* SMAP: the kernel opens the window for exactly the loop that reads the user's buffer */
        for (unsigned long long i = 0; i < len; i++)
        {
            if (p[i] == '\n')
                serial_putc('\r');
            serial_putc(p[i]);
        }
        user_access_end();
        return (long long)len;
    }
    case SYS_EXIT:
        process_exit((int)f->rdi); /* does not return */
    case SYS_YIELD:
        task_yield();
        return 0;
    default:
        return -1;
    }
}

#if defined(__x86_64__) && !defined(HARIS_HOST)

typedef unsigned long long u64;

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE 1ULL

/* RFLAGS bits cleared on entry: IF (no interrupts until we are back), TF, DF (the C code expects it clear), NT, AC. */
#define SYSCALL_FLAG_MASK 0x44700ULL

/* One CPU, interrupts off from SYSCALL to SYSRET: plain variables do instead of per-CPU data. */
u64 syscall_user_rsp;
u64 syscall_kstack;

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
        "    movq %rsp, syscall_user_rsp(%rip)\n"
        "    movq syscall_kstack(%rip), %rsp\n"
        "    pushq syscall_user_rsp(%rip)\n"
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
        "    subq $32, %rsp\n"
        "    cld\n"
        "    call syscall_dispatch\n"
        "    addq $32, %rsp\n"
        "    movq %rax, 48(%rsp)\n" /* the result goes into the saved rax */
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
        "    sysretq\n"
        "2:  movq $-1, %rcx\n"
        "    subq $32, %rsp\n"
        "    call process_exit\n"
        "3:  hlt\n"
        "    jmp 3b\n");

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
     * STAR[63:48] + 8 and CS from + 16: the base is the selector just below user data (segments.h). */
    u64 star = ((u64)((SEL_USER_DATA & ~7u) - 8) << 48) | ((u64)SEL_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    wrmsr(MSR_SFMASK, SYSCALL_FLAG_MASK);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}

void syscall_set_kernel_stack(u64 top)
{
    syscall_kstack = top;
    gdt_set_kernel_stack(top); /* the same stack for interrupts that arrive while ring 3 code runs */
}

#else

/* Host tests: no instruction, no registers; the stack is just remembered. */
static unsigned long long KSTACK;
void syscall_init(void) {}
void syscall_set_kernel_stack(unsigned long long top)
{
    KSTACK = top;
    gdt_set_kernel_stack(top);
}

#endif
