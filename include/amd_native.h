#ifndef FIROZOS_AMD_NATIVE_H
#define FIROZOS_AMD_NATIVE_H
#include "gpu_types.h"

/* Safe Renoir native-init preflight (v47 diagnostic split). No vendor MMIO writes or firmware
   execution are performed here. It verifies the handoff is sufficient for a
   later PSP implementation. */
int amd_native_preflight(uint16_t vendor, uint16_t device,
                         uint16_t subsys_vendor, uint16_t subsys_device,
                         uint8_t revision, uint64_t mmio_base);
const char *amd_native_status(void);
const char *amd_native_psp_status(void);
const char *amd_native_failed_firmware(void);
uint32_t amd_native_failed_size(void);
uint32_t amd_native_psp_sol(void);
const char *amd_native_gfx_preflight_status(void);


/* Read-only GC (graphics core) liveness probe. Never writes any register. */
int amd_native_gc_probe(uint64_t mmio_base);
const char *amd_native_gc_status(void);
uint32_t amd_native_gc_grbm_status(void);
uint32_t amd_native_gc_grbm_status2(void);
uint32_t amd_native_gc_gb_addr_config(void);
uint32_t amd_native_gc_cp_stat(void);

/* Read-only MMHUB + GFXHUB VM state and VRAM range (no register writes). */
int amd_native_vm_probe(uint64_t mmio_base);
const char *amd_native_vm_probe_status(void);
uint32_t amd_native_mmhub_ctx0_cntl(void);
uint32_t amd_native_mmhub_l2_cntl(void);
uint32_t amd_native_mmhub_pt_base_lo(void);
uint32_t amd_native_mmhub_pt_base_hi(void);
uint32_t amd_native_mmhub_sys_ap_low(void);
uint32_t amd_native_mmhub_sys_ap_high(void);
uint32_t amd_native_mmhub_fb_base(void);
uint32_t amd_native_mmhub_fb_top(void);
uint32_t amd_native_gc_fb_base(void);
uint32_t amd_native_gc_fb_top(void);
uint32_t amd_native_gc_pt_base_hi(void);
uint32_t amd_native_vram_mib(void);     /* 0 if not determined */
uint32_t amd_native_mmhub_fb_offset(void);
uint32_t amd_native_gc_fb_offset(void);

/* Opt-in GART/VMID0 bring-up (writes registers). cpu_vram_base = CPU address
   of PCI BAR0 (VRAM aperture). Returns 1 on success, 0 on abort/rollback. */
