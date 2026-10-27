/* HarisOS idle: see include/idle.h. */
#include "idle.h"
#include "fault.h"
#include "gdt.h"
#include "io.h"
#include "timer.h"
#include "bkl.h"
#include "percpu.h"

#if defined(__x86_64__)

#define IDT_COUNT 256
#define VEC_TIMER 0x40
#define VEC_NET IDLE_VEC_NET
#define VEC_SPURIOUS 0xFF

#define MSR_APIC_BASE 0x1B
#define APIC_BASE_ENABLE (1ULL << 11)
#define APIC_BASE_X2APIC (1ULL << 10)

#define LAPIC_TPR 0x080
#define LAPIC_EOI 0x0B0
#define LAPIC_SVR 0x0F0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR 0x390
#define LAPIC_TIMER_DIV 0x3E0

#define LVT_MASKED (1U << 16)
#define LVT_PERIODIC (1U << 17)

#define IOAPIC_BASE 0xFEC00000ULL /* the usual address; the MADT could say otherwise, but this is only used to mask */

typedef struct
{
    unsigned short off_lo, sel;
    unsigned char ist, type;
    unsigned short off_mid;
    unsigned int off_hi, zero;
} __attribute__((packed)) IdtEntry;

typedef struct
{
    unsigned short limit;
    unsigned long long base;
} __attribute__((packed)) DescPtr;

static IdtEntry IDT[IDT_COUNT];

static volatile unsigned *LAPIC;
static int READY;
static unsigned PER_MS; /* LAPIC timer counts per millisecond, measured on the boot processor and reused by the others (same bus clock) */

/* The interrupt handlers are assembly: they run between any two instructions of the kernel, so they save every
 * register that the code they call (or they themselves) may change.
 *   timer     : acknowledge the tick (LAPIC EOI register), then call task_tick(), which may switch to another
 *               thread (see task.h). Saves the registers a C function may change: rax rcx rdx r8-r11 and xmm0-5
 *               (the Microsoft x64 ABI keeps the others itself, and so does the context switch).
 *   other IRQ : only the EOI (an unexpected device interrupt must not stay "in service")
 *   spurious  : return without EOI (the APIC says none is in service)
 * A CPU exception (vectors 0..31) is a bug: fault.c reports it and stops (see fault.h). */
unsigned long long idle_lapic_eoi; /* address of the LAPIC EOI register, read by the handlers */
unsigned idle_x2apic;              /* 1 = the local APIC is in x2APIC mode: registers are MSRs (0x800 + offset / 16), no MMIO, 32-bit ids */

/* Acknowledge the interrupt being served (clobbers rax, rcx, rdx; uses the local labels 1 and 2). xAPIC: write 0 to the EOI register in
 * memory. x2APIC: the same register is MSR 0x80B. */
#define IDLE_ASM_EOI                         \
    "    cmpl $0, idle_x2apic(%rip)\n"       \
    "    jne 1f\n"                           \
    "    movq idle_lapic_eoi(%rip), %rax\n"  \
    "    movl $0, (%rax)\n"                  \
    "    jmp 2f\n"                           \
    "1:  movl $0x80B, %ecx\n"                \
    "    xorl %eax, %eax\n"                  \
    "    xorl %edx, %edx\n"                  \
    "    wrmsr\n"                            \
    "2:\n"
void idle_isr_timer(void);
void idle_isr_net(void);
void idle_isr_tlb(void);
void idle_isr_other(void);
void idle_isr_spurious(void);

/* Stack alignment in idle_isr_timer: the CPU aligns RSP to 16 and pushes 5 qwords (40 bytes) -> RSP is 8 mod 16;
 * 7 pushes (56) -> 0 mod 16; then 128 bytes = 32 shadow space for the call + 96 for xmm0-5, still 0 mod 16. */
