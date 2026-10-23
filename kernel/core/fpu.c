/* HarisOS floating point / SSE state: see include/fpu.h. Hardware only (FXSAVE, control registers). */
#include "fpu.h"

#if defined(__x86_64__) && !defined(HARIS_HOST)

typedef unsigned long long u64;

#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR0_TS (1ULL << 3)
#define CR0_NE (1ULL << 5)
#define CR4_OSFXSR (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_OSXSAVE (1ULL << 18)

/* The state of a CPU that has just been initialised; copied into every new process. 16-byte aligned for FXSAVE. */
static unsigned char DEFAULT_STATE[FPU_STATE_SIZE] __attribute__((aligned(16)));

void fpu_init(void)
{
    u64 cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    /* x87 and SSE usable and never "unavailable": no emulation (EM), no lazy switching (TS), numeric errors reported the
     * modern way (NE, MP); the OS supports FXSAVE (OSFXSR) and SIMD exceptions (OSXMMEXCPT). The extended state of AVX
     * (OSXSAVE) stays off: nothing here saves it, so those instructions must not run. The firmware usually did all this. */
    cr0 = (cr0 & ~(CR0_EM | CR0_TS)) | CR0_MP | CR0_NE;
    cr4 = (cr4 | CR4_OSFXSR | CR4_OSXMMEXCPT) & ~CR4_OSXSAVE;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    unsigned mxcsr = FPU_MXCSR_DEFAULT;
    __asm__ volatile("fninit" : : : "memory");
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr) : "memory");
    __asm__ volatile("fxsave %0" : "=m"(*(unsigned char(*)[FPU_STATE_SIZE])DEFAULT_STATE) : : "memory");
}

void fpu_save(void *state) { __asm__ volatile("fxsave %0" : "=m"(*(unsigned char(*)[FPU_STATE_SIZE])state) : : "memory"); }

void fpu_restore(const void *state)
{
    __asm__ volatile("fxrstor %0" : : "m"(*(const unsigned char(*)[FPU_STATE_SIZE])state) : "memory");
}

/* Back to the normal control words without touching the xmm registers (FXRSTOR would load all of them). */
void fpu_reset_control(void)
{
    unsigned mxcsr = FPU_MXCSR_DEFAULT;
    __asm__ volatile("fninit" : : : "memory");
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr) : "memory");
}

void fpu_default(void *state)
{
    volatile unsigned long long *d = (volatile unsigned long long *)state; /* no memcpy in this build */
    const unsigned long long *s = (const unsigned long long *)DEFAULT_STATE;
    for (int i = 0; i < FPU_STATE_SIZE / 8; i++)
        d[i] = s[i];
}

#else

void fpu_init(void) {}
void fpu_save(void *state) { (void)state; }
void fpu_restore(const void *state) { (void)state; }
void fpu_default(void *state) { (void)state; }
void fpu_reset_control(void) {}

#endif
