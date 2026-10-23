/* ============================================================
 * HarisOS floating point / SSE state of a process
 *
 *   fpu_init();                 once, early: make sure x87 and SSE work, and remember what a clean state looks like
 *   fpu_save(state);            store the x87 / MMX / SSE registers (FXSAVE, 512 bytes) of what is running now
 *   fpu_restore(state);         load them again
 *   fpu_default(state);         fill `state` with the clean state a new process starts with
 *
 * A user program may use floating point and SSE freely (any code the compiler makes for doubles, memcpy loops ...). Those
 * registers - xmm0-xmm15, the x87 stack, the x87 control word, MXCSR (rounding mode, exception masks) - belong to the
 * process, not to the CPU: when the scheduler switches from one process to another it saves them in the old one's Task and
 * loads the new one's, so no process ever sees (or can corrupt) another's numbers.
 *
 * Kernel threads do not own any: the kernel's C code uses SSE for its own purposes, and expects the default control
 * words. So when the CPU goes from a process to a kernel thread the clean state is loaded.
 *
 * What is NOT covered: AVX and later (ymm / zmm registers, XSAVE). The kernel keeps CR4.OSXSAVE off, so those
 * instructions are undefined (#UD) in ring 3: programs are limited to what FXSAVE holds, which is everything a baseline
 * x86-64 compiler emits.
 *
 * Single places where the registers can be clobbered without a thread switch are handled where they happen: the timer
 * interrupt, NMI and system call entry code save xmm0-xmm5 around the C code they call (the Microsoft x64 ABI lets C code
 * change those six; it keeps xmm6-xmm15 itself).
 * ============================================================ */
#ifndef HARIS_FPU_H
#define HARIS_FPU_H

#define FPU_STATE_SIZE 512 /* an FXSAVE image; it must be 16-byte aligned */

/* The default MXCSR: all SIMD exceptions masked, round to nearest. */
#define FPU_MXCSR_DEFAULT 0x1F80

void fpu_init(void);
void fpu_save(void *state);
void fpu_restore(const void *state);
void fpu_default(void *state);
void fpu_reset_control(void); /* x87 unit and MXCSR back to the normal values, xmm registers untouched (kernel thread after a process) */

#endif