__asm__(".text\n"
        ".p2align 4\n"
        ".globl idle_isr_timer\n"
        "idle_isr_timer:\n"
        /* An application processor that sleeps in its idle loop (ring 0, idle flag set, no big kernel lock) only counts the tick and goes back
         * to sleep: it does not take the lock. Its idle loop (smp.c) looks for work with the scheduler lock alone, and only a processor that
         * finds some takes the big kernel lock. The flag stays set; the loop knows what that means. (The boot processor does not do this: its
         * main loop polls the devices after every wake-up, which needs the lock anyway.) */
        "    testb $3, 8(%rsp)\n"
        "    jnz 20f\n"
        "    cmpl $0, percpu_ready(%rip)\n"
        "    je 20f\n"
        "    cmpl $0, %gs:16\n"
        "    je 20f\n"
        "    cmpq $0, %gs:64\n"
        "    je 20f\n"
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    incq %gs:8\n"
        "    incq %gs:80\n"
        IDLE_ASM_EOI
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    iretq\n"
        "20:\n"
        /* From ring 3: the user's GS is active, swap in the kernel's (percpu.h) and take the big kernel lock. From a processor that was
         * waiting in HLT: take the lock and stay in the kernel (bkl.h). */
        BKL_ISR_ENTER_IRQ("8")
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r10\n"
        "    pushq %r11\n"
        "    subq $128, %rsp\n"
        "    movups %xmm0, 32(%rsp)\n"
        "    movups %xmm1, 48(%rsp)\n"
        "    movups %xmm2, 64(%rsp)\n"
        "    movups %xmm3, 80(%rsp)\n"
        "    movups %xmm4, 96(%rsp)\n"
        "    movups %xmm5, 112(%rsp)\n"
        "    cmpl $0, percpu_ready(%rip)\n" /* count the tick in this processor's record (percpu.h, gs:8) */
        "    je 3f\n"
        "    incq %gs:8\n"
        "3:\n"
        IDLE_ASM_EOI /* EOI first: task_tick may not come back for a long time */
        "    cld\n"
        "    call task_tick\n"
        "    leaq 128(%rsp), %rcx\n" /* the frame: r11 is the lowest word */
        "    call signal_irq\n" /* a signal handler that is due runs before the program goes on (usersig.h) */
        "    movups 32(%rsp), %xmm0\n"
        "    movups 48(%rsp), %xmm1\n"
        "    movups 64(%rsp), %xmm2\n"
        "    movups 80(%rsp), %xmm3\n"
        "    movups 96(%rsp), %xmm4\n"
        "    movups 112(%rsp), %xmm5\n"
        "    addq $128, %rsp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        BKL_ISR_EXIT("8") /* back to ring 3: let go of the lock, the user's GS again */
        "    iretq\n"
        ".p2align 4\n"
        ".globl idle_isr_net\n"
        "idle_isr_net:\n" /* the network card: net_irq_handler() (e1000.c) clears the card's cause, then the EOI. Same frame as the timer. */
        BKL_ISR_ENTER_IRQ("8")
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r10\n"
        "    pushq %r11\n"
        "    subq $128, %rsp\n"
        "    movups %xmm0, 32(%rsp)\n"
        "    movups %xmm1, 48(%rsp)\n"
        "    movups %xmm2, 64(%rsp)\n"
        "    movups %xmm3, 80(%rsp)\n"
        "    movups %xmm4, 96(%rsp)\n"
        "    movups %xmm5, 112(%rsp)\n"
        "    cld\n"
        "    call net_irq_handler\n"
        IDLE_ASM_EOI /* the line is level triggered: the EOI comes after the card let go of it */
        "    movups 32(%rsp), %xmm0\n"
        "    movups 48(%rsp), %xmm1\n"
        "    movups 64(%rsp), %xmm2\n"
        "    movups 80(%rsp), %xmm3\n"
        "    movups 96(%rsp), %xmm4\n"
        "    movups 112(%rsp), %xmm5\n"
        "    addq $128, %rsp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        BKL_ISR_EXIT("8")
        "    iretq\n"
        ".p2align 4\n"
        ".globl idle_isr_tlb\n"
        "idle_isr_tlb:\n" /* TLB flush request from another processor (smp.c). Deliberately NO big kernel lock and NO swapgs/%gs: the sender
                           * holds the lock, and this must run on a processor that sits in ring 3 (user GS active) or in HLT. */
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    movq %cr3, %rax\n"
        "    movq %rax, %cr3\n" /* drops every non-global translation */
        "    movq %cr4, %rax\n"
        "    movq %rax, %rcx\n"
        "    andq $-129, %rcx\n" /* clear CR4.PGE (bit 7) ... */
        "    movq %rcx, %cr4\n"
        "    movq %rax, %cr4\n" /* ... and set it again: that drops the global ones (kernel mappings) too */
        "    lock incq smp_tlb_acks(%rip)\n"
        "    pushq %rdx\n" /* (the x2APIC EOI is a wrmsr: ecx, edx) */
        IDLE_ASM_EOI
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    iretq\n"
        ".p2align 4\n"
        ".globl idle_isr_other\n"
        "idle_isr_other:\n"
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        IDLE_ASM_EOI
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    iretq\n"
        ".p2align 4\n"
        ".globl idle_isr_spurious\n"
        "idle_isr_spurious:\n"
        "    iretq\n");

