/* HarisOS GDT: see include/gdt.h. */
#include "gdt.h"

typedef unsigned long long u64;

void gdt_build(u64 gdt[GDT_ENTRIES], const u64 tss[2])
{
    /* Descriptor = limit 0xFFFF, base 0, access byte (bits 40-47), flags (bits 52-55). In 64-bit mode base and limit
     * are not used. Access: 0x9A / 0xFA = present, code, readable (DPL 0 / 3); 0x92 / 0xF2 = present, data,
     * writable. Flags 0xA = 64-bit code (L = 1, D = 0) + 4 KiB granularity; 0xC = 4 KiB granularity + D (data). */
    gdt[0] = 0;
    gdt[SEL_KERNEL_CODE / 8] = 0x00AF9A000000FFFFULL;
    gdt[SEL_KERNEL_DATA / 8] = 0x00CF92000000FFFFULL;
    gdt[SEL_USER_DATA / 8] = 0x00CFF2000000FFFFULL;
    gdt[SEL_USER_CODE / 8] = 0x00AFFA000000FFFFULL;
    gdt[SEL_TSS / 8] = tss[0];
    gdt[SEL_TSS / 8 + 1] = tss[1];
}

#if defined(__x86_64__) && !defined(HARIS_HOST)

#include "fault.h"
#include "percpu.h"
#include "smp.h"

typedef struct
{
    unsigned short limit;
    u64 base;
} __attribute__((packed)) DescPtr;

static u64 GDT[GDT_ENTRIES];

/* The firmware's GDT lives in memory we no longer own, so switch to ours before any interrupt can happen. */
void gdt_init(void)
{
    u64 tss[2];
    fault_tss_descriptor(tss);
    gdt_build(GDT, tss);
    gdt_load(GDT);
}

/* Load a built GDT on the calling processor: segment registers, then the task register (an application processor does this
 * with a GDT and TSS of its own, see smp.c). */
void gdt_load(u64 gdt[GDT_ENTRIES])
{
    DescPtr p = {GDT_ENTRIES * 8 - 1, (u64)gdt};
    __asm__ volatile("lgdt %0\n\t"
                     "pushq %1\n\t" /* a far return reloads CS: the only way to change it in 64-bit mode */
                     "leaq 1f(%%rip), %%rax\n\t"
                     "pushq %%rax\n\t"
                     "lretq\n"
                     "1:\n\t"
                     "movw %2, %%ax\n\t"
                     "movw %%ax, %%ds\n\t"
                     "movw %%ax, %%es\n\t"
                     "movw %%ax, %%ss\n\t"
                     "movw %3, %%ax\n\t"
                     "ltr %%ax\n\t" /* the TSS gives the double fault its own stack, and ring 3 its kernel stack */
                     :
                     : "m"(p), "i"((u64)SEL_KERNEL_CODE), "i"(SEL_KERNEL_DATA), "i"(SEL_TSS)
                     : "rax", "memory");
}

/* Every processor has its own TSS: the boot processor's is in fault.c, an application processor's in smp.c. */
void gdt_set_kernel_stack(u64 rsp0)
{
    if (percpu_index() == 0)
        fault_tss_set_rsp0(rsp0);
    else
        smp_set_kernel_stack(rsp0);
}

/* Ask the CPU what it thinks of our selectors. LAR (load access rights) sets ZF only if the selector is usable from
 * the current privilege level and its descriptor is valid; the result has the descriptor's DPL in bits 13-14 and
 * the L bit (64-bit code) in bit 21. */
static int access_rights(unsigned sel, unsigned *ar)
{
    unsigned r = 0;
    unsigned char ok;
    __asm__ volatile("lar %2, %1\n\t"
                     "setz %0"
                     : "=q"(ok), "+r"(r)
                     : "r"((unsigned)sel)
                     : "cc");
    *ar = r;
    return ok;
}

int gdt_check(void)
{
    unsigned ar;
    if (!access_rights(SEL_USER_CODE, &ar))
        return -1;
    if (((ar >> 13) & 3) != 3 || !(ar & (1u << 21)) || (ar & (1u << 22)) || !(ar & (1u << 11)))
        return -2; /* not ring 3, not 64-bit code (L=1, D=0), or not a code segment */
    if (!access_rights(SEL_USER_DATA, &ar))
        return -3;
    if (((ar >> 13) & 3) != 3 || (ar & (1u << 11)) || !(ar & (1u << 9)))
        return -4; /* not ring 3, or code instead of data, or not writable */
    unsigned short tr;
    __asm__ volatile("str %0" : "=r"(tr));
    if (tr != SEL_TSS)
        return -5;
    return 0;
}

#endif
