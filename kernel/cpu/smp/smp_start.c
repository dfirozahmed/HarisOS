/* SMP: the trampoline, installing it, the start of an application processor, running a job on one. */
#include "smp_internal.h"

#if defined(__x86_64__) && !defined(HARIS_HOST)

volatile int percpu_ready; /* percpu.h: set once the boot processor has a GS base */

/* ---- the trampoline: three pieces of code in three modes, in one block that is copied to a page below 1 MiB ----
 * The assembler does not know where it will run, so nothing in it refers to a label: the real mode code works relative to CS, the
 * other two get their base (EBX) from the real mode code, and the far jumps read pointers the loader patched in (smp_patch_image).
 * It has a section of its own (so that .org counts from the first byte) and is never executed there. */
__asm__(".section .smptr,\"dr\"\n"
        ".balign 16\n"
        ".globl smp_tramp_start\n"
        "smp_tramp_start:\n"
        ".code16\n"
        "    cli\n"
        "    cld\n"
        "    movw %cs, %ax\n"
        "    movw %ax, %ds\n"        /* data fields are addressed relative to this page */
        "    movzwl %ax, %ebx\n"
        "    shll $4, %ebx\n"        /* EBX = physical address of the page, kept through the mode changes */
        "    lgdtl 0x800\n"
        "    movl %cr0, %eax\n"
        "    orl $1, %eax\n"         /* protected mode on */
        "    movl %eax, %cr0\n"
        "    ljmpl *0x808\n"         /* reloads CS with the 32-bit code segment */
        ".balign 16\n"
        ".globl smp_tramp_pm32\n"
        "smp_tramp_pm32:\n"
        ".code32\n"
        "    movw $0x10, %ax\n"
        "    movw %ax, %ds\n"
        "    movw %ax, %es\n"
        "    movw %ax, %ss\n"
        "    movl $0x620, %eax\n"    /* CR4: PAE (long mode needs it) + OSFXSR + OSXMMEXCPT (the compiler may use SSE) */
        "    movl %eax, %cr4\n"
        "    movl 0x81C(%ebx), %eax\n"
        "    movl %eax, %cr3\n"      /* the trampoline's own tables: identity map of the first 2 MiB */
        "    movl 0x818(%ebx), %esi\n"
        "    movl $0xC0000080, %ecx\n"
        "    movl 32(%esi), %eax\n"  /* the BSP's EFER, so the NX bit of the kernel's page tables is understood */
        "    movl 36(%esi), %edx\n"
        "    orl $0x100, %eax\n"     /* LME: long mode enable */
        "    wrmsr\n"
        "    movl $0x80010033, %eax\n" /* CR0: PE MP ET NE WP PG; CD and NW (set by INIT: caches off!) are cleared */
        "    movl %eax, %cr0\n"      /* paging on with LME: the processor is now in long mode, still in 32-bit compatibility code */
        "    ljmpl *0x810(%ebx)\n"
        ".balign 16\n"
        ".globl smp_tramp_lm64\n"
        "smp_tramp_lm64:\n"
        ".code64\n"
        "    xorl %eax, %eax\n"
        "    movw %ax, %ds\n"
        "    movw %ax, %es\n"
        "    movw %ax, %ss\n"
        "    movl 0x818(%rbx), %esi\n" /* the parameter page (SmpParams) */
        "    movq 0(%rsi), %rsp\n"
        "    movq 8(%rsi), %rcx\n"   /* first argument of ap_entry (Microsoft x64 ABI) */
        "    movq 16(%rsi), %rdx\n"
        "    movq 24(%rsi), %rax\n"
        "    movq %rax, %cr3\n"      /* the kernel's page tables; this page stays mapped (identity) in them */
        "    xorl %ebp, %ebp\n"
        "    subq $32, %rsp\n"       /* shadow space of the call */
        "    callq *%rdx\n"
        "1:  cli\n"
        "    hlt\n"
        "    jmp 1b\n"
        ".org 0x840\n"              /* the code must end before the patched fields at 0x800: .org fails if it does not */
        ".globl smp_tramp_end\n"
        "smp_tramp_end:\n"
        ".text\n");

extern const u8 smp_tramp_start[], smp_tramp_pm32[], smp_tramp_lm64[], smp_tramp_end[];


ApCpu CPU[SMP_MAX_CPUS];
u8 AP_STACK[SMP_MAX_CPUS][SMP_STACK_SIZE] __attribute__((aligned(16)));
/* IST1 (double fault), IST2 (page fault on a stack overflow) and IST3 (NMI) each get a stack of their own, like the boot processor's (fault.c): an NMI
 * that arrives while a page fault is being reported must not land on the page fault's stack. */
static u8 AP_IST[SMP_MAX_CPUS][3][4096] __attribute__((aligned(16)));
int TOTAL = 1;

int smp_install(u64 code, u64 data)
{
    if (!code || !data || code >= 0x100000 || (code & 0xFFF) || (data & 0xFFF) || data >= 0x100000000ULL)
        return -1;
    if ((unsigned)(smp_tramp_end - smp_tramp_start) != SMP_IMAGE_SIZE)
        return -1;
    volatile u8 *dst = (volatile u8 *)code;
    for (unsigned i = 0; i < SMP_IMAGE_SIZE; i++)
        dst[i] = smp_tramp_start[i];
    smp_patch_image((u8 *)code, (unsigned)code, (unsigned)data, (unsigned)(data + 4096),
                    (unsigned)(smp_tramp_pm32 - smp_tramp_start), (unsigned)(smp_tramp_lm64 - smp_tramp_start));
    volatile u8 *params = (volatile u8 *)data;
    for (unsigned i = 0; i < 4096; i++)
        params[i] = 0;
    smp_build_tables((u64 *)(data + 4096), (u64 *)(data + 8192), (u64 *)(data + 12288), data + 8192, data + 12288);
    return 0;
}

