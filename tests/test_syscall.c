/* Host test for the system call dispatcher (kernel/core/syscall.c): what each call does with its arguments. The entry
 * code (SYSCALL instruction, register saving) is hardware and is only built for the kernel; the checks a call makes on
 * pointers from ring 3 are what matters most, and they are tested here. */
#include "syscall.h"
#include "aspace.h"
#include "process.h"
#include "task.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* ---- what the dispatcher talks to ---- */
static AddressSpace FAKE_AS;
static AddressSpace *CURRENT_AS = &FAKE_AS;
static int USER_OK = 1, USER_OK_CALLS;
static unsigned long long OK_VA, OK_LEN;
static int OK_WRITE = -1;
static char OUT[256];
static int OUT_N, YIELDS, EXIT_CODE = -12345;
static jmp_buf EXIT_JMP;

struct AddressSpace *task_address_space(void) { return CURRENT_AS; }

int aspace_user_ok(const AddressSpace *as, unsigned long long va, unsigned long long len, int write)
{
    CHECK(as == &FAKE_AS);
    USER_OK_CALLS++;
    OK_VA = va;
    OK_LEN = len;
    OK_WRITE = write;
    return USER_OK;
}

void serial_putc(char c)
{
    if (OUT_N < (int)sizeof(OUT) - 1)
        OUT[OUT_N++] = c;
    OUT[OUT_N] = 0;
}

void task_yield(void) { YIELDS++; }

void process_exit(int code)
{
    EXIT_CODE = code;
    longjmp(EXIT_JMP, 1); /* the real one never returns; neither does this: it jumps out of the dispatcher */
}

void gdt_set_kernel_stack(unsigned long long rsp0) { (void)rsp0; }

static long long call(unsigned long long num, unsigned long long a, unsigned long long b)
{
    SyscallFrame f;
    memset(&f, 0, sizeof(f));
    f.rax = num;
    f.rdi = a;
    f.rsi = b;
    return syscall_dispatch(&f);
}

static void reset(void)
{
    OUT_N = 0;
    OUT[0] = 0;
    USER_OK = 1;
    USER_OK_CALLS = 0;
    OK_WRITE = -1;
    CURRENT_AS = &FAKE_AS;
}

int main(void)
{
    char buf[] = "hi\nthere";

    /* the frame layout is what the assembly pushes: 10 qwords, in this order */
    CHECK(sizeof(SyscallFrame) == 80);
    CHECK(__builtin_offsetof(SyscallFrame, r9) == 0 && __builtin_offsetof(SyscallFrame, rdi) == 40);
    CHECK(__builtin_offsetof(SyscallFrame, rax) == 48 && __builtin_offsetof(SyscallFrame, rcx) == 56);
    CHECK(__builtin_offsetof(SyscallFrame, user_rsp) == 72);

    /* SYS_WRITE: the buffer is checked first (read access), then sent; a newline gets a carriage return before it */
    reset();
    unsigned long long c0 = syscall_count();
    CHECK(call(SYS_WRITE, (unsigned long long)buf, 8) == 8);
    CHECK(USER_OK_CALLS == 1 && OK_VA == (unsigned long long)buf && OK_LEN == 8 && OK_WRITE == 0);
    CHECK(strcmp(OUT, "hi\r\nthere") == 0);
    CHECK(syscall_count() == c0 + 1);

    /* a buffer the user may not read: refused, nothing sent */
    reset();
    USER_OK = 0;
    CHECK(call(SYS_WRITE, 0x1000, 16) == -1);
    CHECK(USER_OK_CALLS == 1 && OUT_N == 0);

    /* too long: refused before even looking at the pointer */
    reset();
    CHECK(call(SYS_WRITE, (unsigned long long)buf, SYS_WRITE_MAX + 1) == -1);
    CHECK(USER_OK_CALLS == 0 && OUT_N == 0);
    CHECK(call(SYS_WRITE, (unsigned long long)buf, ~0ULL) == -1);
    CHECK(USER_OK_CALLS == 0 && OUT_N == 0);

    /* the longest allowed length is accepted (the check is asked about all of it) */
    reset();
    static char big[SYS_WRITE_MAX];
    memset(big, 'x', sizeof(big));
    OUT_N = 0;
    CHECK(call(SYS_WRITE, (unsigned long long)big, SYS_WRITE_MAX) == SYS_WRITE_MAX);
    CHECK(OK_LEN == SYS_WRITE_MAX);

    /* an empty write is fine and touches nothing */
    reset();
    CHECK(call(SYS_WRITE, 0xDEADBEEF, 0) == 0);
    CHECK(USER_OK_CALLS == 0 && OUT_N == 0);

    /* a thread without a user space (a kernel thread) cannot make this call */
    reset();
    CURRENT_AS = 0;
    CHECK(call(SYS_WRITE, (unsigned long long)buf, 3) == -1);
    CHECK(OUT_N == 0);

    /* yield and unknown numbers */
    reset();
    CHECK(call(SYS_YIELD, 0, 0) == 0 && YIELDS == 1);
    CHECK(call(0, 0, 0) == -1);
    CHECK(call(99, 1, 2) == -1);
    CHECK(call(~0ULL, 0, 0) == -1);
    CHECK(call(SYS_WRITE + 1000, 0, 0) == -1);
    CHECK(YIELDS == 1 && OUT_N == 0);

    /* exit: the argument is the exit code (as an int), and the call does not return */
    reset();
    if (setjmp(EXIT_JMP) == 0)
    {
        call(SYS_EXIT, 42, 0);
        CHECK(!"SYS_EXIT returned");
    }
    CHECK(EXIT_CODE == 42);
    if (setjmp(EXIT_JMP) == 0)
        call(SYS_EXIT, (unsigned long long)(long long)-1, 0);
    CHECK(EXIT_CODE == -1);
    if (setjmp(EXIT_JMP) == 0)
        call(SYS_EXIT, 0x1FFFFFFFFULL, 0); /* only the low 32 bits are an int */
    CHECK(EXIT_CODE == -1);

    if (fails == 0)
        printf("syscall tests passed\n");
    return fails ? 1 : 0;
}
