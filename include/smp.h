/* ============================================================
 * HarisOS SMP, stage 1: start the other processors ("application processors", AP) and park them.
 *
 *   smp_install(code, data)   before ExitBootServices (boot/efi_entry.c): the start-up code is copied into `code`, a page below 1 MiB
 *   smp_start(code, data)     after idle_init(): wake every enabled processor the MADT lists; returns how many came up
 *   smp_cpu_total()           processors found, the boot processor (index 0) included
 *   smp_cpu_online(i)         1 when processor i is running (index 0, the boot processor, always)
 *   smp_cpu_apic_id(i)        its local APIC id
 *
 * How a processor is woken: the boot processor (BSP) sends it INIT, then twice SIPI ("start-up IPI") through the local APIC. A SIPI
 * carries one byte, the page number of the code to start at, and the processor begins there in 16-bit real mode, CS = page << 8
 * (so the code must be below 1 MiB and 4 KiB aligned). The code is a short "trampoline" that climbs to 64-bit mode on the way:
 *   real mode -> protected mode (own small GDT) -> long mode (own identity page tables for the first 2 MiB, so paging can be switched
 *   on while the code still runs below 1 MiB) -> the kernel's page tables (CR3 of the BSP) -> ap_entry() on a stack of its own.
 * ap_entry loads the processor's own GDT + TSS, sets its GS base to its per-CPU record (percpu.h), loads the shared IDT, enables its
 * local APIC with its own 1 ms timer, announces itself and sleeps in hlt. Its timer handler (idle.c) only counts ticks in the
 * record: no scheduling happens on it yet, and it never calls task_tick, so the BSP behaves exactly as on one processor.
 *
 * Memory the loader gives (efi_entry.c): `code` = 1 page (EfiLoaderCode: executable even when the firmware maps data as NX), and
 * `data` = 4 pages (EfiLoaderData): the parameter block (page 0) and the three page tables of the trampoline (pages 1..3).
 * ============================================================ */
#ifndef HARIS_SMP_H
#define HARIS_SMP_H

#define SMP_MAX_CPUS 16 /* boot processor included; more in the MADT are left asleep */
#define SMP_STACK_SIZE 16384
#define SMP_DATA_PAGES 4

/* Layout of the trampoline page. Code from offset 0, the fields the loader patches from SMP_OFF_GDTR on. */
#define SMP_OFF_GDTR 0x800   /* u16 limit, u32 base: the trampoline GDT */
#define SMP_OFF_PM_PTR 0x808 /* u32 offset, u16 selector: far jump to the 32-bit code */
#define SMP_OFF_LM_PTR 0x810 /* u32 offset, u16 selector: far jump to the 64-bit code */
#define SMP_OFF_DATA 0x818   /* u32: physical address of the parameter page */
#define SMP_OFF_PML4 0x81C   /* u32: physical address of the trampoline's PML4 */
#define SMP_OFF_GDT 0x820    /* 4 descriptors: null, 64-bit code (0x08), 32-bit data (0x10), 32-bit code (0x18) */
#define SMP_IMAGE_SIZE 0x840

/* Parameter page, written by the BSP before each SIPI (the processors are started one at a time). */
typedef struct
{
    unsigned long long stack; /* top of the stack of the processor about to start */
    unsigned long long index; /* its number (first argument of ap_entry) */
    unsigned long long entry; /* ap_entry */
    unsigned long long cr3;   /* the BSP's page tables */
    unsigned long long efer;  /* the BSP's EFER (NXE, SCE ...) without the read-only LMA bit */
    unsigned long long cr0;   /* the BSP's CR0, CR4 and PAT as they are when the processors start: protect_init, fpu_init and fbwc have */
    unsigned long long cr4;   /* run by then, so an application processor gets the same SMEP / SMAP / SSE / page attribute setup */
    unsigned long long pat;
} SmpParams;

/* ---- pure functions (host-tested) ---- */
/* The APIC ids of the processors to start: `apic_ids` (n of them, from the MADT) without the boot processor, duplicates and ids
 * above 255 (xAPIC ids are 8 bits), at most `max`. Returns how many were written to `out`. */
int smp_pick_aps(const unsigned *apic_ids, int n, unsigned bsp_id, unsigned *out, int max);
/* Write the fields of the trampoline page `img` (SMP_IMAGE_SIZE bytes, code already copied in): pm32_off / lm64_off are the offsets
 * of the 32-bit and 64-bit entry points inside the page. */
void smp_patch_image(unsigned char *img, unsigned code_phys, unsigned data_phys, unsigned pml4_phys, unsigned pm32_off,
                     unsigned lm64_off);
/* The trampoline's page tables: identity map of the first 2 MiB with one big page. Each table is 512 entries. */
void smp_build_tables(unsigned long long *pml4, unsigned long long *pdpt, unsigned long long *pd, unsigned long long pdpt_phys,
                      unsigned long long pd_phys);

/* The boot processor's own per-CPU record (percpu.h): GS base set, so this_cpu() works on it. Before smp_start, long before the first
 * user program. smp_bsp_gs_check: 0 when this_cpu() still finds that record (after ring 3 code has run), else -1. */
void smp_bsp_init(void);
int smp_bsp_gs_check(void);

/* Set the rsp0 of the calling application processor's TSS: the stack the CPU switches to when ring 3 code is interrupted (gdt.h). */
void smp_set_kernel_stack(unsigned long long rsp0);
/* Run fn on application processor `cpu` (1 ..), which picks it up within a millisecond; smp_job_done tells when it has returned. */
int smp_run_on(int cpu, void (*fn)(void));
int smp_job_done(int cpu);
/* Boot self test of the spinlock: every online processor, the boot one included, increments one counter many times under the same
 * lock. 0 = the count is exact, 1 = skipped (no application processor), negative = failed. */
int smp_selftest_spinlock(void);
/* The same for the heap and the page allocator (kmalloc / kfree / pmm_alloc_page on every processor at once, outside the big kernel lock;
 * blocks are filled with a pattern and checked before they are freed). 0 = ok, 1 = skipped, negative = failed. */
int smp_selftest_alloc(void);

/* TLB shootdown. A processor keeps translations of its own; when the kernel changes a page table entry that other processors may have
 * cached, it calls this (after the change, and after its own invlpg): every other online processor gets IDLE_VEC_TLB and reloads CR3
 * and toggles CR4.PGE (all translations, global ones too). It does not wait for them: the sender holds the big kernel lock and a
 * processor that waits for the lock has interrupts off, so waiting for an answer could deadlock. A processor in ring 3 or in HLT
 * flushes at once; one that waits for the lock flushes as soon as interrupts come back on, before it returns to ring 3. */
void smp_tlb_flush_others(void);
extern volatile unsigned long long smp_tlb_acks; /* flush requests answered so far, all processors together */
/* Boot self test: one request, every application processor must answer it. 0 = ok, 1 = skipped (no application processor), negative = failed. */
int smp_selftest_tlb(void);

int smp_install(unsigned long long code, unsigned long long data);
int smp_start(unsigned long long code, unsigned long long data);
int smp_cpu_total(void);
int smp_cpu_online(int i);
unsigned smp_cpu_apic_id(int i);
unsigned long long smp_cpu_ticks(int i); /* timer interrupts processor i (an application processor) has taken, 0 for the boot processor */

#endif