u64 rdmsr(unsigned msr)
{
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

void wait_us(unsigned us)
{
    u64 t0 = timer_us();
    while (timer_us() - t0 < us)
        __asm__ volatile("pause");
}

static void wrmsr(unsigned msr, u64 v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((unsigned)v), "d"((unsigned)(v >> 32)));
}

#define MSR_KERNEL_GS_BASE 0xC0000102u

void smp_bsp_init(void)
{
    CPU[0].pc.index = 0;
    percpu_set(&CPU[0].pc);
    percpu_ready = 1;
    bkl_word = 1; /* the boot processor is in the kernel: it holds the big kernel lock (bkl.h) */
    wrmsr(MSR_KERNEL_GS_BASE, 0); /* the base ring 3 starts with; SWAPGS exchanges it with the kernel's (idle.c, fault.c, syscall.c, process.c) */
}

int smp_bsp_gs_check(void) { return this_cpu() == &CPU[0].pc && this_cpu()->index == 0 ? 0 : -1; }

void smp_set_kernel_stack(u64 rsp0) { ((ApCpu *)this_cpu())->tss.rsp[0] = rsp0; } /* PerCpu is the first member of ApCpu */

const volatile SmpParams *PARAMS; /* the BSP's settings, copied by every processor that starts */


/* The first C code an application processor runs: stack and (kernel) page tables are set, nothing else is. Interrupts stay off. */
void ap_entry(u64 index)
{
    ApCpu *c = &CPU[index];

    /* Own TSS (a TSS is "busy" once loaded and cannot be loaded by two processors) inside an own copy of the GDT. */
    c->tss.rsp[0] = (u64)(AP_STACK[index] + SMP_STACK_SIZE);
    for (int i = 0; i < 3; i++)
        c->tss.ist[i] = (u64)(AP_IST[index][i] + sizeof(AP_IST[index][i]));
    c->tss.iomap_base = sizeof(ApTss);
    u64 base = (u64)&c->tss, limit = sizeof(ApTss) - 1, desc[2];
    desc[0] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) | (((limit >> 16) & 0xF) << 48) |
              (((base >> 24) & 0xFF) << 56);
    desc[1] = base >> 32;
    gdt_build(c->gdt, desc);
    gdt_load(c->gdt);

    /* After the GDT: loading a segment register can reset the GS base (a null selector does on some processors). */
    c->pc.index = (unsigned)index;
    percpu_set(&c->pc);

    /* The control registers and the page attribute table are per processor. Take the boot processor's values (SMEP / SMAP, global
     * pages, SSE enable, write-combining entry), and turn on SYSCALL here too: a user thread may run on this processor one day. */
    __asm__ volatile("mov %0, %%cr0" : : "r"(PARAMS->cr0) : "memory");
    __asm__ volatile("mov %0, %%cr4" : : "r"(PARAMS->cr4) : "memory");
    wrmsr(MSR_PAT, PARAMS->pat);
    syscall_init();

    idle_ap_init(); /* the shared IDT, local APIC on, its own 1 ms timer (a handler that only counts, see idle.c) */
    c->lapic_id = idle_lapic_id();
    c->pc.idle = 1; /* this processor is not in the kernel (bkl.h): the first interrupt takes the lock for it */
    __atomic_store_n(&c->online, 1, __ATOMIC_RELEASE);

    /* The idle loop. Every round starts and ends in the IDLE state with interrupts off. Order matters: a job (smp_run_on) runs first,
     * outside the big kernel lock, because the processor that posted it may be waiting for it while it holds the lock. Then the
     * processor takes the lock to look for a user thread (task_cpu_idle runs it and returns when there is none), lets go and sleeps. */
    int idle_registered = 0;
    __asm__ volatile("cli");
    for (;;)
    {
        void (*job)(void) = __atomic_load_n(&c->job, __ATOMIC_ACQUIRE);
        if (job)
        {
            c->job = 0;
            job();
            __atomic_store_n(&c->job_done, 1, __ATOMIC_RELEASE);
        }

        /* Only a processor that has something to run takes the big kernel lock: the question is answered with the scheduler lock alone
         * (task_cpu_has_work), so idle processors do not queue up for the kernel once a millisecond. The first round always goes in: it
         * registers the idle context. */
        if (!idle_registered || task_cpu_has_work())
        {
            bkl_lock();
            c->pc.idle = 0;
            if (!idle_registered && task_register_idle() == 0)
                idle_registered = 1;
            task_cpu_idle();
            c->pc.idle = 1;
            bkl_unlock();
        }

        /* Sleep until the next tick or until somebody wakes us (smp_wake_idle). The timer tick of a sleeping processor only counts itself
         * and leaves `idle` set (idle.c). Any other interrupt that needs the kernel (the network card) finds `idle` set, takes the lock and
         * clears it: we are in the kernel again, so give the lock back the same way as above. */
        __asm__ volatile("sti; hlt; cli" : : : "memory");
        if (!c->pc.idle)
        {
            c->pc.idle = 1;
            bkl_unlock();
        }
    }
}

int smp_run_on(int cpu, void (*fn)(void))
{
    if (cpu < 1 || cpu >= TOTAL || !CPU[cpu].online || !fn)
        return -1;
    CPU[cpu].job_done = 0;
    __atomic_store_n(&CPU[cpu].job, fn, __ATOMIC_RELEASE);
    return 0;
}

int smp_job_done(int cpu) { return cpu >= 1 && cpu < TOTAL && __atomic_load_n(&CPU[cpu].job_done, __ATOMIC_ACQUIRE); }

#endif
