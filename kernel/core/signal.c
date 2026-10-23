/* HarisOS signal handlers: see include/signal.h. */
#include "usersig.h"
#include "aspace.h"
#include "paging.h"
#include "task.h"
#include "uaccess.h"

typedef unsigned long long u64;

#define SIGFRAME_MAGIC 0x5349474652414D45ULL /* "SIGFRAME" */
#define XMM_BYTES 96
#define RED_ZONE 128
#define FLAGS_KEEP 0xCD5ULL /* CF PF AF ZF SF TF DF OF: what a program may set on itself */

typedef struct
{
    u64 rax, rcx, rdx, rdi, rsi, r8, r9, r10, r11, rip, rflags, rsp;
} UserRegs;

typedef struct
{
    u64 magic, sig, old_blocked, from_syscall;
    UserRegs r;
    unsigned char xmm[XMM_BYTES];
} SigFrame;

/* the lowest signal number that is pending, not blocked and has a handler, or 0 */
static int pick(const Process *p)
{
    if (!p->sig_entry)
        return 0;
    unsigned ready = p->sig_pending & ~p->sig_blocked & SIG_HANDLER_MASK;
    for (int n = 1; n < 32; n++)
        if ((ready & (1u << n)) && p->sig_handler[n])
            return n;
    return 0;
}

int signal_deliverable(const Process *p) { return p && pick(p) != 0; }

/* The xmm0-5 the entry code saved sit 96 bytes below the frame, in a system call and in the timer interrupt alike. */
static unsigned char *xmm_area(void *frame) { return (unsigned char *)frame - XMM_BYTES; }

static int in_user_half(u64 a) { return a >= PAGING_USER_BASE && a < PAGING_USER_END; }

/* A user address as a pointer. (The host test has its own memory standing in for the user stack.) */
#if defined(HARIS_HOST)
extern volatile unsigned char *host_user_ptr(unsigned long long addr);
#define UPTR(a) host_user_ptr(a)
#else
#define UPTR(a) ((volatile unsigned char *)(a))
#endif

static void put_user(u64 dst, const void *src, unsigned n)
{
    volatile unsigned char *d = UPTR(dst);
    const unsigned char *s = src;
    user_access_begin();
    for (unsigned i = 0; i < n; i++)
        d[i] = s[i];
    user_access_end();
}

static void get_user(void *dst, u64 src, unsigned n)
{
    const volatile unsigned char *s = UPTR(src);
    unsigned char *d = dst;
    user_access_begin();
    for (unsigned i = 0; i < n; i++)
        d[i] = s[i];
    user_access_end();
}

/* Pushes the SigFrame for `sig` below the red zone and returns the stack pointer the entry stub starts with, or 0 if the stack is not
 * usable (the caller ends the process). */
static u64 push_frame(Process *p, int sig, const UserRegs *r, const unsigned char *xmm, int from_syscall)
{
    const AddressSpace *as = task_address_space();
    if (!as || !in_user_half(r->rsp) || r->rsp < PAGING_USER_BASE + RED_ZONE + sizeof(SigFrame) + 64)
        return 0;
    u64 frame = (r->rsp - RED_ZONE - sizeof(SigFrame)) & ~15ULL;
    if (!in_user_half(frame - 8) || !aspace_user_ok(as, frame - 8, sizeof(SigFrame) + 8, 1))
        return 0;
    SigFrame sf;
    sf.magic = SIGFRAME_MAGIC;
    sf.sig = (u64)sig;
    sf.old_blocked = p->sig_blocked;
    sf.from_syscall = (u64)from_syscall;
    sf.r = *r;
    for (int i = 0; i < XMM_BYTES; i++)
        sf.xmm[i] = xmm[i];
    u64 zero = 0; /* the "return address" of the stub: never used */
    put_user(frame, &sf, sizeof(sf));
    put_user(frame - 8, &zero, 8);
    p->sig_pending &= ~(1u << sig);
    p->sig_blocked |= 1u << sig; /* no second run of the same handler inside the first */
    return frame - 8;
}

long long signal_syscall_return(SyscallFrame *f, long long result)
{
    Process *p = process_current();
    if (!p)
        return result;
    int sig = pick(p);
    if (!sig)
        return result;
    UserRegs r = {(u64)result, 0, f->rdx, f->rdi, f->rsi, f->r8, f->r9, f->r10, 0, f->rcx, f->r11, f->user_rsp};
    u64 rsp = push_frame(p, sig, &r, xmm_area(f), 1);
    if (!rsp)
        process_exit(SIGSEGV_EXIT);
    f->rcx = p->sig_entry; /* SYSRET goes to rcx */
    f->r11 = 0x202;
    f->user_rsp = rsp;
    f->rdx = p->sig_handler[sig];
    f->r8 = (u64)sig;
    return result;
}

void signal_irq(IrqFrame *f)
{
    if ((f->cs & 3) != 3)
        return;
    Process *p = process_current();
    if (!p)
        return;
    int sig = pick(p);
    if (!sig)
        return;
    /* rdi and rsi are not in this frame (the stub does not push them: the interrupt code keeps them, as callee-saved registers) */
    UserRegs r = {f->rax, f->rcx, f->rdx, 0, 0, f->r8, f->r9, f->r10, f->r11, f->rip, f->rflags, f->rsp};
    u64 rsp = push_frame(p, sig, &r, xmm_area(f), 0);
    if (!rsp)
        process_exit(SIGSEGV_EXIT);
    f->rip = p->sig_entry;
    f->rflags = 0x202;
    f->rsp = rsp;
    f->rdx = p->sig_handler[sig];
    f->r8 = (u64)sig;
}

long long signal_sigreturn(SyscallFrame *f)
{
    Process *p = process_current();
    const AddressSpace *as = task_address_space();
    if (!p || !as)
        return -1;
    u64 frame = f->user_rsp + 8; /* the stub's "add rsp, 40" has put rsp back where delivery left it */
    SigFrame sf;
    if (!in_user_half(frame) || !aspace_user_ok(as, frame, sizeof(sf), 0))
        process_exit(SIGSEGV_EXIT);
    get_user(&sf, frame, sizeof(sf));
    if (sf.magic != SIGFRAME_MAGIC || !in_user_half(sf.r.rip) || !in_user_half(sf.r.rsp))
        process_exit(SIGSEGV_EXIT); /* not a frame this kernel made: a program that jumps here by itself gets nothing */
    p->sig_blocked = (unsigned)sf.old_blocked & SIG_HANDLER_MASK;
    u64 flags = (sf.r.rflags & FLAGS_KEEP) | 0x202;
    unsigned char *x = xmm_area(f);
    for (int i = 0; i < XMM_BYTES; i++)
        x[i] = sf.xmm[i]; /* the system call's own exit puts these back in xmm0-5 */
    f->rdx = sf.r.rdx;
    f->r8 = sf.r.r8;
    f->r9 = sf.r.r9;
    f->r10 = sf.r.r10;
    f->user_rsp = sf.r.rsp;
    if (sf.from_syscall)
    {
        f->rdi = sf.r.rdi;
        f->rsi = sf.r.rsi;
        f->rcx = sf.r.rip; /* the normal return: SYSRET goes to rcx with the flags in r11 */
        f->r11 = flags;
    }
    else
    {
        /* rdi and rsi were not saved: they still hold what the program had (the handler keeps them) and are left alone. rcx and
         * r11 have values of their own, which SYSRET cannot give back: the return is an IRETQ. */
        f->rcx = sf.r.rcx;
        f->r11 = sf.r.r11;
        syscall_request_iret(sf.r.rip, flags);
    }
    return (long long)sf.r.rax;
}
