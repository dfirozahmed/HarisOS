/* Host test for signal handler delivery (kernel/core/signal.c): the frame pushed on the user stack, the registers of the entry stub, the
 * return from the handler (SYSRET and IRETQ flavours), the checks on a frame that a program may have changed, blocking and ordering. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The MinGW <stdio.h> defines a macro SYS_OPEN of its own; the kernel call number is the one meant here. */
#undef SYS_OPEN
#include "aspace.h"
#include "usersig.h"
#include "task.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

typedef unsigned long long ull;

/* ---- the "user stack": 64 KiB of host memory at a user address ---- */
#define STACK_BASE 0x00007FFF00000000ULL
#define STACK_SIZE 0x10000ULL
static unsigned char STACK[STACK_SIZE];
static int STACK_OK = 1;
volatile int host_uaccess;

volatile unsigned char *host_user_ptr(ull addr)
{
    if (addr < STACK_BASE || addr >= STACK_BASE + STACK_SIZE)
    {
        printf("FAIL: the kernel touched user address %llx outside the test stack\n", addr);
        fails++;
        return STACK;
    }
    return STACK + (addr - STACK_BASE);
}

static AddressSpace FAKE_AS;
struct AddressSpace *task_address_space(void) { return &FAKE_AS; }
int aspace_user_ok(const AddressSpace *as, ull va, ull len, int write)
{
    (void)as;
    (void)write;
    return STACK_OK && va >= STACK_BASE && va + len <= STACK_BASE + STACK_SIZE;
}

static Process PROC;
static Process *CURRENT = &PROC;
Process *process_current(void) { return CURRENT; }

static jmp_buf EXIT_JMP;
static int EXIT_CODE;
void process_exit(int code)
{
    EXIT_CODE = code;
    longjmp(EXIT_JMP, 1);
}

static int IRET_N;
static ull IRET_RIP, IRET_RFLAGS;
void syscall_request_iret(ull rip, ull rflags)
{
    IRET_N++;
    IRET_RIP = rip;
    IRET_RFLAGS = rflags;
}

#define ENTRY 0x400000002000ULL
#define HANDLER 0x400000003000ULL
#define HANDLER2 0x400000004000ULL

static void reset(void)
{
    memset(&PROC, 0, sizeof(PROC));
    PROC.sig_entry = ENTRY;
    PROC.sig_handler[SIGINT] = HANDLER;
    PROC.sig_handler[SIGTERM] = HANDLER2;
    memset(STACK, 0xAB, sizeof(STACK));
    STACK_OK = 1;
    CURRENT = &PROC;
    IRET_N = 0;
}

/* what the kernel stack looks like at the entry of a system call: 96 bytes of xmm0-5 just below the frame */
typedef struct
{
    unsigned char xmm[96];
    SyscallFrame f;
} SysArea;

typedef struct
{
    unsigned char xmm[96];
    IrqFrame f;
} IrqArea;

static void fill_xmm(unsigned char *x, unsigned char seed)
{
    for (int i = 0; i < 96; i++)
        x[i] = (unsigned char)(seed + i);
}

static int exits_with(int code, void (*fn)(void *), void *arg)
{
    if (setjmp(EXIT_JMP) == 0)
    {
        fn(arg);
        return 0;
    }
    return EXIT_CODE == code;
}

static void call_sigreturn(void *a) { signal_sigreturn(&((SysArea *)a)->f); }

