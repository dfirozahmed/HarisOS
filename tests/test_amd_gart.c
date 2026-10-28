/* Host test of the AMD Renoir GART / VMID0 bring-up (kernel/drivers/gpu/amd_native.c, compiled UNCHANGED).
 *
 * The driver runs against a simulated BAR5 register file and 512 MiB of simulated VRAM (tests/gpu_sim/amd_sim_mmio.c) in the
 * state the real HP 15s-eq1172AU (Renoir 1002:1636) shows before any write: VM L2 / context 0 off, system aperture and FB
 * range of a 512 MiB card, TLB invalidate acknowledged at once. Nothing here touches real hardware.
 *
 * Built twice by tests/run_amdgpu_tests.sh: with the page table in system RAM (what the driver does now) and with it in
 * VRAM (-DFIROZOS_GART_SYSMEM_PT=0, the older bring-up). Same checks, different place to read the table back. */
#include <stdio.h>
#include "amd_native.h"

#ifndef FIROZOS_GART_SYSMEM_PT
#define FIROZOS_GART_SYSMEM_PT 1
#endif

void sim_init(void);
void sim_fail_writes(int on);
void sim_fail_after(int successful_writes);
void sim_heap_reset(int mode);
int sim_heap_ready(void);
uint32_t gpu_mmio_read32(uint64_t base, uint32_t off);

#define MMIO 0xFE400000ULL
#define VRAM 0x10000000ULL
#define MMHUB 0x1A000u
#define GFXHUB 0x2000u
#define H(base,r) (((base)+(r)) << 2)
#define MM_CTX0 H(MMHUB,0x06C0u)
#define MM_PTLO H(MMHUB,0x072Bu)
#define MM_TLB H(MMHUB,0x0833u)
#define MM_ACK H(MMHUB,0x0706u)
#define GC_CTX0 H(GFXHUB,0x0880u)
#define GC_PTLO H(GFXHUB,0x08EBu)
#define GC_TLB H(GFXHUB,0x0987u)
#define GC_ACK H(GFXHUB,0x08C6u)
#define GART_TABLE_OFF 0x08000000u
#define PTE_VALID 1ULL
#define PTE_SYSTEM (1ULL<<1)
#define PTE_SNOOP (1ULL<<2)
#define PTE_EXEC (1ULL<<4)
#define PTE_READ (1ULL<<5)
#define PTE_WRITE (1ULL<<6)
#define PTE_MTYPE_UC (3ULL<<57)
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL
#define PTE_FLAGS (PTE_MTYPE_UC|PTE_VALID|PTE_SNOOP|PTE_EXEC|PTE_READ|PTE_WRITE)

static uint64_t read64(uint64_t base, uint32_t lo_off) {
    uint64_t lo=gpu_mmio_read32(base,lo_off);
    uint64_t hi=gpu_mmio_read32(base,lo_off+4u);
    return lo | (hi<<32);
}

#if FIROZOS_GART_SYSMEM_PT
/* Where the page table is after amd_native_gart_enable(): the address the hubs were given (PTB). */
static uint64_t pt_address(void) {
    return ((uint64_t)amd_native_gart_pt_pa_hi()<<32) | amd_native_gart_pt_pa_lo();
}
#endif

static uint64_t read_pte(uint32_t index) {
#if FIROZOS_GART_SYSMEM_PT
    const volatile uint32_t *t=(const volatile uint32_t *)(unsigned long long)pt_address();
    return (uint64_t)t[index*2u] | ((uint64_t)t[index*2u+1u]<<32);
#else
    uint64_t lo=gpu_mmio_read32(VRAM,GART_TABLE_OFF + index*8u);
    uint64_t hi=gpu_mmio_read32(VRAM,GART_TABLE_OFF + index*8u + 4u);
    return lo | (hi<<32);
#endif
}

static int check(int cond, const char *name) {
    printf("%s: %s\n", name, cond ? "PASS" : "FAIL");
    return cond;
}

