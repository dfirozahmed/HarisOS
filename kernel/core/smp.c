/* HarisOS SMP, stage 1: see include/smp.h. */
#include "smp.h"

typedef unsigned long long u64;
typedef unsigned char u8;

static void put16(u8 *p, unsigned v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put32(u8 *p, unsigned v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static void put64(u8 *p, u64 v)
{
    put32(p, (unsigned)v);
    put32(p + 4, (unsigned)(v >> 32));
}

int smp_pick_aps(const unsigned *apic_ids, int n, unsigned bsp_id, unsigned *out, int max)
{
    int count = 0;
    for (int i = 0; i < n && count < max; i++)
    {
        unsigned id = apic_ids[i];
        if (id == bsp_id || id > 0xFF)
            continue;
        int seen = 0;
        for (int j = 0; j < count; j++)
            if (out[j] == id)
                seen = 1;
        if (!seen)
            out[count++] = id;
    }
    return count;
}

void smp_patch_image(u8 *img, unsigned code_phys, unsigned data_phys, unsigned pml4_phys, unsigned pm32_off, unsigned lm64_off)
{
    /* The trampoline GDT. 0x08: 64-bit code (L = 1); 0x10: flat 32-bit data; 0x18: flat 32-bit code. */
    put64(img + SMP_OFF_GDT + 0, 0);
    put64(img + SMP_OFF_GDT + 8, 0x00209A0000000000ULL);
    put64(img + SMP_OFF_GDT + 16, 0x00CF92000000FFFFULL);
    put64(img + SMP_OFF_GDT + 24, 0x00CF9A000000FFFFULL);

    put16(img + SMP_OFF_GDTR, 4 * 8 - 1); /* the real mode code loads this with a 32-bit base, so the page's physical address */
    put32(img + SMP_OFF_GDTR + 2, code_phys + SMP_OFF_GDT);
    put16(img + SMP_OFF_GDTR + 6, 0);

    put32(img + SMP_OFF_PM_PTR, code_phys + pm32_off);
    put16(img + SMP_OFF_PM_PTR + 4, 0x18);
    put16(img + SMP_OFF_PM_PTR + 6, 0);

    put32(img + SMP_OFF_LM_PTR, code_phys + lm64_off);
    put16(img + SMP_OFF_LM_PTR + 4, 0x08);
    put16(img + SMP_OFF_LM_PTR + 6, 0);

    put32(img + SMP_OFF_DATA, data_phys);
    put32(img + SMP_OFF_PML4, pml4_phys);
}

void smp_build_tables(u64 *pml4, u64 *pdpt, u64 *pd, u64 pdpt_phys, u64 pd_phys)
{
    for (int i = 0; i < 512; i++)
    {
        pml4[i] = 0;
        pdpt[i] = 0;
        pd[i] = 0;
    }
    pml4[0] = pdpt_phys | 3;  /* present, writable */
    pdpt[0] = pd_phys | 3;
    pd[0] = 0x83;             /* present, writable, 2 MiB page at 0 */
}

#if defined(__x86_64__) && !defined(HARIS_HOST)

#include "acpi.h"
#include "gdt.h"
#include "idle.h"
#include "percpu.h"
#include "spinlock.h"
#include "syscall.h"
#include "timer.h"

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

#define MSR_EFER 0xC0000080u
#define EFER_LMA (1ULL << 10)

typedef struct
{
    unsigned int reserved0;
    u64 rsp[3];
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    unsigned short reserved3;
    unsigned short iomap_base;
} __attribute__((packed)) ApTss;

typedef struct
{
    PerCpu pc;               /* first: gs base of this processor (percpu.h); pc.apic_id is what the MADT says */
    u64 gdt[GDT_ENTRIES];
    ApTss tss;
    unsigned lapic_id;       /* read by the processor itself once it runs */
    volatile unsigned online;
    void (*volatile job)(void); /* set by smp_run_on, picked up by the processor within one timer tick */
    volatile int job_done;
} ApCpu;

static ApCpu CPU[SMP_MAX_CPUS];
static u8 AP_STACK[SMP_MAX_CPUS][SMP_STACK_SIZE] __attribute__((aligned(16)));
static u8 AP_IST[SMP_MAX_CPUS][4096] __attribute__((aligned(16))); /* one stack for IST1..3: nothing else runs on an AP in stage 1 */
static int TOTAL = 1;

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

static u64 rdmsr(unsigned msr)
{
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void wait_us(unsigned us)
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
    wrmsr(MSR_KERNEL_GS_BASE, 0); /* the base ring 3 starts with; SWAPGS exchanges it with the kernel's (idle.c, fault.c, syscall.c, process.c) */
}

int smp_bsp_gs_check(void) { return this_cpu() == &CPU[0].pc && this_cpu()->index == 0 ? 0 : -1; }

void smp_set_kernel_stack(u64 rsp0) { ((ApCpu *)this_cpu())->tss.rsp[0] = rsp0; } /* PerCpu is the first member of ApCpu */

static const volatile SmpParams *PARAMS; /* the BSP's settings, copied by every processor that starts */

#define MSR_PAT 0x277u

/* The first C code an application processor runs: stack and (kernel) page tables are set, nothing else is. Interrupts stay off. */
static void ap_entry(u64 index)
{
    ApCpu *c = &CPU[index];

    /* Own TSS (a TSS is "busy" once loaded and cannot be loaded by two processors) inside an own copy of the GDT. */
    c->tss.rsp[0] = (u64)(AP_STACK[index] + SMP_STACK_SIZE);
    for (int i = 0; i < 3; i++)
        c->tss.ist[i] = (u64)(AP_IST[index] + sizeof(AP_IST[index]));
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
    __atomic_store_n(&c->online, 1, __ATOMIC_RELEASE);
    for (;;)
    {
        void (*job)(void) = __atomic_load_n(&c->job, __ATOMIC_ACQUIRE);
        if (job)
        {
            c->job = 0;
            job();
            __atomic_store_n(&c->job_done, 1, __ATOMIC_RELEASE);
        }
        __asm__ volatile("sti; hlt"); /* sleep until the next tick (the handler counts it in gs:8), then look for a job again */
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

/* ---- spinlock self test: all processors fight for one lock ---- */
#define SPIN_TEST_ROUNDS 100000

static Spinlock TEST_LOCK = SPINLOCK_INIT;
static volatile u64 TEST_COUNT;
static volatile int TEST_INSIDE, TEST_OVERLAP;

static void spin_test_job(void)
{
    for (int i = 0; i < SPIN_TEST_ROUNDS; i++)
    {
        u64 f = spin_lock_irqsave(&TEST_LOCK);
        if (TEST_INSIDE)
            TEST_OVERLAP++; /* two processors inside at once: the lock does not exclude */
        TEST_INSIDE = 1;
        u64 v = TEST_COUNT; /* a read and a later write: updates are lost without the lock */
        TEST_COUNT = v + 1;
        TEST_INSIDE = 0;
        spin_unlock_irqrestore(&TEST_LOCK, f);
    }
}

int smp_selftest_spinlock(void)
{
    int n = 0;
    for (int k = 1; k < TOTAL; k++)
        if (smp_run_on(k, spin_test_job) == 0)
            n++;
    if (!n)
        return 1;
    spin_test_job(); /* the boot processor takes part */
    u64 t0 = timer_us();
    for (int k = 1; k < TOTAL; k++)
        while (CPU[k].online && !smp_job_done(k))
            if (timer_us() - t0 > 20000000)
                return -2;
            else
                __asm__ volatile("pause");
    if (TEST_OVERLAP)
        return -3;
    return TEST_COUNT == (u64)(n + 1) * SPIN_TEST_ROUNDS ? 0 : -1;
}

int smp_start(u64 code, u64 data)
{
    if (!code || !data || !idle_ready() || !timer_ready())
        return 0;

    unsigned ids[ACPI_MAX_CPUS], aps[SMP_MAX_CPUS];
    int n = acpi_cpu_count();
    for (int i = 0; i < n; i++)
        ids[i] = acpi_cpu_apic_id(i);
    unsigned bsp = idle_lapic_id();
    int count = smp_pick_aps(ids, n, bsp, aps, SMP_MAX_CPUS - 1);

    CPU[0].pc.apic_id = bsp;
    CPU[0].lapic_id = bsp;
    CPU[0].online = 1;
    TOTAL = 1;

    volatile SmpParams *p = (volatile SmpParams *)data;
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    p->entry = (u64)ap_entry;
    p->cr3 = cr3;
    p->efer = rdmsr(MSR_EFER) & ~EFER_LMA;
    u64 cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    p->cr0 = cr0 & ~((1ULL << 30) | (1ULL << 29)); /* never CD / NW: caches off */
    p->cr4 = cr4;
    p->pat = rdmsr(MSR_PAT);
    PARAMS = p;

    int up = 0;
    for (int k = 1; k <= count; k++)
    {
        CPU[k].pc.apic_id = aps[k - 1];
        CPU[k].online = 0;
        TOTAL = k + 1;
        p->stack = (u64)(AP_STACK[k] + SMP_STACK_SIZE);
        p->index = (u64)k;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        /* INIT, then SIPI twice: the second is ignored by a processor that already started on the first. */
        if (idle_send_ipi(aps[k - 1], 0x4500) != 0)
            continue;
        wait_us(10000);
        unsigned sipi = 0x4600 | (unsigned)(code >> 12);
        idle_send_ipi(aps[k - 1], sipi);
        wait_us(200);
        idle_send_ipi(aps[k - 1], sipi);

        u64 t0 = timer_us();
        while (!__atomic_load_n(&CPU[k].online, __ATOMIC_ACQUIRE) && timer_us() - t0 < 200000)
            __asm__ volatile("pause");
        if (CPU[k].online)
            up++;
    }
    if (up)
        wait_us(20000); /* long enough for the first timer ticks of the processors started last (smp_cpu_ticks) */
    return up;
}

int smp_cpu_total(void) { return TOTAL; }
int smp_cpu_online(int i) { return i >= 0 && i < TOTAL && CPU[i].online; }
unsigned smp_cpu_apic_id(int i) { return (i >= 0 && i < TOTAL) ? CPU[i].pc.apic_id : 0; }
u64 smp_cpu_ticks(int i) { return (i > 0 && i < TOTAL) ? CPU[i].pc.ticks : 0; }

#else

int smp_install(u64 code, u64 data)
{
    (void)code;
    (void)data;
    return -1;
}
int smp_start(u64 code, u64 data)
{
    (void)code;
    (void)data;
    return 0;
}
void smp_bsp_init(void) {}
int smp_bsp_gs_check(void) { return -1; }
void smp_set_kernel_stack(u64 rsp0) { (void)rsp0; }
int smp_run_on(int cpu, void (*fn)(void))
{
    (void)cpu;
    (void)fn;
    return -1;
}
int smp_job_done(int cpu)
{
    (void)cpu;
    return 0;
}
int smp_selftest_spinlock(void) { return 1; }
int smp_cpu_total(void) { return 1; }
int smp_cpu_online(int i) { return i == 0; }
unsigned smp_cpu_apic_id(int i)
{
    (void)i;
    return 0;
}
u64 smp_cpu_ticks(int i)
{
    (void)i;
    return 0;
}

#endif