static unsigned long long rdmsr(unsigned msr)
{
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((unsigned long long)hi << 32) | lo;
}

static void wrmsr(unsigned msr, unsigned long long v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((unsigned)v), "d"((unsigned)(v >> 32)));
}

/* A register by its xAPIC offset. In x2APIC mode the same register is the MSR 0x800 + offset / 16 (the interrupt command register is the one
 * exception, a single 64-bit MSR: idle_send_ipi). */
static unsigned lapic_read(unsigned reg) { return idle_x2apic ? (unsigned)rdmsr(0x800 + reg / 16) : LAPIC[reg / 4]; }
static void lapic_write(unsigned reg, unsigned v)
{
    if (idle_x2apic)
        wrmsr(0x800 + reg / 16, v);
    else
        LAPIC[reg / 4] = v;
}

static void idt_set(int vec, void (*handler)(void))
{
    unsigned long long a = (unsigned long long)handler;
    IDT[vec].off_lo = (unsigned short)a;
    IDT[vec].sel = SEL_KERNEL_CODE;
    IDT[vec].ist = 0;
    IDT[vec].type = 0x8E; /* present, interrupt gate (IF cleared on entry) */
    IDT[vec].off_mid = (unsigned short)(a >> 16);
    IDT[vec].off_hi = (unsigned)(a >> 32);
    IDT[vec].zero = 0;
}

static void idt_load(void)
{
    DescPtr p = {sizeof(IDT) - 1, (unsigned long long)IDT};
    __asm__ volatile("lidt %0" : : "m"(p));
}

/* Legacy interrupt sources stay quiet: every device is polled, none may interrupt us (ioapic_init unmasks a line later when a driver asks). */
static void silence_legacy_irqs(void)
{
    io_out8(0x21, 0xFF); /* 8259 PICs: mask all lines */
    io_out8(0xA1, 0xFF);

    volatile unsigned *io = (volatile unsigned *)IOAPIC_BASE; /* IOREGSEL at +0x00, IOWIN at +0x10 */
    io[0] = 1;
    unsigned ver = io[4];
    if (ver == 0xFFFFFFFFu || ver == 0)
        return; /* no IOAPIC here */
    unsigned last = (ver >> 16) & 0xFF;
    for (unsigned i = 0; i <= last; i++)
    {
        io[0] = 0x10 + 2 * i; /* low half of redirection entry i */
        io[4] = io[4] | LVT_MASKED;
    }
}

int idle_init(void)
{
    if (!timer_ready())
        return -1; /* the LAPIC timer is calibrated against the TSC clock */

    unsigned long long base = rdmsr(MSR_APIC_BASE);
    if (base & APIC_BASE_X2APIC)
        idle_x2apic = 1; /* the firmware switched to x2APIC (it must when a processor has an id above 254): MSRs instead of MMIO */
    else if (!(base & APIC_BASE_ENABLE))
        wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);
    unsigned long long addr = base & 0xFFFFFFFFFF000ULL;
    if (!addr && !idle_x2apic)
        return -1;
    LAPIC = (volatile unsigned *)addr;
    idle_lapic_eoi = addr + LAPIC_EOI;

    gdt_init(); /* our own GDT (kernel and user segments) and TSS, see gdt.h */
    for (int i = 0; i < IDT_COUNT; i++)
        idt_set(i, i < 32 ? fault_stub_table[i] : idle_isr_other);
    idt_set(VEC_TIMER, idle_isr_timer);
    idt_set(VEC_NET, idle_isr_net); /* quiet until a driver routes an I/O APIC line to it (ioapic.h) */
    idt_set(IDLE_VEC_TLB, idle_isr_tlb);
    IDT[8].ist = 1;  /* #DF runs on the TSS's IST1 stack: it may be the kernel stack that broke */
    IDT[2].ist = 3;  /* an NMI runs on IST3, never on the user's stack (the SYSCALL entry has a window where RSP is the user's) */
    IDT[14].ist = 2; /* so does #PF (IST2): a thread stack that overflowed into its guard page cannot take the frame */
    idt_set(VEC_SPURIOUS, idle_isr_spurious);
    idt_load();

    silence_legacy_irqs();

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS); /* software enable */

    /* Calibrate: let the timer count down from the maximum for 10 ms of TSC time. */
    lapic_write(LAPIC_TIMER_DIV, 0x3); /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VEC_TIMER);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
    unsigned long long t0 = timer_us();
    while (timer_us() - t0 < 10000)
        __asm__ volatile("pause");
    unsigned per_ms = (0xFFFFFFFFu - lapic_read(LAPIC_TIMER_CUR)) / 10;
    lapic_write(LAPIC_TIMER_INIT, 0);
    if (per_ms == 0)
        return -1;

    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | VEC_TIMER); /* a tick every millisecond */
    lapic_write(LAPIC_TIMER_INIT, per_ms);
    PER_MS = per_ms;
    READY = 1;
    return 0;
}