int main(void)
{
    /* what is deliverable */
    reset();
    CHECK(!signal_deliverable(&PROC) && !signal_deliverable(0));
    PROC.sig_pending = 1u << SIGINT;
    CHECK(signal_deliverable(&PROC));
    PROC.sig_blocked = 1u << SIGINT;
    CHECK(!signal_deliverable(&PROC));                        /* blocked: it waits */
    PROC.sig_blocked = 0;
    PROC.sig_handler[SIGINT] = 0;
    CHECK(!signal_deliverable(&PROC));                        /* no handler: nothing to run */
    PROC.sig_handler[SIGINT] = HANDLER;
    PROC.sig_entry = 0;
    CHECK(!signal_deliverable(&PROC));                        /* no entry stub */
    PROC.sig_entry = ENTRY;
    PROC.sig_pending = (1u << 5) | (1u << 20);                 /* signals that cannot have a handler are never deliverable */
    CHECK(!signal_deliverable(&PROC));

    /* delivery at the end of a system call */
    reset();
    PROC.sig_pending = 1u << SIGINT;
    SysArea sa;
    memset(&sa, 0, sizeof(sa));
    fill_xmm(sa.xmm, 7);
    sa.f.rax = SYS_READ;
    sa.f.rdx = 0xD1;
    sa.f.rsi = 0xE1;
    sa.f.rdi = 0xF1;
    sa.f.r8 = 0x81;
    sa.f.r9 = 0x91;
    sa.f.r10 = 0xA1;
    sa.f.rcx = 0x400000001234ULL;          /* the user rip SYSCALL saved */
    sa.f.r11 = 0x246;
    sa.f.user_rsp = STACK_BASE + 0x8008;   /* a stack in the middle: 8 mod 16 */
    unsigned char xmm_before[96];
    memcpy(xmm_before, sa.xmm, 96);
    long long ret = signal_syscall_return(&sa.f, 42);
    CHECK(ret == 42);
    CHECK(sa.f.rcx == ENTRY && sa.f.r11 == 0x202 && sa.f.rdx == HANDLER && sa.f.r8 == SIGINT);
    CHECK((sa.f.user_rsp & 15) == 8);                         /* as at the entry of a function */
    CHECK(sa.f.user_rsp < STACK_BASE + 0x8008 - 128);         /* below the red zone */
    CHECK(PROC.sig_pending == 0 && PROC.sig_blocked == (1u << SIGINT));
    CHECK(*(ull *)(STACK + (sa.f.user_rsp - STACK_BASE)) == 0); /* the stub's "return address" */
    ull frame = sa.f.user_rsp + 8;
    CHECK((frame & 15) == 0 && *(ull *)(STACK + (frame - STACK_BASE)) == 0x5349474652414D45ULL);
    CHECK(memcmp(sa.xmm, xmm_before, 96) == 0);               /* the entry area itself is untouched: the handler gets the old values */
    int untouched = 1;
    for (ull a = STACK_BASE + 0x8008 - 128; a < STACK_BASE + 0x8008 + 64; a++)
        untouched = untouched && STACK[a - STACK_BASE] == 0xAB;
    CHECK(untouched);                                         /* the red zone and what is above the stack pointer are as they were */

    /* the handler runs, then SYS_SIGRETURN: everything is back, flags are masked, the result is the one the call had */
    sa.f.rax = SYS_SIGRETURN;
    sa.f.rcx = 0xDEAD; /* what a handler and the stub may have left in registers the kernel restores */
    sa.f.rdx = 0xBEEF;
    sa.f.r8 = 0xBEEF;
    sa.f.rdi = 0x1111;
    sa.f.rsi = 0x2222;
    fill_xmm(sa.xmm, 100); /* the handler used xmm0-5 */
    long long back = signal_sigreturn(&sa.f);
    CHECK(back == 42);
    CHECK(sa.f.rcx == 0x400000001234ULL && sa.f.r11 == (0x246 & 0xCD5) + 0x202 - ((0x246 & 0xCD5) & 0x202));  /* IF kept, ZF + PF kept */
    CHECK(sa.f.user_rsp == STACK_BASE + 0x8008);
    CHECK(sa.f.rdx == 0xD1 && sa.f.rsi == 0xE1 && sa.f.rdi == 0xF1 && sa.f.r8 == 0x81 && sa.f.r9 == 0x91 && sa.f.r10 == 0xA1);
    CHECK(memcmp(sa.xmm, xmm_before, 96) == 0);               /* xmm0-5 back as the program had them */
    CHECK(PROC.sig_blocked == 0 && IRET_N == 0);              /* the signal can come again; a system call return uses SYSRET */

    /* delivery at a timer tick: the same, but the return is an IRETQ and rcx, r11 come back too */
    reset();
    PROC.sig_pending = 1u << SIGTERM;
    IrqArea ia;
    memset(&ia, 0, sizeof(ia));
    fill_xmm(ia.xmm, 33);
    ia.f.rax = 0xA0;
    ia.f.rcx = 0xC0;
    ia.f.rdx = 0xD0;
    ia.f.r8 = 0x80;
    ia.f.r9 = 0x90;
    ia.f.r10 = 0xA0A;
    ia.f.r11 = 0xB0B;
    ia.f.rip = 0x400000005678ULL;
    ia.f.cs = 0x23;
    ia.f.rflags = 0x293;
    ia.f.rsp = STACK_BASE + 0x9000;
    ia.f.ss = 0x1B;
    unsigned char ixmm[96];
    memcpy(ixmm, ia.xmm, 96);
    signal_irq(&ia.f);
    CHECK(ia.f.rip == ENTRY && ia.f.rflags == 0x202 && ia.f.rdx == HANDLER2 && ia.f.r8 == SIGTERM && ia.f.cs == 0x23 && ia.f.ss == 0x1B);
    CHECK((ia.f.rsp & 15) == 8 && ia.f.rsp < STACK_BASE + 0x9000 - 128);
    CHECK(PROC.sig_pending == 0 && PROC.sig_blocked == (1u << SIGTERM));
    SysArea ret_area;                                            /* the handler's SYS_SIGRETURN */
    memset(&ret_area, 0, sizeof(ret_area));
    ret_area.f.rax = SYS_SIGRETURN;
    ret_area.f.user_rsp = ia.f.rsp;
    ret_area.f.rdi = 0x7171; /* still what the program had: the handler keeps rdi and rsi */
    ret_area.f.rsi = 0x7272;
    fill_xmm(ret_area.xmm, 200);
    back = signal_sigreturn(&ret_area.f);
    CHECK(back == 0xA0);                                          /* rax as the program had it */
    CHECK(IRET_N == 1 && IRET_RIP == 0x400000005678ULL && IRET_RFLAGS == ((0x293 & 0xCD5) | 0x202));
    CHECK(ret_area.f.rcx == 0xC0 && ret_area.f.r11 == 0xB0B && ret_area.f.rdx == 0xD0 && ret_area.f.r8 == 0x80 && ret_area.f.r9 == 0x90 && ret_area.f.r10 == 0xA0A);
    CHECK(ret_area.f.user_rsp == STACK_BASE + 0x9000 && ret_area.f.rdi == 0x7171 && ret_area.f.rsi == 0x7272); /* rdi, rsi untouched */
    CHECK(memcmp(ret_area.xmm, ixmm, 96) == 0 && PROC.sig_blocked == 0);

    /* a timer tick in the kernel's own code (ring 0) does nothing; neither does one when nothing is pending */
    reset();
    PROC.sig_pending = 1u << SIGINT;
    memset(&ia, 0, sizeof(ia));
    ia.f.cs = 0x08;
    ia.f.rip = 0xFFFF800000001000ULL;
    ia.f.rsp = STACK_BASE + 0x9000;
    signal_irq(&ia.f);
    CHECK(ia.f.rip == 0xFFFF800000001000ULL && PROC.sig_pending == (1u << SIGINT));
    reset();
    ia.f.cs = 0x23;
    signal_irq(&ia.f);
    CHECK(ia.f.cs == 0x23 && ia.f.rip == 0xFFFF800000001000ULL);
    CURRENT = 0;
    PROC.sig_pending = 1u << SIGINT;
    signal_irq(&ia.f);                                            /* a kernel thread: no process */
    CHECK(ia.f.rip == 0xFFFF800000001000ULL);
    CHECK(signal_syscall_return(&sa.f, 5) == 5);
    CURRENT = &PROC;

    /* two signals pending: the lower number first; while its handler runs the other is blocked only if it is the same */
    reset();
    PROC.sig_pending = (1u << SIGINT) | (1u << SIGTERM);
    memset(&sa, 0, sizeof(sa));
    sa.f.user_rsp = STACK_BASE + 0x8008;
    sa.f.rcx = 0x400000001000ULL;
    signal_syscall_return(&sa.f, 0);
    CHECK(sa.f.r8 == SIGINT && PROC.sig_pending == (1u << SIGTERM) && signal_deliverable(&PROC)); /* SIGTERM is next, from inside the handler */
    ull first_rsp = sa.f.user_rsp;
    signal_syscall_return(&sa.f, 0);                              /* "the next system call" inside the handler */
    CHECK(sa.f.r8 == SIGTERM && sa.f.user_rsp < first_rsp && PROC.sig_blocked == ((1u << SIGINT) | (1u << SIGTERM)));
    CHECK(!signal_deliverable(&PROC));

    /* a stack the kernel cannot write: the process ends (139) instead of running the handler */
    reset();
    PROC.sig_pending = 1u << SIGINT;
    memset(&sa, 0, sizeof(sa));
    sa.f.user_rsp = STACK_BASE + 0x40;                            /* no room below it */
    sa.f.rcx = 0x400000001000ULL;
    if (setjmp(EXIT_JMP) == 0)
    {
        signal_syscall_return(&sa.f, 0);
        CHECK(!"delivery on a tiny stack must end the process");
    }
    CHECK(EXIT_CODE == SIGSEGV_EXIT);
    reset();
    PROC.sig_pending = 1u << SIGINT;
    STACK_OK = 0;                                                  /* the pages are not writable */
    sa.f.user_rsp = STACK_BASE + 0x8008;
    if (setjmp(EXIT_JMP) == 0)
    {
        signal_syscall_return(&sa.f, 0);
        CHECK(!"unwritable stack");
    }
    CHECK(EXIT_CODE == SIGSEGV_EXIT);
    reset();
    PROC.sig_pending = 1u << SIGINT;
    sa.f.user_rsp = 0xFFFF800000008000ULL;                         /* a kernel address as the stack */
    if (setjmp(EXIT_JMP) == 0)
    {
        signal_syscall_return(&sa.f, 0);
        CHECK(!"kernel stack pointer");
    }
    CHECK(EXIT_CODE == SIGSEGV_EXIT);

    /* a frame the program changed or made up: refused, and no registers are taken from it */
    reset();
    PROC.sig_pending = 1u << SIGINT;
    memset(&sa, 0, sizeof(sa));
    sa.f.user_rsp = STACK_BASE + 0x8008;
    sa.f.rcx = 0x400000001234ULL;
    sa.f.r11 = 0x202;
    signal_syscall_return(&sa.f, 1);
    ull stub_rsp = sa.f.user_rsp;
    ull fr = stub_rsp + 8;
    ull *w = (ull *)(STACK + (fr - STACK_BASE));
    ull saved_magic = w[0];
    /* offsets in SigFrame: magic, sig, old_blocked, from_syscall, then rax rcx rdx rdi rsi r8 r9 r10 r11 rip rflags rsp */
    enum { RIP = 4 + 9, RFLAGS = 4 + 10, RSP = 4 + 11, BLOCKED = 2 };
    SysArea bad;
    memset(&bad, 0, sizeof(bad));
    bad.f.rax = SYS_SIGRETURN;
    bad.f.user_rsp = stub_rsp;

    w[RFLAGS] = 0xFFFFFFFFFFFFFFFFULL;                             /* every flag set: IOPL, NT, VM, AC ... */
    bad.f.user_rsp = stub_rsp; /* (a return changes it: set it again for every try) */
    signal_sigreturn(&bad.f);
    CHECK(bad.f.r11 == (0xCD5 | 0x202) && (bad.f.r11 & 0x3000) == 0 && (bad.f.r11 & 0x20000) == 0 && (bad.f.r11 & 0x40000) == 0);   /* no IOPL, VM, AC */
    CHECK(PROC.sig_blocked == 0);
    w[RFLAGS] = 0x202;
    PROC.sig_blocked = 0xFF;
    w[BLOCKED] = 0xFFFFFFFFULL;                                    /* an old mask with bits that cannot be blocked */
    bad.f.user_rsp = stub_rsp;
    signal_sigreturn(&bad.f);
    CHECK(PROC.sig_blocked == (SIG_HANDLER_MASK) && (PROC.sig_blocked & (1u << SIGKILL)) == 0);
    w[BLOCKED] = 0;

    w[0] = saved_magic ^ 1;                                        /* not a frame this kernel made */
    bad.f.user_rsp = stub_rsp;
    CHECK(exits_with(SIGSEGV_EXIT, call_sigreturn, &bad));
    w[0] = saved_magic;
    w[RIP] = 0xFFFF800000001000ULL;                                /* "return" into the kernel's half */
    bad.f.user_rsp = stub_rsp;
    CHECK(exits_with(SIGSEGV_EXIT, call_sigreturn, &bad));
    w[RIP] = 0x400000001234ULL;
    w[RSP] = 0xFFFF800000008000ULL;
    bad.f.user_rsp = stub_rsp;
    CHECK(exits_with(SIGSEGV_EXIT, call_sigreturn, &bad));
    w[RSP] = STACK_BASE + 0x8008;
    bad.f.user_rsp = 0x1000;                                       /* the stub's stack pointer is not in the user half */
    CHECK(exits_with(SIGSEGV_EXIT, call_sigreturn, &bad));
    bad.f.user_rsp = STACK_BASE + STACK_SIZE - 16;                 /* the frame would run off the end of the stack */
    CHECK(exits_with(SIGSEGV_EXIT, call_sigreturn, &bad));
    bad.f.user_rsp = stub_rsp;
    CURRENT = 0;
    CHECK(signal_sigreturn(&bad.f) == -1);                         /* a kernel thread */
    CURRENT = &PROC;
    CHECK(signal_sigreturn(&bad.f) == 1);                          /* and the intact frame still works: the result was 1 */

    if (fails == 0)
        printf("signal tests passed\n");
    return fails ? 1 : 0;
}