int main(void) {
    int ok=1, r;
    const uint64_t fb_offset = 0x410ULL << 24;           /* the simulated MC_VM_FB_OFFSET */
    const uint64_t expected_va = 0x100000000ULL;          /* first 4 GiB boundary above 512 MiB VRAM */
    uint64_t pte, va, translated, expected_pt;

    printf("page table in %s\n", FIROZOS_GART_SYSMEM_PT ? "system RAM" : "VRAM");
#if FIROZOS_GART_SYSMEM_PT
    if (!sim_heap_ready()) {
        printf("SKIPPED: no memory arena below 4 GiB on this host\n");
        return 0;
    }
#endif

    sim_init();
    sim_heap_reset(0);
    r=amd_native_vm_probe(MMIO);
    printf("VM probe: %s\n", amd_native_vm_probe_status());
    printf("VRAM: %u MiB\n", amd_native_vram_mib());
    ok &= check(r && amd_native_vram_mib()==512, "VM probe / 512 MiB VRAM");
    if (!r) return 1;

    r=amd_native_gart_enable(MMIO, VRAM);
    printf("GART: %s\n", amd_native_gart_status());
    ok &= check(r, "GART enable");
    if (!r) return 2;

#if FIROZOS_GART_SYSMEM_PT
    expected_pt = pt_address();
    ok &= check(expected_pt!=0 && expected_pt<0x100000000ULL && (expected_pt&0xFFFULL)==0, "page table is 4 KiB aligned and below 4 GiB");
    ok &= check(read64(MMIO,MM_PTLO)==(expected_pt|PTE_VALID|PTE_SYSTEM), "MMHUB PTB readback (SYSTEM bit set)");
    ok &= check(read64(MMIO,GC_PTLO)==(expected_pt|PTE_VALID|PTE_SYSTEM), "GFXHUB PTB readback (SYSTEM bit set)");
#else
    expected_pt = fb_offset + GART_TABLE_OFF;
    ok &= check(read64(MMIO,MM_PTLO)==(expected_pt|PTE_VALID), "MMHUB PTB readback");
    ok &= check(read64(MMIO,GC_PTLO)==(expected_pt|PTE_VALID), "GFXHUB PTB readback");
#endif
    ok &= check(amd_native_gart_va()==expected_va, "GART VA = 0x100000000");
    ok &= check((gpu_mmio_read32(MMIO,MM_CTX0)&1u)!=0, "MMHUB VMID0 enabled");
    ok &= check((gpu_mmio_read32(MMIO,GC_CTX0)&1u)!=0, "GFXHUB VMID0 enabled");
    ok &= check((gpu_mmio_read32(MMIO,MM_TLB)&1u)!=0, "MMHUB L1 TLB enabled");
    ok &= check((gpu_mmio_read32(MMIO,GC_TLB)&1u)!=0, "GFXHUB L1 TLB enabled");
    ok &= check(gpu_mmio_read32(MMIO,MM_ACK)&1u, "MMHUB TLB invalidate ACK");
    ok &= check(gpu_mmio_read32(MMIO,GC_ACK)&1u, "GFXHUB TLB invalidate ACK");

    /* the CP probe PTE the driver preloads before VMID0 is enabled: GART page 0x100 -> VRAM + 1 MiB (as an MC address) */
    pte=read_pte(0x100);
    ok &= check(pte==((fb_offset+0x00100000ULL)|PTE_FLAGS), "preloaded CP probe PTE[0x100]");

    r=amd_native_gart_map(0,0x00100000ULL);
    pte=read_pte(0);
    printf("GART map[0]: %s PTE=0x%llX\n", r?"PASS":"FAIL",(unsigned long long)pte);
    ok &= check(r && pte==(0x00100000ULL|PTE_FLAGS), "PTE[0] readback");
    va=expected_va;
    translated=(pte & PTE_ADDR_MASK) | (va & 0xFFFULL);
    ok &= check(translated==0x00100000ULL, "VA[0] -> PA 0x00100000");

    r=amd_native_gart_map(1,0x00200000ULL);
    pte=read_pte(1);
    ok &= check(r && (pte & PTE_ADDR_MASK)==0x00200000ULL, "PTE[1] / VA+4K -> PA 0x00200000");
    ok &= check(!amd_native_gart_map(GART_TABLE_OFF/8u,0x00300000ULL), "Out-of-range index rejected");
    ok &= check(!amd_native_gart_map(2,0x00100123ULL), "Unaligned physical address rejected");

    /* Force the MMHUB phase to fail after the complete GFXHUB phase.
       The implementation must disable VMID0 on both hubs. */
    sim_init();
    sim_heap_reset(0);
    amd_native_vm_probe(MMIO);
    sim_fail_after(12); /* 12 successful GFXHUB writes, then MMHUB write fails */
    r=amd_native_gart_enable(MMIO,VRAM);
    printf("MMHUB injected failure: %s hub=%u step=%u\n", r?"FAIL":"PASS", amd_native_gart_fail_hub(), amd_native_gart_fail_step());
    ok &= check(!r && amd_native_gart_fail_hub()==2, "MMHUB failure detected");
    ok &= check((gpu_mmio_read32(MMIO,GC_CTX0)&1u)==0, "GFXHUB VMID0 rolled back");
    ok &= check((gpu_mmio_read32(MMIO,MM_CTX0)&1u)==0, "MMHUB VMID0 remains disabled");

    /* Original all-write failure remains covered too. */
    sim_init();
    sim_heap_reset(0);
    amd_native_vm_probe(MMIO);
    sim_fail_writes(1);
    r=amd_native_gart_enable(MMIO,VRAM);
    ok &= check(!r, "Immediate write failure detected");

#if FIROZOS_GART_SYSMEM_PT
    /* The GPU is given 32-bit addresses of heap blocks: no memory, or a block above 4 GiB, must stop the bring-up before
       any hub register is written (VMID0 stays off on both hubs). */
    for (int mode=1; mode<=2; mode++) {
        sim_init();
        sim_heap_reset(mode);
        amd_native_vm_probe(MMIO);
        r=amd_native_gart_enable(MMIO,VRAM);
        printf("GART with %s: %s\n", mode==1?"no memory":"a block above 4 GiB", amd_native_gart_status());
        ok &= check(!r, mode==1 ? "No heap memory: GART enable refused" : "Heap block above 4 GiB: GART enable refused");
        ok &= check((gpu_mmio_read32(MMIO,GC_CTX0)&1u)==0 && (gpu_mmio_read32(MMIO,MM_CTX0)&1u)==0,
                    mode==1 ? "No heap memory: both hubs untouched" : "Heap block above 4 GiB: both hubs untouched");
    }
#endif

    printf("\nAMD GART TEST %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 10;
}
