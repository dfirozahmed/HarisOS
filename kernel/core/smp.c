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
#include "timer.h"

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
    u64 gdt[GDT_ENTRIES];
    ApTss tss;
    unsigned lapic_id;       /* read by the processor itself once it runs */
    volatile unsigned online;
    unsigned apic_id;        /* what the MADT says */
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

    idle_ap_init(); /* the shared IDT, local APIC on, no timer */
    c->lapic_id = idle_lapic_id();
    __atomic_store_n(&c->online, 1, __ATOMIC_RELEASE);
    for (;;)
        __asm__ volatile("cli; hlt");
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

    CPU[0].apic_id = bsp;
    CPU[0].lapic_id = bsp;
    CPU[0].online = 1;
    TOTAL = 1;

    volatile SmpParams *p = (volatile SmpParams *)data;
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    p->entry = (u64)ap_entry;
    p->cr3 = cr3;
    p->efer = rdmsr(MSR_EFER) & ~EFER_LMA;

    int up = 0;
    for (int k = 1; k <= count; k++)
    {
        CPU[k].apic_id = aps[k - 1];
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
    return up;
}

int smp_cpu_total(void) { return TOTAL; }
int smp_cpu_online(int i) { return i >= 0 && i < TOTAL && CPU[i].online; }
unsigned smp_cpu_apic_id(int i) { return (i >= 0 && i < TOTAL) ? CPU[i].apic_id : 0; }

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
int smp_cpu_total(void) { return 1; }
int smp_cpu_online(int i) { return i == 0; }
unsigned smp_cpu_apic_id(int i)
{
    (void)i;
    return 0;
}

#endif
