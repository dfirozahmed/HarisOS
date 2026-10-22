/* ============================================================
 * HarisOS CPU exception reporting
 *
 *   fault_init(...);        once, early: remember the framebuffer so a crash can be shown on screen
 *   idle_init() then installs fault_stub_table[0..31] into the IDT (see idle.c)
 *
 * Why: a CPU exception (vectors 0..31) is a kernel bug. Instead of freezing silently, the stub saves every
 * register, and the handler prints the vector, the error code, RIP, CR2 and all registers to the screen and
 * to the serial port COM1 (QEMU: -serial stdio), then halts.
 *
 * The text is built by fault_format() (fault_fmt.c), which has no hardware access, so it is tested on the host.
 * ============================================================ */
#ifndef HARIS_FAULT_H
#define HARIS_FAULT_H

/* What the assembly stub leaves on the stack, lowest address first. The order is fixed by the push order in
 * fault.c: the CPU pushes ss..rip (and an error code for some vectors), the stub adds the vector and the
 * general registers. */
typedef struct
{
    unsigned long long r15, r14, r13, r12, r11, r10, r9, r8;
    unsigned long long rdi, rsi, rbp, rbx, rdx, rcx, rax;
    unsigned long long vector;
    unsigned long long error; /* the CPU's error code, or 0 when this vector has none */
    unsigned long long rip, cs, rflags, rsp, ss;
} FaultFrame;

/* Vectors for which the CPU pushes an error code: 8, 10-14, 17, 21, 29, 30. */
#define FAULT_ERROR_CODE_MASK \
    ((1u << 8) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 17) | (1u << 21) | (1u << 29) | (1u << 30))

/* ---- text (fault_fmt.c, no hardware access) ------------------ */
const char *fault_name(unsigned vector);       /* "#PF PAGE FAULT" */
int fault_has_error_code(unsigned vector);
/* Multi-line report, lines separated by '\n', always NUL-terminated. Returns its length (without the NUL). */
int fault_format(char *buf, int cap, const FaultFrame *f, unsigned long long cr2);

/* ---- hardware side (fault.c) --------------------------------- */
/* Remember the framebuffer for the crash screen. red_mask_mode as in gfx_init(). Call as early as possible. */
void fault_init(void *framebuffer, unsigned width, unsigned height, unsigned pitch, unsigned red_mask_mode);

/* Entry points for IDT vectors 0..31. */
extern void (*const fault_stub_table[32])(void);

/* Fill the two 64-bit words of a TSS descriptor (system segment, 16 bytes in the GDT) for the TSS that holds the
 * double-fault stack. The caller puts them into its GDT and loads the selector with ltr. */
void fault_tss_descriptor(unsigned long long out[2]);

/* Called by the stub with the saved registers. Never returns. */
__attribute__((noreturn)) void fault_dispatch(FaultFrame *f);

#endif
