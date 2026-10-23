/* HarisOS user program body: checks that the floating point / SSE registers belong to the process.
 *
 * Two copies of this program (FPUA.ELF and FPUB.ELF, built with a different SEED, see fpua.c / fpub.c) run at the same time,
 * and the timer interrupt takes the CPU away from each of them many times. Each one puts its own values into everything the
 * CPU keeps for a process - xmm0-xmm15, two numbers on the x87 stack, the x87 control word and MXCSR (with a rounding mode
 * of its own) - spins for a while without calling the kernel, and then checks that every register still holds exactly what
 * it put there. If the kernel let the other copy's values leak in, or lost its own, a check fails.
 *
 * It exits with 44 if all rounds pass, otherwise with the number of the first failed check:
 *   1 = an xmm register changed   2 = the x87 stack changed   3 = MXCSR changed   4 = the x87 control word changed */
#include "sys.h"

#ifndef SEED
#error "define SEED before including fputest.h"
#endif

#define ROUNDS 6
#define SPIN 2000000ULL

/* A different 64-bit pattern for every register and every copy of the program. */
static unsigned long long pattern(int index)
{
    return 0x0123456789ABCDEFULL * (unsigned long long)(SEED) + 0x1111111111111111ULL * (unsigned long long)(index + 1);
}

typedef struct
{
    unsigned long long xmm_bad; /* bit n set: xmm n did not keep its pattern */
    long long st0, st1;         /* the two x87 stack values read back */
    unsigned mxcsr;
    unsigned short fcw;
} Result;

static void one_round(unsigned long long base, long long v1, long long v2, unsigned mxcsr_set, unsigned short fcw_set,
                      unsigned long long spin, Result *r)
{
    unsigned long long bad = 0;
    long long out0 = 0, out1 = 0;
    unsigned mxcsr_out = 0;
    unsigned short fcw_out = 0;
    /* In the assembler text, "\\k" reaches the assembler as \k, the parameter of the .irp loop (one pass per register). */
    __asm__ volatile(
        /* x87: a clean unit, our control word, two integers on the stack (fild loads a 64-bit integer exactly) */
        "fninit\n\t"
        "fldcw %[fcw_in]\n\t"
        "fildll %[v1]\n\t"
        "fildll %[v2]\n\t"
        "ldmxcsr %[mx_in]\n\t"
        /* xmm0..xmm15 = base + n in the low 64 bits */
        ".irp k,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n\t"
        "leaq \\k(%[base]), %%rax\n\t"
        "movq %%rax, %%xmm\\k\n\t"
        ".endr\n\t"
        /* spin: no system call, so the only way anything can interfere is the timer interrupt and the scheduler */
        "movq %[spin], %%rcx\n\t"
        "1: decq %%rcx\n\t"
        "jnz 1b\n\t"
        /* check xmm0..xmm15, collecting a bit mask of the ones that changed */
        "xorq %[bad], %[bad]\n\t"
        ".irp k,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n\t"
        "movq %%xmm\\k, %%rax\n\t"
        "leaq \\k(%[base]), %%rdx\n\t"
        "cmpq %%rdx, %%rax\n\t"
        "je 2f\n\t"
        "orq $(1 << \\k), %[bad]\n\t"
        "2:\n\t"
        ".endr\n\t"
        /* read back the x87 stack, MXCSR and the control word */
        "fistpll %[o0]\n\t"
        "fistpll %[o1]\n\t"
        "stmxcsr %[mx_out]\n\t"
        "fnstcw %[fcw_out]\n\t"
        : [bad] "+r"(bad), [o0] "=m"(out0), [o1] "=m"(out1), [mx_out] "=m"(mxcsr_out), [fcw_out] "=m"(fcw_out)
        : [base] "r"(base), [v1] "m"(v1), [v2] "m"(v2), [mx_in] "m"(mxcsr_set), [fcw_in] "m"(fcw_set), [spin] "r"(spin)
        : "rax", "rcx", "rdx", "cc", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
          "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "st", "st(1)");
    r->xmm_bad = bad;
    r->st0 = out0; /* the top of the stack is what was pushed last: v2 */
    r->st1 = out1;
    r->mxcsr = mxcsr_out;
    r->fcw = fcw_out;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void)
{
    int fail = 0;
    /* each copy has its own rounding mode in both control registers (bits 13-14 of MXCSR, bits 10-11 of the x87 word) */
    unsigned mxcsr_set = 0x1F80u | (((unsigned)(SEED)&3u) << 13);
    unsigned short fcw_set = (unsigned short)(0x037Fu | (((unsigned)(SEED)&3u) << 10));
    long long v1 = 0x1000000000000000LL + (long long)(SEED)*12345;
    long long v2 = 0x2000000000000000LL + (long long)(SEED)*54321;

    for (int round = 0; round < ROUNDS && !fail; round++)
    {
        Result r;
        one_round(pattern(round), v1, v2, mxcsr_set, fcw_set, SPIN, &r);
        if (r.xmm_bad)
            fail = 1;
        else if (r.st0 != v2 || r.st1 != v1)
            fail = 2;
        else if (r.mxcsr != mxcsr_set)
            fail = 3;
        else if (r.fcw != fcw_set)
            fail = 4;
    }

    print(fail ? "fpu test FAILED\n" : "fpu test: registers kept\n");
    sys_exit(fail ? fail : 44);
}