int idle_ready(void) { return READY; }

unsigned idle_lapic_id(void) { return READY ? (idle_x2apic ? lapic_read(0x20) : lapic_read(0x20) >> 24) : 0; }

int idle_is_x2apic(void) { return idle_x2apic != 0; }

int idle_send_ipi(unsigned apic_id, unsigned icr_low)
{
    if (!READY)
        return -1;
    if (idle_x2apic)
    {
        /* One 64-bit MSR: the 32-bit destination in the high half. There is no delivery status bit; the write itself is the send. */
        wrmsr(0x830, ((unsigned long long)apic_id << 32) | icr_low);
        return 0;
    }
    if (apic_id > 0xFF)
        return -1; /* xAPIC: the destination field has 8 bits; an id above that would reach another processor */
    /* bit 12 of the low half: delivery status, 1 while the previous IPI is still being sent */
    for (int i = 0; lapic_read(0x300) & (1u << 12); i++)
        if (i > 1000000)
            return -1;
    lapic_write(0x310, apic_id << 24);
    lapic_write(0x300, icr_low); /* writing the low half sends it */
    for (int i = 0; lapic_read(0x300) & (1u << 12); i++)
        if (i > 1000000)
            return -1;
    return 0;
}

void idle_ap_init(void)
{
    idt_load();
    if (idle_x2apic)
    {
        /* INIT put this processor's APIC back into xAPIC mode: switch it to x2APIC like the boot processor's before touching any register. */
        unsigned long long base = rdmsr(MSR_APIC_BASE);
        wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE | APIC_BASE_X2APIC);
    }
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS);

    /* The timer is part of each processor's own local APIC: it has to be programmed by the processor itself. */
    lapic_write(LAPIC_TIMER_DIV, 0x3); /* divide by 16, like the boot processor */
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | VEC_TIMER); /* the same handler as the boot processor's: it runs the scheduler tick */
    lapic_write(LAPIC_TIMER_INIT, PER_MS);
}

void idle_send_self_nmi(void)
{
    if (!READY)
        return;
    /* Interrupt command register: delivery mode NMI (100b at bit 8), level assert (bit 14), destination shorthand
     * "self" (01b at bit 18). Writing the low half sends it. */
    if (idle_x2apic)
    {
        wrmsr(0x830, (1u << 18) | (1u << 14) | (4u << 8));
        return;
    }
    lapic_write(0x310, 0);
    lapic_write(0x300, (1u << 18) | (1u << 14) | (4u << 8));
}

void idle_wait(void)
{
    if (!READY)
    {
        __asm__ volatile("pause");
        return;
    }
    if (!percpu_ready)
    {
        /* "sti" takes effect after the next instruction, so a tick cannot slip in between sti and hlt and be missed. */
        __asm__ volatile("sti; hlt; cli" : : : "memory");
        return;
    }
    /* With the big kernel lock (bkl.h): a processor that sleeps must not hold it, or the others could not enter the kernel. Interrupts
     * are off here. The interrupt that wakes us takes the lock again in its entry code (BKL_ISR_ENTER_IRQ), clears `idle` and leaves
     * the lock taken, because we go on in the kernel. Only a wake-up without an interrupt (an NMI, an SMI) leaves `idle` set. */
    PerCpu *c = this_cpu();
    c->idle = 1;
    bkl_unlock();
    __asm__ volatile("sti; hlt; cli" : : : "memory");
    if (c->idle)
    {
        bkl_lock();
        c->idle = 0;
    }
}

#else

int idle_init(void) { return -1; }
int idle_ready(void) { return 0; }
int idle_is_x2apic(void) { return 0; }
unsigned idle_lapic_id(void) { return 0; }
int idle_send_ipi(unsigned apic_id, unsigned icr_low)
{
    (void)apic_id;
    (void)icr_low;
    return -1;
}
void idle_ap_init(void) {}
void idle_wait(void) {}
void idle_send_self_nmi(void) {}

#endif