int amd_native_gart_enable(uint64_t mmio_base, uint64_t cpu_vram_base);
const char *amd_native_gart_status(void);
const char *amd_native_gart_map_status(void);
/* Map GART page index -> system physical page (4 KiB aligned). */
int amd_native_gart_map(uint32_t index, uint64_t phys);
uint64_t amd_native_gart_va(void);
uint32_t amd_native_gart_fail_hub(void);   /* 0 none, 1 GFXHUB, 2 MMHUB */
uint32_t amd_native_gart_fail_step(void);
/* GFXHUB state as of the last amd_native_vm_probe() */
uint32_t amd_native_gfxhub_ctx0(void);
uint32_t amd_native_gfxhub_l2(void);
uint32_t amd_native_gfxhub_tlb(void);
uint32_t amd_native_gfxhub_pt_lo(void);
uint32_t amd_native_gfxhub_pt_hi(void);
uint32_t amd_native_gfxhub_pt_start(void);
uint32_t amd_native_gfxhub_pt_end(void);
uint32_t amd_native_gart_pdb0_index(void);
uint32_t amd_native_gart_pdb0_lo(void);
uint32_t amd_native_gart_pdb0_hi(void);
uint32_t amd_native_gart_pdb0_entry_lo(void);
uint32_t amd_native_gart_pdb0_entry_hi(void);
uint32_t amd_native_gart_pt_pa_lo(void);
uint32_t amd_native_gart_pt_pa_hi(void);
uint32_t amd_native_mc_base_lo(void);
uint32_t amd_native_mc_base_hi(void);
uint64_t amd_native_vram_mc_addr(uint32_t offset, uint32_t bytes);
/* Read-only VM protection-fault registers (is a DMA client faulting?) */
void amd_native_fault_probe(uint64_t mmio_base);
/* Read-only RLC/CP snapshot: slot 0 before firmware load, slot 1 after */
void amd_native_gc_state_snap(uint64_t mmio_base, int slot);
int amd_native_gc_state_count(void);
const char *amd_native_gc_state_name(int i);
/* Opt-in minimal CP ring test (un-halts CE/PFP/ME, ring+writeback in VRAM). */
/* Opt-in: two ring-test variants in one boot (VRAM ring, system-RAM ring). */
int amd_native_cp_ring_matrix(uint64_t mmio_base, uint64_t cpu_vram_base, int fw_ready, int direct_ucode);
int amd_native_cp_vm_gart_test(uint64_t mmio_base, uint64_t cpu_vram_base, int fw_ready);
const char *amd_native_cp_vm_status(void);
uint32_t amd_native_cp_vm_case_value(uint32_t c, uint32_t what);
uint32_t amd_native_cp_vmid0_ctx(void);
uint32_t amd_native_cp_vmid0_pt_lo(void);
uint32_t amd_native_cp_vmid0_pt_hi(void);
uint32_t amd_native_cp_vmid1_ctx(void);
uint32_t amd_native_cp_vmid1_pt_lo(void);
uint32_t amd_native_cp_vmid1_pt_hi(void);
uint32_t amd_native_cp_vmid1_setup_pass(void);
uint32_t amd_native_cp_vmid1_old_ctx(void);
uint32_t amd_native_cp_vmid1_old_pt_lo(void);
uint32_t amd_native_cp_vmid1_old_pt_hi(void);
uint32_t amd_native_cp_vmid1_after_ctx(void);
uint32_t amd_native_cp_vmid1_after_pt_lo(void);
uint32_t amd_native_cp_vmid1_after_pt_hi(void);
uint32_t amd_native_cp_vm_value(int what); /* 8=PA low32, 9=PA high32 */
uint32_t amd_native_cp_vm_pte_addr_lo(void);
uint32_t amd_native_cp_vm_pte_addr_hi(void);
const char *amd_native_cp_ucode_status(void);
/* what: 0 RAM read before direct load, 1 after, 2 firmware file; which: 0 PFP,1 CE,2 ME; i: 0..3 */
uint32_t amd_native_cp_ucode(int what, int which, int i);
const char *amd_native_cp_matrix_status(void);
const char *amd_native_cp_var_name(int i);
int amd_native_cp_var_count(void);
/* fields: 0 ran,1 pass,2 step,3 rptr,4 wb,5 scratch,6 me_cntl,7 grbm,8 busy,
   9 stalled1,10 wptr_poll_cntl,11 doorbell_ctl,12 rb0_base,13 gc_ap_low,14 gc_ap_high,
   15 pfp_hdr,16 me_hdr,17 ce_hdr,18 stalled2,19 stalled3,20 cpf_status,21 grbm_status2 */
uint32_t amd_native_cp_var(int i, int field);
int amd_native_cp_ring_test(uint64_t mmio_base, uint64_t cpu_vram_base, int fw_ready);
/* Opt-in: stop and start the RLC like Linux gfx_v9_0_rlc_resume() (partial: no clear-state buffer). WRITES registers. */
int amd_native_rlc_start(uint64_t mmio_base);
const char *amd_native_rlc_status(void);
uint32_t amd_native_rlc_value(int i);   /* 0 CNTL before, 1 STAT before, 2 GPM before, 3 CGCG before, 4 CNTL after stop, 5 CNTL after start, 6 STAT, 7 GPM, 8 STAT +1ms, 9 GPM +1ms */
const char *amd_native_cp_status(void);
uint32_t amd_native_cp_step(void);
uint32_t amd_native_cp_scratch(void);
uint32_t amd_native_cp_me_before(void);
uint32_t amd_native_cp_me_after(void);
uint32_t amd_native_cp_rptr_reg(void);
uint32_t amd_native_cp_rptr_wb(void);
uint32_t amd_native_cp_wptr_end(void);
uint32_t amd_native_cp_grbm_after(void);
uint32_t amd_native_cp_busy_after(void);
uint32_t amd_native_gc_state_val(int slot, int i);   /* slots 0..3 */
/* Opt-in: un-halt CP front-end + MEC for ~20 ms, observe, then restore. */
int amd_native_cp_pulse_test(uint64_t mmio_base);
const char *amd_native_cp_pulse_status(void);
uint32_t amd_native_cp_pulse_scratch(void);
uint32_t amd_native_fault_mm_status(void);
uint32_t amd_native_fault_mm_lo(void);
uint32_t amd_native_fault_mm_hi(void);
uint32_t amd_native_fault_gc_status(void);
uint32_t amd_native_fault_gc_lo(void);
int amd_native_cp_ib_fence_test(uint64_t mmio);
const char *amd_native_cp_ib_status(void);
const char *amd_native_cp_ib_va_status(void);
/* V148 persistent ring API (VMID0, GART VA) */
int      amd_gpu_ring_init(uint64_t mmio);
uint32_t amd_gpu_submit_ib(uint64_t ib_va, uint32_t ib_dwords);  /* 0 = error, else fence seq */
uint32_t amd_gpu_submit_raw(const uint32_t *dw, uint32_t n);   /* packets straight into the ring + fence; 0 = error, else fence seq */
int      amd_gpu_fence_wait(uint32_t seq, uint32_t loops);       /* 1 reached, 0 timeout */
int      amd_gpu_ring_selftest(uint64_t mmio);
/* The persistent ring is up and not wedged; CPU access to VRAM (byte offset) and to the 4 KiB IB page the ring runs (for amd_3d.c) */
int      amd_gpu_ring_running(void);
void     amd_gpu_vram_write32(uint32_t off, uint32_t v);
uint32_t amd_gpu_vram_read32(uint32_t off);
void     amd_gpu_ib_write32(uint32_t dword_index, uint32_t v);
void     amd_gpu_ib_flush(void);
uint32_t amd_gpu_reg_read32(uint32_t dword_addr); /* a register of the GPU by its dword address (read only) */
void     amd_gpu_wc_flush(void);        /* mfence + a read: the CPU's write-combined VRAM writes are in memory */
void     amd_gpu_fault_snapshot(void);  /* then read amd_native_fault_gc_status() etc. */
void     amd_gpu_fault_clear(void);     /* clears the GC VM fault status (Linux gmc_v9_0_process_interrupt) */
void     amd_gpu_reg_write32(uint32_t dword_addr, uint32_t v);  /* WRITES a GPU register by its dword address */
uint64_t amd_gpu_scratch_va(void);     /* GPU (GART) address of 2 KiB of system-RAM scratch, 0 if none */
void     amd_gpu_scratch_write32(uint32_t off, uint32_t v);
uint32_t amd_gpu_scratch_read32(uint32_t off);
uint64_t amd_gpu_ib_va(void);
const char *amd_native_ring_status(void);
/* V151 GPU draw test */
int      amd_gpu_draw_test(void);
const char *amd_native_draw_status(void);
const uint32_t *amd_native_draw_buf(void);   /* 32x24 XRGB tile, NULL unless PASS */
uint32_t amd_native_draw_value(int what);
/* V152 GPU DMA fill test */
int      amd_gpu_fill_test(void);
const char *amd_native_fill_status(void);
const uint32_t *amd_native_fill_buf(void);   /* 128x128 XRGB, NULL unless PASS */
uint32_t amd_native_fill_value(int what);
/* V153 GPU DMA blit test */
int      amd_gpu_blit_test(void);
const char *amd_native_blit_status(void);
const uint32_t *amd_native_blit_buf(void);   /* 128x128 XRGB, NULL unless PASS */
uint32_t amd_native_blit_value(int what);
/* V154 GPU writes into the scanout framebuffer (call after each gfx_present) */
int      amd_gpu_fb_strip(uint64_t fb_phys, uint32_t pitch, uint32_t width, uint32_t height, volatile uint8_t *fb_cpu);
const char *amd_native_fb_status(void);
uint32_t amd_native_fb_value(int what);
/* V155 batched GPU 2D engine on the scanout framebuffer (unlocked once V154 passed) */
int      amd_gpu2d_ready(void);
int      amd_gpu2d_fill(int x, int y, int w, int h, uint32_t color);
int      amd_gpu2d_copy(int sx, int sy, int dx, int dy, int w, int h);
int      amd_gpu2d_flush(void);                 /* submit batch, wait for fence; 0 = failed (engine latched off) */
int      amd_gpu2d_bench(volatile uint8_t *fb_cpu);
uint32_t amd_gpu2d_value(int what);
/* V156 GPU present: DMA dirty spans from the system-RAM back buffer to the scanout */
int      amd_gpu2d_present_init(const void *back, uint32_t stride_px, uint32_t rows, volatile uint8_t *fb_cpu);
int      amd_gpu2d_present_ready(void);
int      amd_gpu2d_present_copy(uint32_t src_off, uint32_t dst_off, uint32_t bytes);  /* queue; flush with amd_gpu2d_flush() */
const char *amd_gpu2d_present_status(void);
uint32_t amd_gpu2d_present_value(int what);
uint32_t amd_native_ring_value(int what);
uint32_t amd_native_cp_ib_value(int what);
void amd_native_set_pci_cmd(uint32_t cmd);
void amd_native_vm_diag(uint64_t mmio);
uint32_t amd_native_vmdiag(int i);   /* V142 read-only VM diagnostics */
uint32_t amd_native_fault_gc_hi(void);  /* step number that did not stick */

#endif
