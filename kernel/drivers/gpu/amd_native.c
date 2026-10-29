#include "amd_native.h"
#include "gpu_config.h"
#include "amd_firmware.h"
#include "gpu_mmio.h"
#include "amd_psp.h"
#include "heap.h"

static const char *status = "not initialized";

/* AMDGPU common firmware header fields (little-endian on x86).
 * We only validate the memory image here; this does NOT verify AMD's
 * cryptographic signature. PSP remains the authority for authentication. */
static __attribute__((unused)) uint32_t fw32(const volatile uint8_t *p, uint32_t off)
{
    const volatile uint32_t *v = (const volatile uint32_t *)(p + off);
    return *v;
}

static uint32_t rd32(const volatile uint8_t *p, uint32_t off)
{
    const volatile uint32_t *v = (const volatile uint32_t *)(p + off);
    return *v;
}

static uint16_t rd16(const volatile uint8_t *p, uint32_t off)
{
    const volatile uint16_t *v = (const volatile uint16_t *)(p + off);
    return *v;
}

/* common_firmware_header, matching amdgpu_ucode.h:
 *  0  size_bytes
 *  4  header_size_bytes
 *  8  header_version (u16/u16)
 * 12  ip_version (u16/u16)
 * 16  ucode_version
 * 20  ucode_size_bytes
 * 24  ucode_array_offset_bytes
 * 28  crc32
 */
static int validate_common_header(const AmdFirmwareEntry *e)
{
    volatile uint8_t *p;
    uint32_t total, header, payload, payload_off;
    uint16_t major;

    if (!e || !e->address || e->size < 32U)
        return 0;

    p = (volatile uint8_t *)(unsigned long long)(uint32_t)e->address;
    total = rd32(p, 0);
    header = rd32(p, 4);
    major = rd16(p, 8);
    payload = rd32(p, 20);
    payload_off = rd32(p, 24);

    if (total != e->size || total < 32U)
        return 0;
    if (header < 32U || header > total)
        return 0;
    if (major == 0U || major > 4U)
        return 0;
    if (payload == 0U || payload > total)
        return 0;
    if (payload_off < header || payload_off > total)
        return 0;
    if (payload > total - payload_off)
        return 0;

    return 1;
}

static const char *failed_name = "none";
static uint32_t failed_size;
static uint32_t psp_sol_value;
static const char *gfx_preflight_status = "not run";


int amd_native_preflight(uint16_t vendor, uint16_t device,
                         uint16_t subsys_vendor, uint16_t subsys_device,
                         uint8_t revision, uint64_t mmio_base)
{
    int i;
    if (vendor != 0x1002 || device != 0x1636 ||
        subsys_vendor != 0x103C || subsys_device != 0x87CF ||
        revision != 0xC3) {
        status = "Renoir native preflight: target identity mismatch";
        return 0;
    }

    if (amd_firmware_count() != AMD_FW_MAX) {
        status = "Renoir native GPU path: not active; PSP handoff not completed";
        return 0;
    }

    for (i = 0; i < AMD_FW_MAX; ++i) {
        const AmdFirmwareEntry *e = amd_firmware_get(i);
        if (!e || !e->status || !e->address || !e->size ||
            e->size > 32U * 1024U * 1024U) {
            status = "Renoir native GPU path: not active; firmware handoff invalid";
            return 0;
        }
    }

    failed_name = "none";
    failed_size = 0;
    gfx_preflight_status = "GFX 9.3 firmware preflight: not complete";

    /* GPU-info is a GPU-info-specific container. ASD and TA are PSP-specific
       containers, so they are deliberately not parsed as common firmware.
       GFX/SDMA/VCN/DMCUB use the common AMDGPU header family. */
    for (i = 2; i <= 8; ++i) {
        const AmdFirmwareEntry *e = amd_firmware_get(i);
        if (!validate_common_header(e)) {
            failed_name = e ? e->name : "unknown";
            failed_size = e ? e->size : 0;
            gfx_preflight_status = "GFX 9.3 firmware preflight: FAILED";
            status = "Renoir native GPU path: not active; PSP firmware preflight incomplete";
            return 0;
        }
    }
    gfx_preflight_status = "GFX 9.3: PFP/ME/CE/MEC/MEC2/RLC + SDMA headers valid; PSP TMR test pending";
    for (i = 10; i <= 11; ++i) {
        const AmdFirmwareEntry *e = amd_firmware_get(i);
        if (!validate_common_header(e)) {
            failed_name = e ? e->name : "unknown";
            failed_size = e ? e->size : 0;
            gfx_preflight_status = "GFX 9.3 firmware preflight: FAILED";
            status = "Renoir native GPU path: not active; PSP firmware preflight incomplete";
            return 0;
        }
    }

    /* Read-only Renoir PSP sign-of-life probe. Linux amdgpu uses
       MP0 C2PMSG_81 as the sOS sign-of-life register on PSP v12/Renoir.
       The Renoir MP0 base is 0x16000 and C2PMSG_81 is register 0x91;
       amdgpu register offsets are DWORD based, hence BAR byte offset
       (0x16000 + 0x91) * 4 = 0x58244.
       We only READ here: no PSP command, ring setup, reset, or MMIO write. */
    psp_sol_value = 0;
    if (!gpu_mmio_can_access(mmio_base)) {
        status = "Renoir native GPU path: not active; PSP sign-of-life unavailable";
        return 1;
    }
    psp_sol_value = gpu_mmio_read32(mmio_base, 0x00058244U);
    if (psp_sol_value == 0xFFFFFFFFU) {
        status = "Renoir native GPU path: not active; PSP MMIO unavailable";
        return 1;
    }
    if (psp_sol_value == 0U) {
        status = "Renoir native GPU path: not active; PSP sOS not alive";
        return 1;
    }

    if (!amd_psp_active()) {
        status = "Renoir native GPU path: PSP sOS alive; KM ring inactive";
        return 1;
    }

    status = "Renoir native GPU path: PSP active; KM ring ready; BAR5 GFXHUB VM/GART preflight pending";
    return 1;
}

const char *amd_native_status(void) { return status; }

const char *amd_native_psp_status(void)
{
    if (psp_sol_value == 0xFFFFFFFFU)
        return "MMIO read returned 0xFFFFFFFF";
    if (psp_sol_value == 0U)
        return "sOS sign-of-life not present; PSP firmware not loaded";
    return "sOS sign-of-life detected";
}

const char *amd_native_failed_firmware(void) { return failed_name; }
uint32_t amd_native_failed_size(void) { return failed_size; }
uint32_t amd_native_psp_sol(void) { return psp_sol_value; }
const char *amd_native_gfx_preflight_status(void) { return gfx_preflight_status; }

/* ---------------------------------------------------------------------
   Read-only GC liveness probe (step 1 of VM/GART bring-up).

   Register addresses are verified against Linux:
     - renoir_ip_offset.h: GC_BASE__INST0_SEG0 = 0x2000 (DWORD index)
     - gc_9_1_offset.h:    these registers all use BASE_IDX 0
   Byte offset in the BAR5 register aperture = (0x2000 + reg) * 4.
   BAR5 on Renoir is 512 KiB, so every offset below is in range.
   Only 32-bit READS are issued here. */
#define RNO_GC_SEG0_DW   0x2000u
#define RNO_GC_BYTE(reg) ((RNO_GC_SEG0_DW + (reg)) << 2)
#define GC_GRBM_STATUS2_OFF   RNO_GC_BYTE(0x0002u) /* 0x8008 */
#define GC_GRBM_STATUS_OFF    RNO_GC_BYTE(0x0004u) /* 0x8010 */
#define GC_CP_STAT_OFF        RNO_GC_BYTE(0x01a0u) /* 0x8680 */
#define GC_GB_ADDR_CONFIG_OFF RNO_GC_BYTE(0x063eu) /* 0x98F8 */
#define BAR5_REG_APERTURE     0x00080000u

static const char *gc_status = "GC probe not run";
static uint32_t gc_grbm, gc_grbm2, gc_gbaddr, gc_cpstat;

int amd_native_gc_probe(uint64_t mmio_base)
{
    int dead;
    gc_grbm = gc_grbm2 = gc_gbaddr = gc_cpstat = 0xFFFFFFFFu;
    if (!gpu_mmio_can_access(mmio_base)) {
        gc_status = "GC probe skipped: no MMIO";
        return 0;
    }
    if (GC_GB_ADDR_CONFIG_OFF + 4u > BAR5_REG_APERTURE) {
        gc_status = "GC probe skipped: offset outside aperture";
        return 0;
    }
    gc_grbm   = gpu_mmio_read32(mmio_base, GC_GRBM_STATUS_OFF);
    gc_grbm2  = gpu_mmio_read32(mmio_base, GC_GRBM_STATUS2_OFF);
    gc_cpstat = gpu_mmio_read32(mmio_base, GC_CP_STAT_OFF);
    gc_gbaddr = gpu_mmio_read32(mmio_base, GC_GB_ADDR_CONFIG_OFF);

    dead = (gc_grbm == 0xFFFFFFFFu && gc_grbm2 == 0xFFFFFFFFu &&
            gc_cpstat == 0xFFFFFFFFu && gc_gbaddr == 0xFFFFFFFFu);
    if (dead) {
        gc_status = "GC block not responding (all reads 0xFFFFFFFF)";
        return 0;
    }
    if (gc_gbaddr == 0u && gc_grbm == 0u && gc_cpstat == 0u) {
        gc_status = "GC reads all zero: block unpowered or wrong base";
        return 0;
    }
    gc_status = (gc_grbm & 0x80000000u)
        ? "GC responding; GUI busy (UEFI scanout active)"
        : "GC responding; GFX idle";
    return 1;
}

const char *amd_native_gc_status(void) { return gc_status; }
uint32_t amd_native_gc_grbm_status(void) { return gc_grbm; }
uint32_t amd_native_gc_grbm_status2(void) { return gc_grbm2; }
uint32_t amd_native_gc_gb_addr_config(void) { return gc_gbaddr; }
uint32_t amd_native_gc_cp_stat(void) { return gc_cpstat; }

/* ---------------------------------------------------------------------
   Read-only VM / VRAM probe (step 2).

   Offsets verified against the uploaded Linux headers:
     gc_9_1_offset.h       (GC,    BASE_IDX 0 -> 0x2000  dwords)
     mmhub_1_0_offset.h    (MMHUB, BASE_IDX 0 -> 0x1A000 dwords, from
                            renoir_ip_offset.h MMHUB_BASE seg0)
   Byte offset = (hub_base + reg) * 4, all inside the 512 KiB BAR5 window.
   Only reads. */
#define RNO_MMHUB_SEG0_DW    0x1A000u
#define MMHUB_BYTE(reg)      ((RNO_MMHUB_SEG0_DW + (reg)) << 2)

#define MM_VM_L2_CNTL            0x0680u
#define MM_VM_CONTEXT0_CNTL      0x06C0u
#define MM_PT_BASE_LO32          0x072Bu
#define MM_PT_BASE_HI32          0x072Cu
#define MM_FB_LOCATION_BASE      0x082Cu
#define MM_FB_LOCATION_TOP       0x082Du
#define MM_SYS_AP_LOW            0x0831u
#define MM_SYS_AP_HIGH           0x0832u

#define GC_FB_LOCATION_BASE_R    0x0980u
#define GC_FB_LOCATION_TOP_R     0x0981u
#define GC_PT_BASE_HI32_R        0x08ECu
#define MM_FB_OFFSET_R           0x0817u
#define GC_FB_OFFSET_R           0x096Bu

static int gart_on_flag;   /* set once FirozOS itself enabled GART */
static uint32_t gcn_ctx0, gcn_l2, gcn_tlb, gcn_ptlo, gcn_pthi, gcn_ptstart, gcn_ptend;
static const char *vmp_status = "VM probe not run";
static uint32_t mm_ctx0, mm_l2, mm_ptlo, mm_pthi, mm_aplo, mm_aphi;
static uint32_t mm_fbbase, mm_fbtop, gc_fbbase, gc_fbtop, gc_pthi;
static uint32_t vram_mib;
static uint32_t mm_fboff, gc_fboff;

int amd_native_vm_probe(uint64_t mmio_base)
{
    uint64_t base, top, size;
    mm_ctx0 = mm_l2 = mm_ptlo = mm_pthi = mm_aplo = mm_aphi = 0xFFFFFFFFu;
    mm_fbbase = mm_fbtop = gc_fbbase = gc_fbtop = gc_pthi = 0xFFFFFFFFu;
    vram_mib = 0;
    mm_fboff = gc_fboff = 0xFFFFFFFFu;

    if (!gpu_mmio_can_access(mmio_base)) {
        vmp_status = "VM probe skipped: no MMIO";
        return 0;
    }

    mm_l2    = gpu_mmio_read32(mmio_base, MMHUB_BYTE(MM_VM_L2_CNTL));
    mm_ctx0  = gpu_mmio_read32(mmio_base, MMHUB_BYTE(MM_VM_CONTEXT0_CNTL));
    mm_ptlo  = gpu_mmio_read32(mmio_base, MMHUB_BYTE(MM_PT_BASE_LO32));
    mm_pthi  = gpu_mmio_read32(mmio_base, MMHUB_BYTE(MM_PT_BASE_HI32));
    mm_aplo  = gpu_mmio_read32(mmio_base, MMHUB_BYTE(MM_SYS_AP_LOW));
    mm_aphi  = gpu_mmio_read32(mmio_base, MMHUB_BYTE(MM_SYS_AP_HIGH));
    mm_fbbase= gpu_mmio_read32(mmio_base, MMHUB_BYTE(MM_FB_LOCATION_BASE));
    mm_fbtop = gpu_mmio_read32(mmio_base, MMHUB_BYTE(MM_FB_LOCATION_TOP));
    gc_fbbase= gpu_mmio_read32(mmio_base, RNO_GC_BYTE(GC_FB_LOCATION_BASE_R));
    gc_fbtop = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(GC_FB_LOCATION_TOP_R));
    gc_pthi  = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(GC_PT_BASE_HI32_R));
    gcn_ctx0 = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(0x0880u));
    gcn_l2   = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(0x0840u));
    gcn_tlb  = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(0x0987u));
    gcn_ptlo = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(0x08EBu));
    gcn_pthi = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(0x08ECu));
    gcn_ptstart = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(0x090Bu));
    gcn_ptend = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(0x092Bu));
    mm_fboff = gpu_mmio_read32(mmio_base, MMHUB_BYTE(MM_FB_OFFSET_R));
    gc_fboff = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(GC_FB_OFFSET_R));

    if (mm_l2 == 0xFFFFFFFFu && mm_ctx0 == 0xFFFFFFFFu &&
        mm_fbbase == 0xFFFFFFFFu && mm_fbtop == 0xFFFFFFFFu) {
        vmp_status = "MMHUB not responding (all reads 0xFFFFFFFF)";
        return 0;
    }

    /* Linux mmhub_v1_0_get_fb_location(): base/top are in 16 MiB units. */
    if (mm_fbbase != 0xFFFFFFFFu && mm_fbtop != 0xFFFFFFFFu) {
        base = (uint64_t)(mm_fbbase & 0x00FFFFFFu) << 24;
        top  = ((uint64_t)(mm_fbtop & 0x00FFFFFFu) << 24) | 0x00FFFFFFULL;
        if (top > base) {
            size = top - base + 1ULL;
            /* Sanity: 64 MiB .. 16 GiB, otherwise do not trust it. */
            if (size >= (64ULL << 20) && size <= (16ULL << 30))
                vram_mib = (uint32_t)(size >> 20);
        }
    }

    if ((mm_ctx0 & 1u) && gart_on_flag)
        vmp_status = "MMHUB VMID0 enabled by FirozOS GART";
    else if (mm_ctx0 & 1u)
        vmp_status = "MMHUB VMID0 enabled (not by FirozOS)";
    else
        vmp_status = "MMHUB responding; VMID0 disabled";
    return 1;
}

const char *amd_native_vm_probe_status(void) { return vmp_status; }
uint32_t amd_native_mmhub_ctx0_cntl(void)  { return mm_ctx0; }
uint32_t amd_native_mmhub_l2_cntl(void)    { return mm_l2; }
uint32_t amd_native_mmhub_pt_base_lo(void) { return mm_ptlo; }
uint32_t amd_native_mmhub_pt_base_hi(void) { return mm_pthi; }
uint32_t amd_native_mmhub_sys_ap_low(void) { return mm_aplo; }
uint32_t amd_native_mmhub_sys_ap_high(void){ return mm_aphi; }
uint32_t amd_native_mmhub_fb_base(void)    { return mm_fbbase; }
uint32_t amd_native_mmhub_fb_top(void)     { return mm_fbtop; }
uint32_t amd_native_gc_fb_base(void)       { return gc_fbbase; }
uint32_t amd_native_gc_fb_top(void)        { return gc_fbtop; }
uint32_t amd_native_gc_pt_base_hi(void)    { return gc_pthi; }
uint32_t amd_native_vram_mib(void)         { return vram_mib; }
uint32_t amd_native_mmhub_fb_offset(void) { return mm_fboff; }
uint32_t amd_native_gc_fb_offset(void)    { return gc_fboff; }

/* =====================================================================
   OPT-IN: GART / VMID0 bring-up  (follows Linux gfxhub_v1_0 / mmhub_v1_0)

   WRITES GPU REGISTERS. Compiled in always, but only called when
   FIROZOS_GART_ENABLE is 1 (see gpu.h). NOT tested on hardware.

   Field positions come from the uploaded gc_9_1_sh_mask.h /
   mmhub_1_0_sh_mask.h. Register offsets come from the *_offset.h files.
   Things taken from Linux source knowledge (NOT from the uploaded headers):
     - PTE bits: VALID=1<<0, SYSTEM=1<<1, SNOOPED=1<<2, EXEC=1<<4,
       READ=1<<5, WRITE=1<<6
     - GART VA aligned to 4 GiB after VRAM end
     - APU VRAM physical address = (MC_VM_FB_OFFSET << 24) + VRAM offset
     - invalidate engine 17 is the CPU/driver engine
   Safety:
     * refuses to run if the system aperture does not cover VRAM
       (the UEFI scanout would otherwise be translated)
     * every write is read back and compared; on mismatch it stops, and if
       VMID0 was already enabled it is switched off again.
   ===================================================================== */
#if FIROZ_GPU_PRESENT
#define GART_PAGES      8192u                     /* 32 MiB window: room for the mapped back buffer */
#else
#define GART_PAGES      1024u                     /* 4 MiB window (original size) */
#endif
#define GART_TABLE_VRAM_OFF 0x08000000u           /* 128 MiB into VRAM */
#define GART_PDB0_VRAM_OFF   0x07F00000u           /* 127 MiB: 4 KiB PDB0 */

#define PTE_VALID  (1ULL << 0)
#define PTE_SYSTEM (1ULL << 1)
#define PTE_SNOOP  (1ULL << 2)
#define PTE_EXEC   (1ULL << 4)
#define PTE_READ   (1ULL << 5)
#define PTE_WRITE  (1ULL << 6)
/* V145: GFX9 PTE MTYPE field, bits 58:57 (NC=0 RW=1 CC=2 UC=3).  Linux sets
   UC in the GFXHUB L1 TLB for the aperture path, so the proven physical
   writes bypass the GPU L2.  Our PTEs left MTYPE=0 (NC, cached in GL2):
   the CP consumed the packet (rptr==wptr) yet the data never reached
   memory within the poll window.  UC forces write-through. */
#define PTE_MTYPE_UC (3ULL << 57)
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL

typedef struct {
    uint32_t base_dw;      /* hub base (dword index) */
    uint32_t l2_cntl, l2_cntl2, l2_cntl3, l2_cntl4;
    uint32_t ctx0_cntl, pt_base_lo, pt_start_lo, pt_end_lo;
    uint32_t tlb_cntl, sys_ap_lo, sys_ap_hi;
    uint32_t inv_req17, inv_ack17;
    const char *name;
} HubRegs;

static const HubRegs hub_gc = {
    RNO_GC_SEG0_DW, 0x0840u, 0x0841u, 0x0842u, 0x0857u,
    0x0880u, 0x08EBu, 0x090Bu, 0x092Bu,
    0x0987u, 0x0985u, 0x0986u,
    0x08B4u, 0x08C6u, "GFXHUB"
};
static const HubRegs hub_mm = {
    RNO_MMHUB_SEG0_DW, 0x0680u, 0x0681u, 0x0682u, 0x0697u,
    0x06C0u, 0x072Bu, 0x074Bu, 0x076Bu,
    0x0833u, 0x0831u, 0x0832u,
    0x06F4u, 0x0706u, "MMHUB"
};

static const char *gart_status = "GART not attempted (FIROZOS_GART_ENABLE=0)";
static uint64_t gart_va;
static uint64_t g_cpu_vram;
static int gart_table_ready;
static uint64_t gart_cp_probe_pa;
static uint32_t gart_fail_hub, gart_fail_step;
static uint64_t g_gart_mmio;
static uint64_t gart_pdb0_pa;
static uint32_t gart_pdb0_index;
static uint64_t gart_pdb0_entry;
static uint64_t gart_pt_pa;
static const char *gart_map_status = "GART map not attempted";

uint64_t amd_native_gart_va(void) { return gart_va; }
const char *amd_native_gart_status(void) { return gart_status; }
const char *amd_native_gart_map_status(void) { return gart_map_status; }

#define HOFF(h, r) (((h)->base_dw + (r)) << 2)

static uint32_t hrd(uint64_t mmio, const HubRegs *h, uint32_t r)
{ return gpu_mmio_read32(mmio, HOFF(h, r)); }

static int hub_invalidate(uint64_t mmio, const HubRegs *h);

/* Write, read back, compare under mask. Returns 1 if it stuck. */
static int hwr(uint64_t mmio, const HubRegs *h, uint32_t r, uint32_t v, uint32_t mask)
{
    gpu_mmio_write32(mmio, HOFF(h, r), v);
    return (hrd(mmio, h, r) & mask) == (v & mask);
}

static void vram_wr32(uint32_t off, uint32_t v)
{
    gpu_mmio_write32(g_cpu_vram, off, v);
}
static uint32_t vram_rd32(uint32_t off)
{
    return gpu_mmio_read32(g_cpu_vram, off);
}

/* V141: the GART page table can live in system RAM instead of VRAM.
   Evidence for doing so (V133 output): the CP physical write passes, but a
   write through GART VA fails with ALL fault registers 0.  A walker that
   reads garbage/zero PTEs (e.g. if MC_VM_FB_OFFSET-derived PA is not where
   BAR0+0x08000000 really is) produces exactly that silent drop.  Kernel RAM
   is identity mapped and its physical address is known for certain, so the
   page table no longer depends on FB_OFFSET. */
#ifndef FIROZOS_GART_SYSMEM_PT
#define FIROZOS_GART_SYSMEM_PT 1
#endif
static volatile uint32_t *g_sys_pt;       /* sysmem PT (virt == phys) */
static volatile uint32_t *g_sys_probe;    /* sysmem write-target page  */
static uint64_t g_sys_probe_pa;
static int g_case_sys;                    /* current probe targets sysmem */

static void cache_flush_line(volatile void *p)
{
    __asm__ volatile("clflush (%0)" : : "r"(p) : "memory");
}
/* off is always the VRAM-style absolute offset (GART_TABLE_VRAM_OFF + n). */
static void pt_wr32(uint32_t off, uint32_t v)
{
    if (g_sys_pt) {
        volatile uint32_t *p = &g_sys_pt[(off - GART_TABLE_VRAM_OFF) >> 2];
        *p = v; cache_flush_line(p);
    } else vram_wr32(off, v);
}
static uint32_t pt_rd32(uint32_t off)
{
    if (g_sys_pt) {
        volatile uint32_t *p = &g_sys_pt[(off - GART_TABLE_VRAM_OFF) >> 2];
        cache_flush_line(p);
        return *p;
    }
    return vram_rd32(off);
}
/* 4 KiB aligned, zeroed kernel RAM block below 4 GiB. */
/* The unaligned kmalloc() pointer is remembered so a block can be given back. */
static uint8_t *sa_raw[12];
static volatile uint32_t *sa_ptr[12];
static volatile uint32_t *sys_alloc_page_aligned(uint32_t bytes)
{
    uint8_t *raw = (uint8_t *)kmalloc(bytes + 4096u);
    uint32_t a, i;
    volatile uint32_t *p;
    if (!raw) return 0;
    if ((unsigned long long)raw + bytes + 4096u > GPU_DMA_LIMIT) { kfree(raw); return 0; }   /* GPU needs a 32-bit address */
    a = ((uint32_t)(unsigned long long)raw + 0xFFFu) & ~0xFFFu;
    p = (volatile uint32_t *)(unsigned long long)a;
    for (i = 0; i < bytes / 4u; i++) p[i] = 0;
    for (i = 0; i < 12u; i++)
        if (!sa_ptr[i]) { sa_ptr[i] = p; sa_raw[i] = raw; break; }
    return p;
}
static void sys_free_page_aligned(volatile uint32_t *p)
{
    uint32_t i;
    if (!p) return;
    for (i = 0; i < 12u; i++)
        if (sa_ptr[i] == p) { kfree(sa_raw[i]); sa_ptr[i] = 0; sa_raw[i] = 0; return; }
}

static int amd_native_gart_map_flags(uint32_t index, uint64_t phys, int system_bit)
{
    uint64_t pte;
    uint32_t off;
    if (!gart_table_ready || index >= GART_PAGES || (phys & 0xFFFULL)) {
        gart_map_status = "GART map rejected: invalid index/alignment";
        return 0;
    }
    /* Controlled V98 probe: keep all normal PTE attributes identical and
       toggle only PTE_SYSTEM. This does not change the page-table location
       or the MC physical address. */
    pte = phys | PTE_MTYPE_UC | PTE_VALID | PTE_SNOOP |
          PTE_EXEC | PTE_READ | PTE_WRITE | (system_bit ? PTE_SYSTEM : 0);
    off = GART_TABLE_VRAM_OFF + index * 8u;
    pt_wr32(off, (uint32_t)pte);
    pt_wr32(off + 4u, (uint32_t)(pte >> 32));
    if (pt_rd32(off) != (uint32_t)pte ||
        pt_rd32(off + 4u) != (uint32_t)(pte >> 32)) {
        gart_map_status = "GART map failed: PTE readback mismatch";
        return 0;
    }
    /* The PTE was changed after the initial hub flush; invalidate both
       VMID0 TLBs before exposing the mapping to hardware clients. */
    /* CP/GFX VMID0 is serviced by GFXHUB.  Keep MMHUB invalidate separate:
       on this Renoir bring-up the MMHUB context may not have a valid PTB yet,
       and making the GFX PTE mapping depend on it causes a false step-1 abort. */
    if (!hub_invalidate(g_gart_mmio, &hub_gc)) {
        gart_map_status = "GART map failed: GFXHUB TLB invalidate timeout";
        return 0;
    }
    /* Best-effort MMHUB flush for display/CPU clients; do not reject the
       GFXHUB mapping if MMHUB is not initialized on this bring-up path. */
    (void)hub_invalidate(g_gart_mmio, &hub_mm);
    gart_map_status = system_bit
        ? "GART map[0]: PTE SYSTEM-bit probe + GFXHUB TLB invalidate PASS"
        : "GART map[0]: PTE readback + GFXHUB TLB invalidate PASS (MMHUB best-effort)";
    return 1;
}

int amd_native_gart_map(uint32_t index, uint64_t phys)
{
    return amd_native_gart_map_flags(index, phys, 0);
}

static int hub_invalidate(uint64_t mmio, const HubRegs *h)
{
    uint32_t i;
    /* PER_VMID bit0 (VMID0) | INVALIDATE_L2_PTES|PDE0|PDE1|PDE2|L1_PTES (bits 18..22), FLUSH_TYPE 0 */
    uint32_t req = 1u | (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22);
    gpu_mmio_write32(mmio, HOFF(h, h->inv_req17), req);
    for (i = 0; i < 100000u; i++) {
        if (hrd(mmio, h, h->inv_ack17) & 1u)
            return 1;
        __asm__ volatile("pause");
    }
    return 0;
}

static int hub_enable(uint64_t mmio, const HubRegs *h, uint64_t pt_pa,
                      uint64_t pde_flags,
                      uint64_t va_start, uint64_t va_end, int *ctx_on)
{
    uint32_t v;
    uint64_t pde = pt_pa | PTE_VALID | pde_flags;
    *ctx_on = 0;

    /* 1. page table base / start / end (context still disabled) */
    if (!hwr(mmio, h, h->pt_base_lo,     (uint32_t)pde, 0xFFFFFFFFu)) return 1;
    if (!hwr(mmio, h, h->pt_base_lo + 1, (uint32_t)(pde >> 32), 0xFFFFFFFFu)) return 2;
    if (!hwr(mmio, h, h->pt_start_lo,     (uint32_t)(va_start >> 12), 0xFFFFFFFFu)) return 3;
    if (!hwr(mmio, h, h->pt_start_lo + 1, (uint32_t)(va_start >> 44), 0xFFFFFFFFu)) return 4;
    if (!hwr(mmio, h, h->pt_end_lo,       (uint32_t)(va_end >> 12), 0xFFFFFFFFu)) return 5;
    if (!hwr(mmio, h, h->pt_end_lo + 1,   (uint32_t)(va_end >> 44), 0xFFFFFFFFu)) return 6;

    /* 2. L1 TLB: ENABLE_L1_TLB=1, SYSTEM_ACCESS_MODE=3 (bits 4:3),
          SYSTEM_APERTURE_UNMAPPED_ACCESS=0 (bit5), ADVANCED_DRIVER_MODEL=1 (bit6),
          ECO_BITS=0 (10:7), ATC_EN=1 (bit13). MTYPE left at reset value. */
    v = hrd(mmio, h, h->tlb_cntl);
    v |= 1u;
    v = (v & ~0x18u) | (3u << 3);
    v &= ~(1u << 5);
    v |= (1u << 6);
    v &= ~0x780u;
    v |= (1u << 13);
    if (!hwr(mmio, h, h->tlb_cntl, v, 0xFFFFFFFFu)) return 7;

    /* 3. L2 cache: ENABLE_L2_CACHE=1, FRAGMENT_PROCESSING=0, PDE0_TAG_GEN=0,
          PDE_FAULT_CLASSIFICATION=0, CONTEXT1_IDENTITY_ACCESS_MODE=1,
          IDENTITY_MODE_FRAGMENT_SIZE=0 */
    v = hrd(mmio, h, h->l2_cntl);
    v |= 1u;
    v &= ~(1u << 1);
    v &= ~0x00000100u;
    v &= ~0x00040000u;
    v = (v & ~0x00180000u) | (1u << 19);
    v &= ~0x03E00000u;
    if (!hwr(mmio, h, h->l2_cntl, v, 0xFFFFFFFFu)) return 8;

    v = hrd(mmio, h, h->l2_cntl3);   /* BANK_SELECT=9, BIGK_FRAGMENT_SIZE=6 */
    v = (v & ~0x0000003Fu) | 9u;
    v = (v & ~0x000F8000u) | (6u << 15);
    if (!hwr(mmio, h, h->l2_cntl3, v, 0xFFFFFFFFu)) return 9;

    v = hrd(mmio, h, h->l2_cntl4);   /* VMC_TAP_PDE/PTE_REQUEST_PHYSICAL = 0 */
    v &= ~((1u << 6) | (1u << 7));
    if (!hwr(mmio, h, h->l2_cntl4, v, 0xFFFFFFFFu)) return 10;

    /* 4. VMID0 on, single-level page table (PAGE_TABLE_DEPTH=0, bits 2:1) */
    v = hrd(mmio, h, h->ctx0_cntl);
    v |= 1u;
    v &= ~0x6u;
    /* V143: Linux gfxhub_v1_0/mmhub_v1_0 enable_system_domain also sets
       RETRY_PERMISSION_OR_INVALID_PAGE_FAULT (bit 7) = 0.  The reset value
       of CONTEXT0_CNTL is 0x7FFE80 (bit 7 = 1, seen in the V142 output), so
       any failed translation was RETRIED FOREVER instead of faulting:
       the CP write just never completed and no fault was latched. */
    v &= ~0x80u;
    if (!hwr(mmio, h, h->ctx0_cntl, v, 0x87u)) return 11;
    *ctx_on = 1;

    /* 5. flush */
    if (!hub_invalidate(mmio, h)) return 12;
    return 0;
}

static void hub_rollback(uint64_t mmio, const HubRegs *h)
{
    uint32_t v = hrd(mmio, h, h->ctx0_cntl);
    gpu_mmio_write32(mmio, HOFF(h, h->ctx0_cntl), v & ~1u);
}

int amd_native_gart_enable(uint64_t mmio_base, uint64_t cpu_vram_base)
{
    uint64_t vram_start, vram_end, gart_end, pt_pa;
    uint32_t i, fboff, ap_lo, ap_hi, rc;
    int ctx_gc = 0, ctx_mm = 0;

    if (!gpu_mmio_can_access(mmio_base) || !gpu_mmio_can_access(cpu_vram_base)) {
        gart_status = "GART abort: MMIO/VRAM aperture not accessible"; return 0;
    }
    if (!vram_mib || mm_fbbase == 0xFFFFFFFFu || mm_fboff == 0xFFFFFFFFu) {
        gart_status = "GART abort: run VM probe first (VRAM range unknown)"; return 0;
    }
    if (mm_ctx0 & 1u) {
        gart_status = "GART abort: VMID0 already enabled by firmware"; return 0;
    }

    vram_start = (uint64_t)(mm_fbbase & 0x00FFFFFFu) << 24;
    vram_end   = ((uint64_t)(mm_fbtop & 0x00FFFFFFu) << 24) | 0x00FFFFFFULL;

    /* Safety: system aperture must cover VRAM, or scanout would be translated. */
    ap_lo = hrd(mmio_base, &hub_mm, hub_mm.sys_ap_lo);
    ap_hi = hrd(mmio_base, &hub_mm, hub_mm.sys_ap_hi);
    if (((uint64_t)ap_lo << 18) > vram_start ||
        (((uint64_t)ap_hi << 18) | 0x3FFFFULL) < vram_end) {
        gart_status = "GART abort: system aperture does not cover VRAM"; return 0;
    }

    /* GART VA: first 4 GiB boundary above VRAM end. */
    gart_va  = (vram_end + 1ULL + 0xFFFFFFFFULL) & ~0xFFFFFFFFULL;
    gart_end = gart_va + (uint64_t)GART_PAGES * 4096ULL - 1ULL;

    /* CPU access test to the table area in VRAM (also proves BAR0 covers it). */
    g_cpu_vram = cpu_vram_base;
    g_gart_mmio = mmio_base;
    gart_map_status = "GART map not attempted";
    gart_pt_pa = 0;
    vram_wr32(GART_TABLE_VRAM_OFF, 0xA5A55A5Au);
    if (vram_rd32(GART_TABLE_VRAM_OFF) != 0xA5A55A5Au) {
        gart_status = "GART abort: VRAM table area not CPU-writable via BAR0"; return 0;
    }
#if FIROZOS_GART_SYSMEM_PT
    g_sys_pt = sys_alloc_page_aligned(GART_PAGES * 8u);
    g_sys_probe = sys_alloc_page_aligned(4096u);
    if (g_sys_probe) g_sys_probe_pa = (uint64_t)(unsigned long long)g_sys_probe;
    if (!g_sys_pt) { gart_status = "GART abort: no sysmem for page table"; return 0; }
#endif
    for (i = 0; i < GART_PAGES * 2u; i++)
        pt_wr32(GART_TABLE_VRAM_OFF + i * 4u, 0);   /* all PTEs invalid */
    /* V111: mm_fboff must be captured before constructing the preloaded
       PTE. V110 used fboff before initialization, so the preload pointed
       at the wrong physical address and the CP preflight rejected it. */
    fboff = mm_fboff;

    /* Preload the CP VM probe mapping BEFORE VMID0 context enable. The
       initial hub flush then sees the exact MC physical PTE. */
    {
        const uint32_t probe_index = 0x100u;
        const uint64_t probe_phys = ((uint64_t)fboff << 24) + 0x00100000ULL;
        const uint64_t probe_pte = probe_phys | PTE_MTYPE_UC | PTE_VALID | PTE_SNOOP |
                                   PTE_EXEC | PTE_READ | PTE_WRITE;
        uint32_t poff = GART_TABLE_VRAM_OFF + probe_index * 8u;
        pt_wr32(poff, (uint32_t)probe_pte);
        pt_wr32(poff + 4u, (uint32_t)(probe_pte >> 32));
        if (pt_rd32(poff) != (uint32_t)probe_pte ||
            pt_rd32(poff + 4u) != (uint32_t)(probe_pte >> 32)) {
            gart_status = "GART abort: preloaded CP probe PTE readback mismatch";
            return 0;
        }
    }
    gart_cp_probe_pa = ((uint64_t)fboff << 24) + 0x00100000ULL;
    gart_table_ready = 1;

    pt_pa = ((uint64_t)fboff << 24) + (uint64_t)GART_TABLE_VRAM_OFF;
    if (g_sys_pt) pt_pa = (uint64_t)(unsigned long long)g_sys_pt;   /* system RAM PT */
    gart_pt_pa = pt_pa;
    /* RESTORE: V88/V92 known-good VMID0 configuration uses the PTB
       directly as the context-0 root (PAGE_TABLE_DEPTH=0). Do not build
       or enable the experimental PDB0 -> PTB hierarchy here. */
    gart_pdb0_pa = 0;
    gart_pdb0_index = 0;
    gart_pdb0_entry = 0;

    rc = (uint32_t)hub_enable(mmio_base, &hub_gc, pt_pa, g_sys_pt ? PTE_SYSTEM : 0, gart_va, gart_end, &ctx_gc);
    if (rc) {
        if (ctx_gc) hub_rollback(mmio_base, &hub_gc);
        gart_status = "GART FAILED on GFXHUB (rolled back)";
        gart_fail_hub = 1; gart_fail_step = rc;
        gart_table_ready = 0;
        return 0;
    }
    rc = (uint32_t)hub_enable(mmio_base, &hub_mm, pt_pa, g_sys_pt ? PTE_SYSTEM : 0, gart_va, gart_end, &ctx_mm);
    if (rc) {
        if (ctx_mm) hub_rollback(mmio_base, &hub_mm);
        hub_rollback(mmio_base, &hub_gc);
        gart_status = "GART FAILED on MMHUB (rolled back both hubs)";
        gart_fail_hub = 2; gart_fail_step = rc;
        gart_table_ready = 0;
        return 0;
    }

    gart_on_flag = 1;
    gart_status = g_sys_pt
        ? "GART/VMID0 enabled on GFXHUB+MMHUB (PT in system RAM)"
        : "GART/VMID0 enabled on GFXHUB+MMHUB (4 MiB hardware bring-up)";
    return 1;
}

uint32_t amd_native_gart_fail_hub(void)  { return gart_fail_hub; }
uint32_t amd_native_gart_fail_step(void) { return gart_fail_step; }

uint32_t amd_native_gfxhub_ctx0(void) { return gcn_ctx0; }
uint32_t amd_native_gfxhub_l2(void)   { return gcn_l2; }
uint32_t amd_native_gfxhub_tlb(void)  { return gcn_tlb; }
uint32_t amd_native_gfxhub_pt_lo(void){ return gcn_ptlo; }
uint32_t amd_native_gfxhub_pt_hi(void){ return gcn_pthi; }
uint32_t amd_native_gfxhub_pt_start(void){ return gcn_ptstart; }
uint32_t amd_native_gfxhub_pt_end(void){ return gcn_ptend; }
uint32_t amd_native_gart_pdb0_index(void){ return gart_pdb0_index; }
uint32_t amd_native_gart_pdb0_lo(void){ return (uint32_t)gart_pdb0_pa; }
uint32_t amd_native_gart_pdb0_hi(void){ return (uint32_t)(gart_pdb0_pa >> 32); }
uint32_t amd_native_gart_pdb0_entry_lo(void){ return (uint32_t)gart_pdb0_entry; }
uint32_t amd_native_gart_pdb0_entry_hi(void){ return (uint32_t)(gart_pdb0_entry >> 32); }
uint32_t amd_native_gart_pt_pa_lo(void){ return (uint32_t)gart_pt_pa; }
uint32_t amd_native_gart_pt_pa_hi(void){ return (uint32_t)(gart_pt_pa >> 32); }
uint32_t amd_native_mc_base_lo(void){ return (uint32_t)((uint64_t)mm_fboff << 24); }
uint32_t amd_native_mc_base_hi(void){ return (uint32_t)(((uint64_t)mm_fboff << 24) >> 32); }

/* MC address of a VRAM offset, or 0 if the range is unknown / outside VRAM. */
uint64_t amd_native_vram_mc_addr(uint32_t offset, uint32_t bytes)
{
    uint64_t vram_bytes = (uint64_t)vram_mib << 20;
    if (!vram_mib || mm_fbbase == 0xFFFFFFFFu)
        return 0;
    if ((uint64_t)offset + (uint64_t)bytes > vram_bytes)
        return 0;
    return ((uint64_t)(mm_fbbase & 0x00FFFFFFu) << 24) + (uint64_t)offset;
}

/* Renoir GPU/MC physical address for a VRAM offset.  This is intentionally
   separate from amd_native_vram_mc_addr(): the proven CP ring path continues
   using the existing FB LOCATION address, while the CP VM PTE must name the
   MC physical VRAM address described by MC_VM_FB_OFFSET. */
static uint64_t amd_native_cp_vm_mc_addr(uint32_t offset, uint32_t bytes)
{
    uint64_t vram_bytes = (uint64_t)vram_mib << 20;
    if (!vram_mib || mm_fboff == 0xFFFFFFFFu)
        return 0;
    if ((uint64_t)offset + (uint64_t)bytes > vram_bytes)
        return 0;
    return ((uint64_t)mm_fboff << 24) + (uint64_t)offset;
}

/* Read-only: VM L2 protection-fault status/address (offsets from the
   uploaded *_offset.h). Shows whether a DMA client (e.g. the PSP) hit the
   VM and faulted. Status fields (sh_mask): MORE_FAULTS[0], WALKER_ERROR[3:1],
   PERMISSION_FAULTS[7:4], MAPPING_ERROR[8], CID[17:9], RW[18], VMID[23:20]. */
static uint32_t f_mm_st, f_mm_lo, f_mm_hi, f_gc_st, f_gc_lo, f_gc_hi;
void amd_native_fault_probe(uint64_t mmio_base)
{
    f_mm_st = f_mm_lo = f_mm_hi = f_gc_st = f_gc_lo = f_gc_hi = 0xFFFFFFFFu;
    if (!gpu_mmio_can_access(mmio_base))
        return;
    f_mm_st = gpu_mmio_read32(mmio_base, MMHUB_BYTE(0x068Bu));
    f_mm_lo = gpu_mmio_read32(mmio_base, MMHUB_BYTE(0x068Cu));
    f_mm_hi = gpu_mmio_read32(mmio_base, MMHUB_BYTE(0x068Du));
    f_gc_st = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(0x084Bu));
    f_gc_lo = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(0x084Cu));
    f_gc_hi = gpu_mmio_read32(mmio_base, RNO_GC_BYTE(0x084Du));
}
uint32_t amd_native_fault_mm_status(void){return f_mm_st;}
uint32_t amd_native_fault_mm_lo(void){return f_mm_lo;}
uint32_t amd_native_fault_mm_hi(void){return f_mm_hi;}
uint32_t amd_native_fault_gc_status(void){return f_gc_st;}
uint32_t amd_native_fault_gc_lo(void){return f_gc_lo;}
uint32_t amd_native_fault_gc_hi(void){return f_gc_hi;}

/* ---------------------------------------------------------------------
   Read-only RLC/CP state snapshot (slot 0 = before PSP firmware loading,
   slot 1 = after). Shows whether the PSP load changed anything visible.
   Offsets and BASE_IDX are from the uploaded gc_9_1_offset.h:
     BASE_IDX 0 -> dword base 0x2000, BASE_IDX 1 -> dword base 0xA000
   (GC_BASE__INST0_SEG0/SEG1 of renoir_ip_offset.h). Reads only.
   --------------------------------------------------------------------- */
#define RNO_GC_SEG1_DW 0xA000u
#define GC1_BYTE(reg) ((RNO_GC_SEG1_DW + (reg)) << 2)
typedef struct { const char *name; uint32_t byte_off; } StateReg;
static const StateReg state_regs[] = {
    {"RLC_CNTL",  GC1_BYTE(0x4C00u)}, {"RLC_STAT",  GC1_BYTE(0x4C04u)},
    {"RLC_GPM",   GC1_BYTE(0x4C40u)}, {"RLC_SAFE",  GC1_BYTE(0x4C05u)},
    {"RLC_PG",    GC1_BYTE(0x4C43u)}, {"RLC_SCHED", GC1_BYTE(0x4CAAu)},
    {"CP_ME",     RNO_GC_BYTE(0x01B6u)}, {"CP_MEC",   RNO_GC_BYTE(0x008Du)},
    {"CP_BUSY",   RNO_GC_BYTE(0x019Fu)}, {"CPC_STAT", RNO_GC_BYTE(0x0084u)},
    {"CP_STALL1", RNO_GC_BYTE(0x019Du)}, {"RB0_CNTL", RNO_GC_BYTE(0x1041u)},
    {"RB0_WPTR",  RNO_GC_BYTE(0x1054u)}, {"SCRATCH0", GC1_BYTE(0x2040u)},
    {"GRBM_ST",   RNO_GC_BYTE(0x0004u)}, {"CP_STAT",  RNO_GC_BYTE(0x01A0u)},
};
#define STATE_REG_COUNT ((int)(sizeof(state_regs)/sizeof(state_regs[0])))
static uint32_t state_val[4][16];
static int state_valid[4];

void amd_native_gc_state_snap(uint64_t mmio_base, int slot)
{
    int i;
    if (slot < 0 || slot > 3) return;
    state_valid[slot] = 0;
    for (i = 0; i < STATE_REG_COUNT; i++) state_val[slot][i] = 0xFFFFFFFFu;
    if (!gpu_mmio_can_access(mmio_base)) return;
    for (i = 0; i < STATE_REG_COUNT; i++) {
        if (state_regs[i].byte_off + 4u > BAR5_REG_APERTURE) continue;
        state_val[slot][i] = gpu_mmio_read32(mmio_base, state_regs[i].byte_off);
    }
    state_valid[slot] = 1;
}
int amd_native_gc_state_count(void) { return STATE_REG_COUNT; }
const char *amd_native_gc_state_name(int i) { return (i>=0 && i<STATE_REG_COUNT) ? state_regs[i].name : ""; }
uint32_t amd_native_gc_state_val(int slot, int i)
{ return (slot>=0 && slot<4 && i>=0 && i<STATE_REG_COUNT && state_valid[slot]) ? state_val[slot][i] : 0xFFFFFFFFu; }

/* =====================================================================
   OPT-IN: minimal CP (command processor) ring test  (FIROZOS_CP_RING_TEST)

   WRITES GPU REGISTERS and un-halts CE/PFP/ME. NOT tested on hardware.
   Follows Linux gfx_v9_0_cp_gfx_resume() + gfx_v9_0_ring_test_ring()
   without the clear-state buffer / golden settings.

   Register offsets/segments and field positions: uploaded
   gc_9_1_offset.h / gc_9_1_sh_mask.h (CP_* are BASE_IDX 0, SCRATCH_REG0
   BASE_IDX 1). From Linux memory (NOT from the headers): the register
   order, MAX_CONTEXT=7, DEVICE_ID=1, PACKET3 SET_UCONFIG_REG = opcode 0x79
   with reg offset relative to 0xC000, 1-dword NOP = 0xFFFF1000, ring padded
   to a 256-dword write pointer.

   The ring and write-back words live in VRAM (MC address, inside the system
   aperture) and are written by the CPU through BAR0, so no GART is needed.
   On any failure CE/PFP/ME are halted again.
   ===================================================================== */
#define CP_RING_VRAM_OFF  0x0A000000u     /* 160 MiB: 4 KiB ring */
#define CP_WB_VRAM_OFF    0x0A001000u     /* 4 KiB write-back page */
#define CP_RING_BYTES     0x1000u
#define CP_ME_HALT_BITS   ((1u<<24)|(1u<<26)|(1u<<28))   /* CE|PFP|ME halt (sh_mask: 0x18,0x1a,0x1c) */

#define R_CP_RB0_WPTR_DELAY   RNO_GC_BYTE(0x01C1u)
#define R_CP_RB_VMID         RNO_GC_BYTE(0x1051u)
/* V127: Renoir GFXHUB VMID1 context registers. Linux gfxhub_v1_0
   programs VM_CONTEXT1_CNTL at context0+1 and VM_CONTEXT1 PT-base
   registers at context0+2 (one 64-bit pair per VMID). */
#define RNO_VM_CONTEXT0_CNTL_OFF   RNO_GC_BYTE(0x0880u)
#define RNO_VM_CONTEXT1_CNTL_OFF   RNO_GC_BYTE(0x0881u)
#define RNO_VM_CONTEXT1_PT_BASE_OFF RNO_GC_BYTE(0x08EDu)
#define RNO_VM_CONTEXT0_PT_BASE_OFF RNO_GC_BYTE(0x08EBu)
#define R_CP_RB0_CNTL        RNO_GC_BYTE(0x1041u)
#define R_CP_RB0_WPTR        RNO_GC_BYTE(0x1054u)
#define R_CP_RB0_WPTR_HI     RNO_GC_BYTE(0x1055u)
#define R_CP_RB0_RPTR_ADDR   RNO_GC_BYTE(0x1043u)
#define R_CP_RB0_RPTR_ADDR_HI RNO_GC_BYTE(0x1044u)
#define R_CP_WPTR_POLL_LO    RNO_GC_BYTE(0x1046u)
#define R_CP_WPTR_POLL_HI    RNO_GC_BYTE(0x1047u)
#define R_CP_RB0_BASE        RNO_GC_BYTE(0x1040u)
#define R_CP_RB0_BASE_HI     RNO_GC_BYTE(0x10B1u)
#define R_CP_DOORBELL_CTRL   RNO_GC_BYTE(0x1059u)
#define R_CP_MAX_CONTEXT     RNO_GC_BYTE(0x10AEu)
#define R_CP_DEVICE_ID       RNO_GC_BYTE(0x104Bu)
#define R_CP_ME_CNTL         RNO_GC_BYTE(0x01B6u)
#define R_CP_RB0_RPTR        RNO_GC_BYTE(0x01C0u)
#define R_CP_BUSY_STAT       RNO_GC_BYTE(0x019Fu)
#define R_SCRATCH_REG0       GC1_BYTE(0x2040u)

static const char *cp_status = "CP ring test not run (FIROZOS_CP_RING_TEST=0)";
static uint32_t cp_step, cp_scratch, cp_me_before, cp_me_after, cp_rptr_reg, cp_rptr_wb, cp_wptr_end, cp_grbm_after, cp_busy_after;

const char *amd_native_cp_status(void){return cp_status;}
uint32_t amd_native_cp_step(void){return cp_step;}
uint32_t amd_native_cp_scratch(void){return cp_scratch;}
uint32_t amd_native_cp_me_before(void){return cp_me_before;}
uint32_t amd_native_cp_me_after(void){return cp_me_after;}
uint32_t amd_native_cp_rptr_reg(void){return cp_rptr_reg;}
uint32_t amd_native_cp_rptr_wb(void){return cp_rptr_wb;}
uint32_t amd_native_cp_wptr_end(void){return cp_wptr_end;}
uint32_t amd_native_cp_grbm_after(void){return cp_grbm_after;}
uint32_t amd_native_cp_busy_after(void){return cp_busy_after;}

static void cp_delay_us(uint64_t m, uint32_t us)
{   /* rough: each MMIO read over PCIe costs on the order of a microsecond */
    uint32_t i; for (i = 0; i < us; i++) (void)gpu_mmio_read32(m, R_CP_ME_CNTL);
}
static int cp_wr_ok(uint64_t m, uint32_t off, uint32_t v, uint32_t mask, uint32_t step)
{
    gpu_mmio_write32(m, off, v);
    if ((gpu_mmio_read32(m, off) & mask) == (v & mask)) return 1;
    cp_step = step;
    return 0;
}


/* =====================================================================
   OPT-IN: start the RLC (run-list controller) before the CP.

   WRITES GPU REGISTERS. The PSP loaded the RLC_G microcode but nothing started the RLC, and the CP ring test (see
   above) timed out with RPTR stuck at 0. Linux gfx_v9_0_hw_init() runs gfx_v9_0_rlc_resume() BEFORE
   gfx_v9_0_cp_resume(): rlc_stop (RLC_CNTL.RLC_ENABLE_F32 = 0, wait for the serdes), RLC_CGCG_CGLS_CTRL = 0, init_pg
   (clear-state buffer address in RLC_CSIB_*, save/restore machine), RLC_SPM_MC_CNTL VMID, then rlc_start
   (RLC_ENABLE_F32 = 1, 50 us). Offsets: gc_9_1_offset.h (all BASE_IDX 1); bit 0 of RLC_CNTL = RLC_ENABLE_F32
   (gc_9_1_sh_mask.h).

   This does only: stop, CGCG/CGLS off, start. NOT done (yet): the clear-state buffer (RLC_CSIB_*, needs the gfx9 clear
   state table), the save/restore machine, the SPM VMID, the wait for RLC_SERDES_CU_MASTER_BUSY (Linux selects every
   SE/SH with GRBM_GFX_INDEX first), the GUI-idle interrupt enables. If the RLC does not run after this, those are the
   next candidates.
   ===================================================================== */
#define R_RLC_CNTL          GC1_BYTE(0x4C00u)
#define R_RLC_STAT          GC1_BYTE(0x4C04u)
#define R_RLC_GPM_STAT      GC1_BYTE(0x4C40u)
#define R_RLC_CGCG_CGLS     GC1_BYTE(0x4C49u)
#define RLC_ENABLE_F32      0x00000001u

static const char *rlc_status = "RLC start not run";
static uint32_t rlc_v[10];   /* 0 CNTL before, 1 STAT before, 2 GPM_STAT before, 3 CGCG before, 4 CNTL after stop,
                                5 CNTL after start, 6 STAT after, 7 GPM_STAT after, 8 STAT +1 ms, 9 GPM_STAT +1 ms */

const char *amd_native_rlc_status(void) { return rlc_status; }
uint32_t amd_native_rlc_value(int i) { return (i >= 0 && i < 10) ? rlc_v[i] : 0; }

int amd_native_rlc_start(uint64_t mmio)
{
    int i;
    for (i = 0; i < 10; i++) rlc_v[i] = 0;
    if (!gpu_mmio_can_access(mmio)) { rlc_status = "RLC start skipped: no MMIO"; return 0; }

    rlc_v[0] = gpu_mmio_read32(mmio, R_RLC_CNTL);
    rlc_v[1] = gpu_mmio_read32(mmio, R_RLC_STAT);
    rlc_v[2] = gpu_mmio_read32(mmio, R_RLC_GPM_STAT);
    rlc_v[3] = gpu_mmio_read32(mmio, R_RLC_CGCG_CGLS);
    if (rlc_v[0] == 0xFFFFFFFFu && rlc_v[1] == 0xFFFFFFFFu) {
        rlc_status = "RLC start skipped: RLC registers read 0xFFFFFFFF"; return 0; }

    /* rlc_stop (without the serdes wait and the GUI-idle interrupt) */
    gpu_mmio_write32(mmio, R_RLC_CNTL, rlc_v[0] & ~RLC_ENABLE_F32);
    rlc_v[4] = gpu_mmio_read32(mmio, R_RLC_CNTL);
    if (rlc_v[4] & RLC_ENABLE_F32) { rlc_status = "RLC start abort: RLC_ENABLE_F32 did not clear"; return 0; }

    gpu_mmio_write32(mmio, R_RLC_CGCG_CGLS, 0);

    /* rlc_start: read-modify-write like WREG32_FIELD15, then 50 us */
    gpu_mmio_write32(mmio, R_RLC_CNTL, gpu_mmio_read32(mmio, R_RLC_CNTL) | RLC_ENABLE_F32);
    cp_delay_us(mmio, 50);
    rlc_v[5] = gpu_mmio_read32(mmio, R_RLC_CNTL);
    rlc_v[6] = gpu_mmio_read32(mmio, R_RLC_STAT);
    rlc_v[7] = gpu_mmio_read32(mmio, R_RLC_GPM_STAT);
    if (!(rlc_v[5] & RLC_ENABLE_F32)) { rlc_status = "RLC start FAILED: RLC_ENABLE_F32 did not stick"; return 0; }

    cp_delay_us(mmio, 1000);
    rlc_v[8] = gpu_mmio_read32(mmio, R_RLC_STAT);
    rlc_v[9] = gpu_mmio_read32(mmio, R_RLC_GPM_STAT);
    rlc_status = "RLC_ENABLE_F32 set (stop, CGCG/CGLS off, start); clear-state buffer and save/restore NOT set up";
    return 1;
}

int amd_native_cp_ring_test(uint64_t mmio, uint64_t cpu_vram, int fw_ready)
{
    uint64_t ring_mc, wb_mc;
    uint32_t v, i, bufsz = 9u;                 /* log2(4096/8) */
    uint32_t cntl = bufsz | ((bufsz - 2u) << 8);
    uint32_t me;
    cp_step = 0; cp_scratch = cp_me_before = cp_me_after = 0;
    cp_rptr_reg = cp_rptr_wb = cp_wptr_end = cp_grbm_after = cp_busy_after = 0;

    if (!fw_ready) { cp_status = "CP ring test skipped: PSP firmware (CE/PFP/ME) not loaded this boot"; return 0; }
    if (!gpu_mmio_can_access(mmio) || !gpu_mmio_can_access(cpu_vram)) {
        cp_status = "CP ring test skipped: MMIO/VRAM aperture not accessible"; return 0; }
    ring_mc = amd_native_vram_mc_addr(CP_RING_VRAM_OFF, CP_RING_BYTES);
    wb_mc   = amd_native_vram_mc_addr(CP_WB_VRAM_OFF, 0x1000u);
    if (!ring_mc || !wb_mc) { cp_status = "CP ring test skipped: VRAM range unknown"; return 0; }

    g_cpu_vram = cpu_vram;
    vram_wr32(CP_RING_VRAM_OFF, 0x5AA5A55Au);
    if (vram_rd32(CP_RING_VRAM_OFF) != 0x5AA5A55Au) {
        cp_status = "CP ring test abort: VRAM ring area not CPU-writable via BAR0"; return 0; }
    for (i = 0; i < CP_RING_BYTES / 4u; i++) vram_wr32(CP_RING_VRAM_OFF + i * 4u, 0);
    for (i = 0; i < 0x1000u / 4u; i++)       vram_wr32(CP_WB_VRAM_OFF + i * 4u, 0);

    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    cp_me_before = me;
    if ((me & CP_ME_HALT_BITS) != CP_ME_HALT_BITS) {
        cp_status = "CP ring test abort: CE/PFP/ME not halted at start (unexpected state)"; return 0; }

    /* --- gfx_v9_0_cp_gfx_resume() order --- */
    if (!cp_wr_ok(mmio, R_CP_RB0_WPTR_DELAY, 0, 0xFFFFFFFFu, 1)) goto fail;
    if (!cp_wr_ok(mmio, R_CP_RB_VMID, 0, 0xFFFFFFFFu, 2)) goto fail;
    gpu_mmio_write32(mmio, R_CP_RB0_CNTL, cntl | (1u << 31));        /* RB_RPTR_WR_ENA */
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR, (uint32_t)wb_mc);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR_HI, (uint32_t)(wb_mc >> 32) & 0xFFFFu);
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_LO, (uint32_t)(wb_mc + 8u));
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_HI, (uint32_t)((wb_mc + 8u) >> 32));
    cp_delay_us(mmio, 1000);
    if (!cp_wr_ok(mmio, R_CP_RB0_CNTL, cntl, 0x3FFFu, 3)) goto fail;
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE, (uint32_t)(ring_mc >> 8), 0xFFFFFFFFu, 4)) goto fail;
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE_HI, (uint32_t)((ring_mc >> 8) >> 32), 0xFFFFFFFFu, 5)) goto fail;
    v = gpu_mmio_read32(mmio, R_CP_DOORBELL_CTRL);
    if (!cp_wr_ok(mmio, R_CP_DOORBELL_CTRL, v & ~(1u << 30), 1u << 30, 6)) goto fail;   /* DOORBELL_EN = 0 */
    if (!cp_wr_ok(mmio, R_CP_MAX_CONTEXT, 7u, 0xFFFFFFFFu, 7)) goto fail;
    if (!cp_wr_ok(mmio, R_CP_DEVICE_ID, 1u, 0xFFFFFFFFu, 8)) goto fail;

    /* --- un-halt CE, PFP, ME --- */
    if (!cp_wr_ok(mmio, R_CP_ME_CNTL, me & ~CP_ME_HALT_BITS, CP_ME_HALT_BITS, 9)) goto fail;
    cp_delay_us(mmio, 50);

    /* --- gfx_v9_0_ring_test_ring(): SET_UCONFIG_REG SCRATCH_REG0 = 0xDEADBEEF --- */
    if (!cp_wr_ok(mmio, R_SCRATCH_REG0, 0xCAFEDEADu, 0xFFFFFFFFu, 10)) goto fail;
    vram_wr32(CP_RING_VRAM_OFF + 0u, 0xC0017900u);        /* PACKET3(SET_UCONFIG_REG, 1) */
    vram_wr32(CP_RING_VRAM_OFF + 4u, 0x00000040u);        /* SCRATCH_REG0 - 0xC000 (dword 0xC040) */
    vram_wr32(CP_RING_VRAM_OFF + 8u, 0xDEADBEEFu);
    for (i = 3; i < 256u; i++) vram_wr32(CP_RING_VRAM_OFF + i * 4u, 0xFFFF1000u);  /* 1-dword NOPs */
    (void)vram_rd32(CP_RING_VRAM_OFF + 255u * 4u);        /* flush posted writes */
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 256u);
    cp_wptr_end = 256u;

    for (i = 0; i < 200000u; i++) {
        cp_scratch = gpu_mmio_read32(mmio, R_SCRATCH_REG0);
        if (cp_scratch == 0xDEADBEEFu) break;
    }
    cp_rptr_reg = gpu_mmio_read32(mmio, R_CP_RB0_RPTR);
    cp_rptr_wb  = vram_rd32(CP_WB_VRAM_OFF);
    cp_me_after = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    cp_grbm_after = gpu_mmio_read32(mmio, GC_GRBM_STATUS_OFF);
    cp_busy_after = gpu_mmio_read32(mmio, R_CP_BUSY_STAT);

    /* leave the CP halted again in every case */
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, gpu_mmio_read32(mmio, R_CP_ME_CNTL) | CP_ME_HALT_BITS);

    if (cp_scratch == 0xDEADBEEFu) {
        cp_status = "CP ring test PASSED: CP executed our packet (scratch=0xDEADBEEF)";
        return 1;
    }
    cp_status = "CP ring test TIMEOUT: CP did not execute the packet (CP re-halted)";
    return 0;

fail:
    cp_me_after = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, cp_me_after | CP_ME_HALT_BITS);   /* back to halted */
    cp_status = "CP ring test ABORTED: a register write did not stick (see step; CP re-halted)";
    return 0;
}

/* ---------------------------------------------------------------------
   Opt-in CP "pulse" test (FIROZOS_CP_PULSE_TEST).
   Mirrors what Linux does at the start of CP bring-up, then undoes it:
     1. scratch-register write/readback (Linux ring test does this first)
     2. CP_ME_CNTL = 0 and CP_MEC_CNTL = 0   (un-halt CE/PFP/ME and MEC1/2)
     3. wait ~20 ms, snapshot (slot 2)
     4. restore the ORIGINAL CP_ME_CNTL / CP_MEC_CNTL (halt again), wait,
        snapshot (slot 3)
   No ring is programmed and no packets are submitted. Registers:
   CP_ME_CNTL 0x01B6, CP_MEC_CNTL 0x008D (seg0), SCRATCH_REG0 0x2040 (seg1),
   all from the uploaded gc_9_1_offset.h; halt bits from gc_9_1_sh_mask.h.
   --------------------------------------------------------------------- */
static const char *pulse_status = "CP pulse test not run";
static uint32_t pulse_scratch_rb, pulse_me_orig, pulse_mec_orig, pulse_me_after, pulse_mec_after;

static void spin_ms(uint32_t ms)
{
    uint32_t i;
    for (i = 0; i < ms * 1000u; i++)
        __asm__ volatile("outb %%al, $0x80" : : "a"(0) : "memory");   /* ~1 us per write */
}

int amd_native_cp_pulse_test(uint64_t mmio_base)
{
    const uint32_t off_me  = RNO_GC_BYTE(0x01B6u);
    const uint32_t off_mec = RNO_GC_BYTE(0x008Du);
    const uint32_t off_scr = GC1_BYTE(0x2040u);
    const uint32_t off_grbm = RNO_GC_BYTE(0x0004u);
    uint32_t g;

    if (!gpu_mmio_can_access(mmio_base)) { pulse_status = "CP pulse skipped: no MMIO"; return 0; }
    g = gpu_mmio_read32(mmio_base, off_grbm);
    if (g == 0xFFFFFFFFu) { pulse_status = "CP pulse skipped: GC not responding"; return 0; }

    /* 1. scratch register round trip */
    gpu_mmio_write32(mmio_base, off_scr, 0xCAFEDEADu);
    pulse_scratch_rb = gpu_mmio_read32(mmio_base, off_scr);
    gpu_mmio_write32(mmio_base, off_scr, 0u);
    if (pulse_scratch_rb != 0xCAFEDEADu) {
        pulse_status = "CP pulse aborted: SCRATCH_REG0 write did not stick";
        return 0;
    }

    pulse_me_orig  = gpu_mmio_read32(mmio_base, off_me);
    pulse_mec_orig = gpu_mmio_read32(mmio_base, off_mec);

    /* 2./3. un-halt, observe */
    gpu_mmio_write32(mmio_base, off_me, 0u);
    gpu_mmio_write32(mmio_base, off_mec, 0u);
    spin_ms(20);
    amd_native_gc_state_snap(mmio_base, 2);

    /* 4. restore the original (halted) state, observe */
    gpu_mmio_write32(mmio_base, off_me,  pulse_me_orig);
    gpu_mmio_write32(mmio_base, off_mec, pulse_mec_orig);
    spin_ms(20);
    pulse_me_after  = gpu_mmio_read32(mmio_base, off_me);
    pulse_mec_after = gpu_mmio_read32(mmio_base, off_mec);
    amd_native_gc_state_snap(mmio_base, 3);

    if (pulse_me_after != pulse_me_orig || pulse_mec_after != pulse_mec_orig) {
        pulse_status = "CP pulse done, but halt state was NOT restored exactly";
        return 0;
    }
    pulse_status = "CP pulse done: scratch OK, un-halted, re-halted (restored)";
    return 1;
}
const char *amd_native_cp_pulse_status(void) { return pulse_status; }
uint32_t amd_native_cp_pulse_scratch(void) { return pulse_scratch_rb; }

/* =====================================================================
   OPT-IN: CP ring test MATRIX (FIROZOS_CP_RING_MATRIX)

   The single VRAM-ring test timed out: scratch unchanged, RPTR stayed 0,
   everything idle, no register write failed. This runs two variants in one
   boot to tell the likely causes apart. Each variant halts the CP again
   afterwards.
     V1 "VRAM": ring + write-back page in VRAM (MC address, via BAR0), as
        before, but WPTR_HI is now written after WPTR (Linux does both) and
        the write pointer is also mirrored into the WPTR poll memory.
     V2 "SYS" : same, but ring + write-back in ordinary system RAM, addressed
        physically (GART is off). If only V2 passes, the CP cannot reach VRAM
        through GFXHUB in its current aperture/TLB mode.
   Recorded per variant: RPTR (reg and write-back), scratch, ME_CNTL, GRBM,
   BUSY, CP_STALLED_STAT1, WPTR_POLL_CNTL, DOORBELL_CONTROL, RB0_BASE,
   GFXHUB aperture LOW/HIGH. WRITES GPU REGISTERS. Not tested on hardware.
   ===================================================================== */
#define R_CP_WPTR_POLL_CNTL  RNO_GC_BYTE(0x01C2u)
#define R_CP_STALLED_STAT1   RNO_GC_BYTE(0x019Du)
#define R_GC_AP_LOW          RNO_GC_BYTE(0x0985u)
#define R_GC_AP_HIGH         RNO_GC_BYTE(0x0986u)

typedef struct {
    const char *name; uint32_t ran, pass, step;
    uint32_t rptr, wb, scr, me, grbm, busy, stall1, poll, db, base, ap_lo, ap_hi;
    uint32_t hd_pfp, hd_me, hd_ce, st2, st3, cpf, grbm2;
} CpVar;
#define CPV_COUNT 6
static CpVar cpv[CPV_COUNT] = {
    { .name = "UCFG3" }, { .name = "UCFG253" }, { .name = "WRDATA" },
    { .name = "D-UCFG3" }, { .name = "D-UCFG253" }, { .name = "D-WRDATA" },   /* same three, after direct ucode load */
};
static const char *cpm_status = "CP ring matrix not run (FIROZOS_CP_RING_MATRIX=0)";

/*  Findings so far (V89/V90 runs):
      * The CP reads the ring in VRAM (RPTR moves), so the ring is reachable.
      * UCFG3 (3 dwords): RPTR reaches 3 but the packet has no visible effect.
      * UCFG253: RPTR stalls around 0x90-0x98 whatever the padding is -> the
        PFP->ME hand-over backs up: the ME is not consuming commands.
      * WRDATA ran after a stalled variant and RPTR never moved (stale queue).
    V91 changes:
      * before EVERY variant the CE/PFP/ME pipes are reset (halt + PIPE_RESET
        bits, then release) so a variant cannot inherit a stuck queue;
      * after every variant the last packet header seen by PFP/ME/CE
        (CP_*_HEADER_DUMP), CP_STALLED_STAT2/3, CPF_STATUS and GRBM_STATUS2
        are recorded - they show where a packet stopped;
      * ME/PFP/CE microcode RAM is read back (CP_ME_RAM_RADDR/DATA etc.) and
        compared with the firmware file: shows whether the PSP really put
        ucode into the engines;
      * optional (direct_ucode=1): Linux's "direct" ucode load through
        CP_PFP_UCODE_ADDR/DATA, CP_CE_UCODE_ADDR/DATA, CP_ME_RAM_WADDR/DATA,
        followed by the same three variants again ("D-" rows).               */
#define CPV_WB_TEST_OFF  0x40u      /* dword inside the write-back page */
#define R_CP_ME_HDR   RNO_GC_BYTE(0x01A1u)
#define R_CP_PFP_HDR  RNO_GC_BYTE(0x01A2u)
#define R_CP_CE_HDR   RNO_GC_BYTE(0x01A4u)
#define R_CP_STALL2   RNO_GC_BYTE(0x019Eu)
#define R_CP_STALL3   RNO_GC_BYTE(0x019Cu)
#define R_CP_CPF_STAT RNO_GC_BYTE(0x0087u)
#define R_GRBM_STAT2  RNO_GC_BYTE(0x0002u)
#define R_PFP_UC_ADDR GC1_BYTE(0x5814u)
#define R_PFP_UC_DATA GC1_BYTE(0x5815u)
#define R_ME_RAM_ADDR GC1_BYTE(0x5816u)     /* RADDR and WADDR are the same register */
#define R_ME_RAM_DATA GC1_BYTE(0x5817u)
#define R_CE_UC_ADDR  GC1_BYTE(0x5818u)
#define R_CE_UC_DATA  GC1_BYTE(0x5819u)
#define R_ME_CNTL_PIPE_RESET ((1u<<16)|(1u<<18)|(1u<<20))   /* CE|PFP|ME pipe0 reset (sh_mask 0x10,0x12,0x14) */

static void cpm_wr(uint32_t off, uint32_t v) { vram_wr32(CP_RING_VRAM_OFF + off, v); }
static uint32_t cpm_rd(uint32_t off) { return vram_rd32(CP_RING_VRAM_OFF + off); }

/* ---- ucode RAM probe / direct load ------------------------------------ */
static uint32_t uc_pre[3][4], uc_post[3][4], uc_fw[3][4], uc_ver[3];
static int uc_have_post;
static const char *uc_status = "ucode probe not run";
static const int uc_fw_idx[3] = { 6 /*PFP*/, 2 /*CE*/, 3 /*ME*/ };

static int uc_resolve(int which, const uint32_t **data, uint32_t *ndw, uint32_t *ver)
{
    const AmdFirmwareEntry *e = amd_firmware_get(uc_fw_idx[which]);
    const uint8_t *f; uint32_t hdr, usize, uoff;
    if (!e || !e->address || !e->size || !e->status) return 0;
    f = (const uint8_t *)(unsigned long long)(uint32_t)e->address;
    hdr = *(const uint32_t *)(f + 4);
    usize = *(const uint32_t *)(f + 20);
    uoff = *(const uint32_t *)(f + 24);
    if (!usize || usize > 0x400000u || uoff < hdr || (uint64_t)uoff + usize > e->size) return 0;
    *data = (const uint32_t *)(f + uoff); *ndw = usize / 4u; *ver = *(const uint32_t *)(f + 16);
    return 1;
}
static void uc_read4(uint64_t m, int which, uint32_t out[4])
{
    uint32_t i, a = (which == 0) ? R_PFP_UC_ADDR : (which == 1) ? R_CE_UC_ADDR : R_ME_RAM_ADDR;
    uint32_t d = (which == 0) ? R_PFP_UC_DATA : (which == 1) ? R_CE_UC_DATA : R_ME_RAM_DATA;
    gpu_mmio_write32(m, a, 0);
    for (i = 0; i < 4; i++) out[i] = gpu_mmio_read32(m, d);
}
static void uc_probe(uint64_t m, uint32_t dst[3][4])
{
    int w;
    for (w = 0; w < 3; w++) uc_read4(m, w, dst[w]);
}
static void uc_expected(void)
{
    int w, i;
    for (w = 0; w < 3; w++) {
        const uint32_t *d; uint32_t n, v;
        for (i = 0; i < 4; i++) uc_fw[w][i] = 0xFFFFFFFFu;
        uc_ver[w] = 0;
        if (uc_resolve(w, &d, &n, &v)) { for (i = 0; i < 4 && (uint32_t)i < n; i++) uc_fw[w][i] = d[i]; uc_ver[w] = v; }
    }
}
/* Linux gfx_v9_0_cp_gfx_load_microcode(): halted CP, PFP, CE, then ME. */
static int uc_direct_load(uint64_t m)
{
    int w; uint32_t i;
    gpu_mmio_write32(m, R_CP_ME_CNTL, gpu_mmio_read32(m, R_CP_ME_CNTL) | CP_ME_HALT_BITS);
    cp_delay_us(m, 100);
    for (w = 0; w < 3; w++) {
        const uint32_t *d; uint32_t n, v;
        uint32_t a = (w == 0) ? R_PFP_UC_ADDR : (w == 1) ? R_CE_UC_ADDR : R_ME_RAM_ADDR;
        uint32_t dr = (w == 0) ? R_PFP_UC_DATA : (w == 1) ? R_CE_UC_DATA : R_ME_RAM_DATA;
        if (!uc_resolve(w, &d, &n, &v)) { uc_status = "direct ucode load skipped: firmware header invalid"; return 0; }
        gpu_mmio_write32(m, a, 0);
        for (i = 0; i < n; i++) gpu_mmio_write32(m, dr, d[i]);
        gpu_mmio_write32(m, a, v);                 /* Linux writes the ucode version into ADDR at the end */
    }
    uc_probe(m, uc_post); uc_have_post = 1;
    uc_status = "direct ucode load done (PFP, CE, ME written)";
    return 1;
}
const char *amd_native_cp_ucode_status(void) { return uc_status; }
uint32_t amd_native_cp_ucode(int what, int which, int i)
{
    if (which < 0 || which > 2 || i < 0 || i > 3) return 0;
    if (what == 0) return uc_pre[which][i];
    if (what == 1) return uc_have_post ? uc_post[which][i] : 0xFFFFFFFFu;
    return uc_fw[which][i];
}

static int cpm_variant(uint64_t mmio, int k, uint64_t ring_gpu, uint64_t wb_gpu)
{
    int kind = k % 3;
    CpVar *r = &cpv[k];
    uint32_t i, v, me, wptr, bufsz = 9u, cntl = bufsz | ((bufsz - 2u) << 8);
    uint64_t tgt = wb_gpu + CPV_WB_TEST_OFF;

    r->ran = 1; r->pass = 0; r->step = 0;
    for (i = 0; i < 0x2000u / 4u; i++) cpm_wr(i * 4u, 0);        /* ring 4K + wb 4K */
    (void)cpm_rd(0x1FFCu);

    /* halt + pipe reset (clears any command queue left by the previous variant), then release reset */
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, (me | CP_ME_HALT_BITS | R_ME_CNTL_PIPE_RESET));
    cp_delay_us(mmio, 100);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, (me | CP_ME_HALT_BITS) & ~R_ME_CNTL_PIPE_RESET);
    cp_delay_us(mmio, 100);
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    if ((me & CP_ME_HALT_BITS) != CP_ME_HALT_BITS) { r->step = 99; return 0; }

    if (!cp_wr_ok(mmio, R_CP_RB0_WPTR_DELAY, 0, 0xFFFFFFFFu, 1)) { r->step = 1; goto out; }
    if (!cp_wr_ok(mmio, R_CP_RB_VMID, 0, 0xFFFFFFFFu, 2)) { r->step = 2; goto out; }
    gpu_mmio_write32(mmio, R_CP_RB0_CNTL, cntl | (1u << 31));        /* RB_RPTR_WR_ENA */
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR, (uint32_t)wb_gpu);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR_HI, (uint32_t)(wb_gpu >> 32) & 0xFFFFu);
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_LO, (uint32_t)(wb_gpu + 8u));
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_HI, (uint32_t)((wb_gpu + 8u) >> 32));
    cp_delay_us(mmio, 1000);
    if (!cp_wr_ok(mmio, R_CP_RB0_CNTL, cntl, 0x3FFFu, 3)) { r->step = 3; goto out; }
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE, (uint32_t)(ring_gpu >> 8), 0xFFFFFFFFu, 4)) { r->step = 4; goto out; }
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE_HI, (uint32_t)((ring_gpu >> 8) >> 32), 0xFFFFFFFFu, 5)) { r->step = 5; goto out; }
    v = gpu_mmio_read32(mmio, R_CP_DOORBELL_CTRL);
    if (!cp_wr_ok(mmio, R_CP_DOORBELL_CTRL, v & ~(1u << 30), 1u << 30, 6)) { r->step = 6; goto out; }
    if (!cp_wr_ok(mmio, R_CP_MAX_CONTEXT, 7u, 0xFFFFFFFFu, 7)) { r->step = 7; goto out; }
    if (!cp_wr_ok(mmio, R_CP_DEVICE_ID, 1u, 0xFFFFFFFFu, 8)) { r->step = 8; goto out; }
    if (!cp_wr_ok(mmio, R_SCRATCH_REG0, 0xCAFEDEADu, 0xFFFFFFFFu, 10)) { r->step = 10; goto out; }

    if (kind == 2) {                       /* WRITE_DATA: header, control, addr_lo, addr_hi, data */
        cpm_wr(0u,  0xC0033700u);
        cpm_wr(4u,  (5u << 8) | (1u << 20));
        cpm_wr(8u,  (uint32_t)tgt);
        cpm_wr(12u, (uint32_t)(tgt >> 32));
        cpm_wr(16u, 0xA5A5F00Du);
        wptr = 5u;
    } else {
        cpm_wr(0u, 0xC0017900u);                        /* PACKET3(SET_UCONFIG_REG, 1) */
        cpm_wr(4u, 0x00000040u);                        /* SCRATCH_REG0 - 0xC000 */
        cpm_wr(8u, 0xDEADBEEFu);
        wptr = 3u;
        if (kind == 1) {
            for (i = 3; i + 1 < 254u; i += 2) { cpm_wr(i * 4u, 0xC0001000u); cpm_wr(i * 4u + 4u, 0u); }
            wptr = 253u;
        }
    }
    (void)cpm_rd(0x1FFCu);

    if (!cp_wr_ok(mmio, R_CP_ME_CNTL, me & ~CP_ME_HALT_BITS, CP_ME_HALT_BITS, 9)) { r->step = 9; goto out; }
    cp_delay_us(mmio, 50);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, wptr);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0u);

    for (i = 0; i < 200000u; i++) {
        if (kind == 2) { r->scr = vram_rd32(CP_WB_VRAM_OFF + CPV_WB_TEST_OFF); if (r->scr == 0xA5A5F00Du) break; }
        else           { r->scr = gpu_mmio_read32(mmio, R_SCRATCH_REG0);       if (r->scr == 0xDEADBEEFu) break; }
    }
out:
    r->rptr   = gpu_mmio_read32(mmio, R_CP_RB0_RPTR);
    r->wb     = vram_rd32(CP_WB_VRAM_OFF);
    r->me     = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    r->grbm   = gpu_mmio_read32(mmio, GC_GRBM_STATUS_OFF);
    r->busy   = gpu_mmio_read32(mmio, R_CP_BUSY_STAT);
    r->stall1 = gpu_mmio_read32(mmio, R_CP_STALLED_STAT1);
    r->poll   = gpu_mmio_read32(mmio, R_CP_WPTR_POLL_CNTL);
    r->db     = gpu_mmio_read32(mmio, R_CP_DOORBELL_CTRL);
    r->base   = gpu_mmio_read32(mmio, R_CP_RB0_BASE);
    r->ap_lo  = gpu_mmio_read32(mmio, R_GC_AP_LOW);
    r->ap_hi  = gpu_mmio_read32(mmio, R_GC_AP_HIGH);
    r->hd_pfp = gpu_mmio_read32(mmio, R_CP_PFP_HDR);
    r->hd_me  = gpu_mmio_read32(mmio, R_CP_ME_HDR);
    r->hd_ce  = gpu_mmio_read32(mmio, R_CP_CE_HDR);
    r->st2    = gpu_mmio_read32(mmio, R_CP_STALL2);
    r->st3    = gpu_mmio_read32(mmio, R_CP_STALL3);
    r->cpf    = gpu_mmio_read32(mmio, R_CP_CPF_STAT);
    r->grbm2  = gpu_mmio_read32(mmio, R_GRBM_STAT2);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, gpu_mmio_read32(mmio, R_CP_ME_CNTL) | CP_ME_HALT_BITS);
    cp_delay_us(mmio, 100);
    r->pass = (kind == 2) ? (r->scr == 0xA5A5F00Du) : (r->scr == 0xDEADBEEFu);
    return (int)r->pass;
}


/* =====================================================================
   V93: CP -> VMID0/GART memory write.
   V92 proved CP packet execution. This test now makes the CP WRITE_DATA
   packet target the exact VA mapped by amd_native_gart_map(0, 0x00100000).
   Ring storage remains a physical VRAM MC address; only the packet target
   uses the VMID0/GART VA. No firmware RAM is overwritten.
   ===================================================================== */
static const char *cp_vm_status = "CP VM/GART write not run";
static uint32_t cp_vm_pass, cp_vm_step, cp_vm_rptr, cp_vm_me, cp_vm_fault_mm, cp_vm_fault_gc;
static uint32_t cp_vm_rbv, cp_vm_rbb, cp_vm_rbbhi;
static uint32_t cp_vm_cpu_pass, cp_vm_cpu_before, cp_vm_cpu_after;
static uint32_t cp_vm_pte_lo, cp_vm_pte_hi;
static uint64_t cp_vm_pte_pa;
static uint64_t cp_vm_va, cp_vm_pa;
static uint64_t cp_vm_gart_base, cp_vm_gart_end, cp_vm_pte_va;
static uint32_t cp_vm_pte_index;
/* V117: capture the exact CP VM packet and post-submit CP state. */
static uint32_t cp_vm_pkt[5];
static uint32_t cp_vm_hdr_pfp, cp_vm_hdr_me, cp_vm_hdr_ce;
static uint32_t cp_vm_busy, cp_vm_stall1, cp_vm_grbm, cp_vm_grbm2;
static uint64_t cp_vm_gart_base_snapshot;
static uint32_t cp_vm_pte_index_snapshot;
/* V121/V122: non-destructive CP physical-write control test. */
static uint32_t cp_phys_pass, cp_phys_before, cp_phys_after, cp_phys_step;
static uint64_t cp_phys_target, cp_phys_target2;
static uint32_t cp_phys2_pass, cp_phys2_before, cp_phys2_after, cp_phys2_step;
static uint32_t cp_ring_sanity_pass, cp_ring_sanity_before, cp_ring_sanity_after, cp_ring_sanity_step;
/* V124: grouped GPUVM translation probes.  Three VA/PTE indices are tested
   in one boot without changing the VMID0/PTB configuration: page 0, the
   original 0x100 page, and adjacent page 0x101. */
static uint32_t cp_vm_case_pass[3], cp_vm_case_step[3], cp_vm_case_index[3];
static uint64_t cp_vm_case_va[3], cp_vm_case_pa[3];
static uint32_t cp_vm_case_before[3], cp_vm_case_after[3];
static uint32_t cp_vm_case_dst[3], cp_vm_case_vmid[3];
static uint32_t cp_vm_case_fault_gc[3], cp_vm_case_fault_mm[3], cp_vm_case_fault_addr_lo[3], cp_vm_case_ctx[3], cp_vm_case_pt_lo[3];
static uint32_t cp_vm_vmid0_ctx, cp_vm_vmid0_pt_lo, cp_vm_vmid0_pt_hi;
static uint32_t cp_vm_vmid1_ctx, cp_vm_vmid1_pt_lo, cp_vm_vmid1_pt_hi, cp_vm_vmid1_setup_pass;
static uint32_t cp_vm_vmid1_old_ctx, cp_vm_vmid1_old_pt_lo, cp_vm_vmid1_old_pt_hi;
static uint32_t cp_vm_vmid1_after_ctx, cp_vm_vmid1_after_pt_lo, cp_vm_vmid1_after_pt_hi;
static uint32_t cp_vm_case_rb_vmid_before[3], cp_vm_case_rb_vmid_after[3];
/* V144 per-case CP/VM state captured right after the wait loop */
static uint32_t cp_vm_case_rptr[3], cp_vm_case_busy[3], cp_vm_case_stall1[3], cp_vm_case_grbm[3], cp_vm_case_l2st[3], cp_vm_case_wptr[3];
/* V139: capture fault registers immediately before and after each GPUVA probe.
   Read-only diagnostics only; no GART/PTE/VA/PA/CP state is changed here. */
static uint32_t cp_vm_case_fault_gc_before[3], cp_vm_case_fault_mm_before[3];
static uint32_t cp_vm_case_fault_addr_lo_before[3], cp_vm_case_fault_addr_hi_before[3];
static uint32_t cp_vm_case_fault_gc_after[3], cp_vm_case_fault_mm_after[3];
static uint32_t cp_vm_case_fault_addr_hi[3];

static int cp_vm_submit_case(uint64_t mmio, uint64_t ring_mc, uint64_t wb_mc,
                             uint64_t va, uint64_t phys, uint32_t target_off,
                             uint32_t pattern, uint32_t case_no, uint32_t step_base, uint32_t dst_sel, uint32_t vmid)
{
    uint32_t i, me, before, pte_programmed = 0, pte_idx = 0;
    cp_vm_case_pass[case_no] = 0;
    cp_vm_case_step[case_no] = 0;
    cp_vm_case_dst[case_no] = dst_sel;
    cp_vm_case_vmid[case_no] = vmid;
    cp_vm_case_va[case_no] = va;
    cp_vm_case_pa[case_no] = phys;
    cp_vm_case_rb_vmid_before[case_no] = gpu_mmio_read32(mmio, R_CP_RB_VMID);
    /* V139: snapshot existing fault registers before submitting the probe. */
    amd_native_fault_probe(mmio);
    cp_vm_case_fault_gc_before[case_no] = amd_native_fault_gc_status();
    cp_vm_case_fault_mm_before[case_no] = amd_native_fault_mm_status();
    cp_vm_case_fault_addr_lo_before[case_no] = amd_native_fault_gc_lo();
    cp_vm_case_fault_addr_hi_before[case_no] = amd_native_fault_gc_hi();
    if (g_case_sys) { cache_flush_line(g_sys_probe); before = *g_sys_probe; }
    else before = vram_rd32(target_off);
    cp_vm_case_before[case_no] = before;

    /* Program exactly one PTE for this probe, then flush the GFXHUB VMID0
       TLB.  The normal context/PTB setup is deliberately untouched. */
    {
        uint32_t idx = (uint32_t)((va - amd_native_gart_va()) >> 12);
        uint64_t pte = phys | PTE_MTYPE_UC | PTE_VALID | PTE_SNOOP | PTE_EXEC | PTE_READ | PTE_WRITE |
                       (g_case_sys ? PTE_SYSTEM : 0);
        uint32_t off = GART_TABLE_VRAM_OFF + idx * 8u;
        if (idx >= GART_PAGES) { cp_vm_case_step[case_no] = step_base + 1; return 0; }
        cp_vm_case_index[case_no] = idx;
        pte_idx = idx;
        pt_wr32(off, (uint32_t)pte);
        pt_wr32(off + 4u, (uint32_t)(pte >> 32));
        pte_programmed = 1;
        if (pt_rd32(off) != (uint32_t)pte || pt_rd32(off + 4u) != (uint32_t)(pte >> 32)) {
            cp_vm_case_step[case_no] = step_base + 2; goto cleanup;
        }
        if (!hub_invalidate(g_gart_mmio, &hub_gc)) { cp_vm_case_step[case_no] = step_base + 3; goto cleanup; }
    }

    /* V140: native rendering path uses kernel VMID0 only. VMID1 was a
       diagnostic isolation probe; its F500200 fault is not part of the
       production/native path. Keep VMID1 untouched and prove the actual
       kernel VMID0 path without changing the target VA/PA semantics. */
    {
        uint32_t c0 = gpu_mmio_read32(mmio, RNO_VM_CONTEXT0_CNTL_OFF);
        uint32_t oldc1 = gpu_mmio_read32(mmio, RNO_VM_CONTEXT1_CNTL_OFF);
        uint32_t oldptlo = gpu_mmio_read32(mmio, RNO_VM_CONTEXT1_PT_BASE_OFF);
        uint32_t oldpthi = gpu_mmio_read32(mmio, RNO_VM_CONTEXT1_PT_BASE_OFF + 4u);
        cp_vm_vmid1_old_ctx = oldc1; cp_vm_vmid1_old_pt_lo = oldptlo; cp_vm_vmid1_old_pt_hi = oldpthi;
        cp_vm_vmid0_ctx = c0;
        cp_vm_vmid0_pt_lo = gpu_mmio_read32(mmio, RNO_VM_CONTEXT0_PT_BASE_OFF);
        cp_vm_vmid0_pt_hi = gpu_mmio_read32(mmio, RNO_VM_CONTEXT0_PT_BASE_OFF + 4u);
        if (vmid == 1u) {
            uint32_t c1 = (c0 & ~0xFu) | 1u;
            uint32_t ptlo = gpu_mmio_read32(mmio, RNO_VM_CONTEXT0_PT_BASE_OFF);
            uint32_t pthi = gpu_mmio_read32(mmio, RNO_VM_CONTEXT0_PT_BASE_OFF + 4u);
            gpu_mmio_write32(mmio, RNO_VM_CONTEXT1_PT_BASE_OFF, ptlo);
            gpu_mmio_write32(mmio, RNO_VM_CONTEXT1_PT_BASE_OFF + 4u, pthi);
            gpu_mmio_write32(mmio, RNO_VM_CONTEXT1_CNTL_OFF, c1);
            cp_vm_vmid1_pt_lo = gpu_mmio_read32(mmio, RNO_VM_CONTEXT1_PT_BASE_OFF);
            cp_vm_vmid1_pt_hi = gpu_mmio_read32(mmio, RNO_VM_CONTEXT1_PT_BASE_OFF + 4u);
            cp_vm_vmid1_ctx = gpu_mmio_read32(mmio, RNO_VM_CONTEXT1_CNTL_OFF);
            cp_vm_vmid1_setup_pass = ((cp_vm_vmid1_ctx & 1u) != 0u &&
                                      cp_vm_vmid1_pt_lo == ptlo && cp_vm_vmid1_pt_hi == pthi);
            if (!cp_vm_vmid1_setup_pass) { cp_vm_case_step[case_no] = step_base + 14; goto cleanup; }
        }
    }

    /* Reinitialize the same known-good CP ring path used by V123. */
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, me | CP_ME_HALT_BITS | R_ME_CNTL_PIPE_RESET);
    cp_delay_us(mmio, 100);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, me | CP_ME_HALT_BITS);
    cp_delay_us(mmio, 100);
    if (!cp_wr_ok(mmio, R_CP_RB_VMID, vmid, 0xFFFFFFFFu, step_base + 4)) goto cleanup;
    gpu_mmio_write32(mmio, R_CP_RB0_CNTL, (9u | (7u << 8)) | (1u << 31));
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR, (uint32_t)wb_mc);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR_HI, (uint32_t)(wb_mc >> 32) & 0xFFFFu);
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_LO, (uint32_t)(wb_mc + 8u));
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_HI, (uint32_t)((wb_mc + 8u) >> 32));
    if (!cp_wr_ok(mmio, R_CP_RB0_CNTL, (9u | (7u << 8)), 0x3FFFu, step_base + 5)) goto cleanup;
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE, (uint32_t)(ring_mc >> 8), 0xFFFFFFFFu, step_base + 6)) goto cleanup;
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE_HI, (uint32_t)((ring_mc >> 8) >> 32), 0xFFFFFFFFu, step_base + 7)) goto cleanup;
    { uint32_t db = gpu_mmio_read32(mmio, R_CP_DOORBELL_CTRL);
      if (!cp_wr_ok(mmio, R_CP_DOORBELL_CTRL, db & ~(1u << 30), 1u << 30, step_base + 8)) goto cleanup; }
    if (!cp_wr_ok(mmio, R_CP_MAX_CONTEXT, 7u, 0xFFFFFFFFu, step_base + 9)) goto cleanup;
    if (!cp_wr_ok(mmio, R_CP_DEVICE_ID, 1u, 0xFFFFFFFFu, step_base + 10)) goto cleanup;
    if (!cp_wr_ok(mmio, R_SCRATCH_REG0, 0xCAFEDEADu, 0xFFFFFFFFu, step_base + 11)) goto cleanup;

    cpm_wr(0u, 0xC0033700u);
    cpm_wr(4u, (dst_sel << 8) | (1u << 20));
    cpm_wr(8u, (uint32_t)va);
    cpm_wr(12u, (uint32_t)(va >> 32));
    cpm_wr(16u, pattern);
    (void)cpm_rd(0x1FFCu);
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    if (!cp_wr_ok(mmio, R_CP_ME_CNTL, me & ~CP_ME_HALT_BITS, CP_ME_HALT_BITS, step_base + 12)) goto cleanup;
    cp_delay_us(mmio, 50);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 5u);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0u);
    for (i = 0; i < 200000u; i++) {
        if (g_case_sys) { cache_flush_line(g_sys_probe); cp_vm_case_after[case_no] = *g_sys_probe; }
        else cp_vm_case_after[case_no] = vram_rd32(target_off);
        if (cp_vm_case_after[case_no] == pattern) { cp_vm_case_pass[case_no] = 1; break; }
    }
    cp_vm_case_rptr[case_no]   = gpu_mmio_read32(mmio, R_CP_RB0_RPTR);
    cp_vm_case_wptr[case_no]   = gpu_mmio_read32(mmio, R_CP_RB0_WPTR);
    cp_vm_case_busy[case_no]   = gpu_mmio_read32(mmio, R_CP_BUSY_STAT);
    cp_vm_case_stall1[case_no] = gpu_mmio_read32(mmio, R_CP_STALLED_STAT1);
    cp_vm_case_grbm[case_no]   = gpu_mmio_read32(mmio, GC_GRBM_STATUS_OFF);
    cp_vm_case_l2st[case_no]   = gpu_mmio_read32(mmio, RNO_GC_BYTE(0x0843u));
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, gpu_mmio_read32(mmio, R_CP_ME_CNTL) | CP_ME_HALT_BITS);
    cp_delay_us(mmio, 100);
    if (!cp_vm_case_pass[case_no] && !cp_vm_case_step[case_no]) cp_vm_case_step[case_no] = step_base + 13;
    /* V128: capture fault state immediately after this individual probe,
       before the next probe can overwrite the global GFXHUB fault registers. */
    amd_native_fault_probe(mmio);
    cp_vm_case_fault_mm[case_no] = amd_native_fault_mm_status();
    cp_vm_case_fault_gc[case_no] = amd_native_fault_gc_status();
    cp_vm_case_fault_gc_after[case_no] = cp_vm_case_fault_gc[case_no];
    cp_vm_case_fault_mm_after[case_no] = cp_vm_case_fault_mm[case_no];
    cp_vm_case_fault_addr_lo[case_no] = f_gc_lo;
    cp_vm_case_fault_addr_hi[case_no] = amd_native_fault_gc_hi();
    cp_vm_case_ctx[case_no] = (vmid == 1u) ? cp_vm_vmid1_ctx : gpu_mmio_read32(mmio, RNO_VM_CONTEXT0_CNTL_OFF);
    cp_vm_case_pt_lo[case_no] = (vmid == 1u) ? cp_vm_vmid1_pt_lo : gpu_mmio_read32(mmio, RNO_VM_CONTEXT0_PT_BASE_OFF);
    cp_vm_case_rb_vmid_after[case_no] = gpu_mmio_read32(mmio, R_CP_RB_VMID);
cleanup:
    if (vmid == 1u) {
        /* Restore VMID1 context exactly as found. */
        gpu_mmio_write32(mmio, RNO_VM_CONTEXT1_CNTL_OFF, cp_vm_vmid1_old_ctx);
        gpu_mmio_write32(mmio, RNO_VM_CONTEXT1_PT_BASE_OFF, cp_vm_vmid1_old_pt_lo);
        gpu_mmio_write32(mmio, RNO_VM_CONTEXT1_PT_BASE_OFF + 4u, cp_vm_vmid1_old_pt_hi);
        cp_vm_vmid1_after_ctx = gpu_mmio_read32(mmio, RNO_VM_CONTEXT1_CNTL_OFF);
        cp_vm_vmid1_after_pt_lo = gpu_mmio_read32(mmio, RNO_VM_CONTEXT1_PT_BASE_OFF);
        cp_vm_vmid1_after_pt_hi = gpu_mmio_read32(mmio, RNO_VM_CONTEXT1_PT_BASE_OFF + 4u);
        (void)hub_invalidate(g_gart_mmio, &hub_gc);
    }
    /* Restore the PTE for this test page and flush again even on an early
       register/invalidate failure, so the next grouped probe starts clean. */
    if (pte_programmed) {
        uint32_t idx = pte_idx; uint32_t off = GART_TABLE_VRAM_OFF + idx * 8u;
        uint64_t restore = 0;
        if (idx == 0x100u) restore = cp_vm_pte_pa | PTE_MTYPE_UC | PTE_VALID | PTE_SNOOP | PTE_EXEC | PTE_READ | PTE_WRITE;
        pt_wr32(off, (uint32_t)restore); pt_wr32(off + 4u, (uint32_t)(restore >> 32));
        (void)hub_invalidate(g_gart_mmio, &hub_gc);
    }
    if (g_case_sys) { *g_sys_probe = before; cache_flush_line(g_sys_probe); }
    else vram_wr32(target_off, before);
    return (int)cp_vm_case_pass[case_no];
}

int amd_native_cp_vm_gart_test(uint64_t mmio, uint64_t cpu_vram, int fw_ready)
{
    uint64_t ring_mc, wb_mc, va;
    uint32_t me, i, target_before, target_after;
    const uint32_t physical_off = 0x00100000u;
    cp_vm_pass = cp_vm_step = cp_vm_rptr = cp_vm_me = cp_vm_fault_mm = cp_vm_fault_gc = 0;
    cp_vm_rbv = cp_vm_rbb = cp_vm_rbbhi = 0;
    cp_vm_cpu_pass = 0;
    cp_vm_cpu_before = cp_vm_cpu_after = 0;
    cp_vm_pte_lo = cp_vm_pte_hi = 0;
    cp_vm_pte_pa = 0;
    cp_vm_va = cp_vm_pa = 0;
    cp_vm_gart_base = cp_vm_gart_end = cp_vm_pte_va = 0;
    cp_vm_pte_index = 0;
    for (i = 0; i < 5u; i++) cp_vm_pkt[i] = 0;
    cp_vm_hdr_pfp = cp_vm_hdr_me = cp_vm_hdr_ce = 0;
    cp_vm_busy = cp_vm_stall1 = cp_vm_grbm = cp_vm_grbm2 = 0;
    cp_vm_gart_base_snapshot = 0;
    cp_vm_pte_index_snapshot = 0;
    cp_phys_pass = cp_phys_before = cp_phys_after = cp_phys_step = 0;
    cp_phys_target = cp_phys_target2 = 0;
    cp_phys2_pass = cp_phys2_before = cp_phys2_after = cp_phys2_step = 0;
    cp_ring_sanity_pass = cp_ring_sanity_before = cp_ring_sanity_after = cp_ring_sanity_step = 0;
    cp_vm_status = "CP VM/GART write not run";

    if (!fw_ready) { cp_vm_status = "CP VM/GART skipped: CE/PFP/ME not accepted"; return 0; }
    if (!gpu_mmio_can_access(mmio) || !gpu_mmio_can_access(cpu_vram)) {
        cp_vm_status = "CP VM/GART skipped: MMIO/VRAM aperture unavailable"; return 0;
    }
    va = amd_native_gart_va();
    if (!va) {
        cp_vm_status = "CP VM/GART skipped: GART mapping[0] not active"; return 0;
    }
    ring_mc = amd_native_vram_mc_addr(CP_RING_VRAM_OFF, CP_RING_BYTES);
    wb_mc   = amd_native_vram_mc_addr(CP_WB_VRAM_OFF, 0x1000u);
    if (!ring_mc || !wb_mc) { cp_vm_status = "CP VM/GART skipped: VRAM MC address unavailable"; return 0; }

    /* CPU-visible destination is the BAR-relative VRAM offset; the PTE
       itself must contain the GPU/MC physical address including FB base. */
    g_cpu_vram = cpu_vram;
    {
        uint32_t pte_index = physical_off >> 12;
        uint64_t pte_va = va + (uint64_t)physical_off;
        uint64_t pte_pa = gart_cp_probe_pa;
        /* V110: mapping was preloaded before VMID0 enable and flushed by
           hub_enable(). Only verify the exact PTE here; do not rewrite it
           after context activation. */
        if (pte_index >= GART_PAGES || !pte_pa || !gart_table_ready) {
            cp_vm_step = 1;
            cp_vm_status = "CP VM/GART abort: preloaded PTE unavailable";
            return 0;
        }
        va = pte_va;
        cp_vm_pa = pte_pa;
        /* V104: CPU-visible GART preflight.  Do not dereference the GPU VA
           from the CPU: GART is a GPU VM address space, not a CPU pointer.
           Instead read back the exact PTE[0x100] from the BAR-visible page
           table, decode its physical address, then prove that the BAR-visible
           physical VRAM destination itself is CPU writable. */
        {
            uint32_t pte_off = GART_TABLE_VRAM_OFF + pte_index * 8u;
            uint64_t pte = (uint64_t)pt_rd32(pte_off) |
                           ((uint64_t)pt_rd32(pte_off + 4u) << 32);
            cp_vm_pte_lo = (uint32_t)pte;
            cp_vm_pte_hi = (uint32_t)(pte >> 32);
            cp_vm_pte_pa = pte & PTE_ADDR_MASK;
            cp_vm_cpu_before = vram_rd32(physical_off);
            vram_wr32(physical_off, 0x13579BDFu);
            cp_vm_cpu_after = vram_rd32(physical_off);
            cp_vm_cpu_pass = (cp_vm_pte_pa == pte_pa &&
                              (pte & (PTE_VALID | PTE_READ | PTE_WRITE)) ==
                              (PTE_VALID | PTE_READ | PTE_WRITE) &&
                              cp_vm_cpu_after == 0x13579BDFu);
            vram_wr32(physical_off, cp_vm_cpu_before);
        }
        cp_vm_gart_base = amd_native_gart_va();
        cp_vm_gart_base_snapshot = cp_vm_gart_base;
        cp_vm_gart_end = cp_vm_gart_base + (uint64_t)GART_PAGES * 4096ULL - 1ULL;
        cp_vm_pte_va = pte_va;
        cp_vm_pte_index = pte_index;
        cp_vm_pte_index_snapshot = pte_index;
    }
    target_before = vram_rd32(physical_off);
    vram_wr32(physical_off, 0x13579BDFu);
    cp_vm_cpu_after = vram_rd32(physical_off);
    cp_vm_cpu_pass = cp_vm_cpu_pass && (cp_vm_cpu_after == 0x13579BDFu);
    if (!cp_vm_cpu_pass) {
        cp_vm_step = 1; cp_vm_status = "CP VM/GART abort: CPU BAR/PTE preflight failed"; return 0;
    }

    /* V122: use the exact V92/V121 CP ring initialization sequence, including
       RB0_CNTL, DOORBELL, MAX_CONTEXT, DEVICE_ID and SCRATCH before ME release.
       V121 stopped at ME release (step 6), so that was not yet a physical-write
       test. First run a harmless proven WRDATA-to-WB sanity operation. */
    for (i = 0; i < 0x2000u / 4u; i++) cpm_wr(i * 4u, 0);
    (void)cpm_rd(0x1FFCu);
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, me | CP_ME_HALT_BITS | R_ME_CNTL_PIPE_RESET);
    cp_delay_us(mmio, 100);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, (me | CP_ME_HALT_BITS) & ~R_ME_CNTL_PIPE_RESET);
    cp_delay_us(mmio, 100);
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    if ((me & CP_ME_HALT_BITS) != CP_ME_HALT_BITS) { cp_ring_sanity_step = 99; cp_vm_step = 2; goto out; }
    if (!cp_wr_ok(mmio, R_CP_RB0_WPTR_DELAY, 0, 0xFFFFFFFFu, 1)) { cp_ring_sanity_step = 1; cp_vm_step = 2; goto out; }
    if (!cp_wr_ok(mmio, R_CP_RB_VMID, 0u, 0xFFFFFFFFu, 2)) { cp_ring_sanity_step = 2; cp_vm_step = 2; goto out; }
    gpu_mmio_write32(mmio, R_CP_RB0_CNTL, (9u | (7u << 8)) | (1u << 31));
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR, (uint32_t)wb_mc);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR_HI, (uint32_t)(wb_mc >> 32) & 0xFFFFu);
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_LO, (uint32_t)(wb_mc + 8u));
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_HI, (uint32_t)((wb_mc + 8u) >> 32));
    if (!cp_wr_ok(mmio, R_CP_RB0_CNTL, (9u | (7u << 8)), 0x3FFFu, 3)) { cp_ring_sanity_step = 3; cp_vm_step = 2; goto out; }
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE, (uint32_t)(ring_mc >> 8), 0xFFFFFFFFu, 4)) { cp_ring_sanity_step = 4; cp_vm_step = 2; goto out; }
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE_HI, (uint32_t)((ring_mc >> 8) >> 32), 0xFFFFFFFFu, 5)) { cp_ring_sanity_step = 5; cp_vm_step = 2; goto out; }
    { uint32_t db = gpu_mmio_read32(mmio, R_CP_DOORBELL_CTRL); if (!cp_wr_ok(mmio, R_CP_DOORBELL_CTRL, db & ~(1u << 30), 1u << 30, 6)) { cp_ring_sanity_step = 6; cp_vm_step = 2; goto out; } }
    if (!cp_wr_ok(mmio, R_CP_MAX_CONTEXT, 7u, 0xFFFFFFFFu, 7)) { cp_ring_sanity_step = 7; cp_vm_step = 2; goto out; }
    if (!cp_wr_ok(mmio, R_CP_DEVICE_ID, 1u, 0xFFFFFFFFu, 8)) { cp_ring_sanity_step = 8; cp_vm_step = 2; goto out; }
    if (!cp_wr_ok(mmio, R_SCRATCH_REG0, 0xCAFEDEADu, 0xFFFFFFFFu, 10)) { cp_ring_sanity_step = 10; cp_vm_step = 2; goto out; }

    cp_ring_sanity_before = vram_rd32(CP_WB_VRAM_OFF + CPV_WB_TEST_OFF);
    cpm_wr(0u, 0xC0033700u); cpm_wr(4u, (5u << 8) | (1u << 20));
    cpm_wr(8u, (uint32_t)(wb_mc + CPV_WB_TEST_OFF));
    cpm_wr(12u, (uint32_t)((wb_mc + CPV_WB_TEST_OFF) >> 32));
    cpm_wr(16u, 0xA55A5AA5u); (void)cpm_rd(0x1FFCu);
    if (!cp_wr_ok(mmio, R_CP_ME_CNTL, me & ~CP_ME_HALT_BITS, CP_ME_HALT_BITS, 9)) { cp_ring_sanity_step = 9; cp_vm_step = 2; goto out; }
    cp_delay_us(mmio, 50); gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 5u); gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0u);
    for (i = 0; i < 200000u; i++) { cp_ring_sanity_after = vram_rd32(CP_WB_VRAM_OFF + CPV_WB_TEST_OFF); if (cp_ring_sanity_after == 0xA55A5AA5u) { cp_ring_sanity_pass = 1; break; } }
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, gpu_mmio_read32(mmio, R_CP_ME_CNTL) | CP_ME_HALT_BITS); cp_delay_us(mmio, 100);

    /* Rebuild the ring exactly as above, then TEST A: CP -> MC physical. */
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, me | CP_ME_HALT_BITS | R_ME_CNTL_PIPE_RESET); cp_delay_us(mmio,100);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, (me | CP_ME_HALT_BITS) & ~R_ME_CNTL_PIPE_RESET); cp_delay_us(mmio,100); me=gpu_mmio_read32(mmio,R_CP_ME_CNTL);
    if (!cp_wr_ok(mmio,R_CP_RB_VMID,0u,0xFFFFFFFFu,20)) { cp_vm_step=20; goto out; }
    gpu_mmio_write32(mmio,R_CP_RB0_CNTL,(9u|(7u<<8))|(1u<<31)); gpu_mmio_write32(mmio,R_CP_RB0_RPTR,0); gpu_mmio_write32(mmio,R_CP_RB0_WPTR,0); gpu_mmio_write32(mmio,R_CP_RB0_WPTR_HI,0);
    gpu_mmio_write32(mmio,R_CP_RB0_RPTR_ADDR,(uint32_t)wb_mc); gpu_mmio_write32(mmio,R_CP_RB0_RPTR_ADDR_HI,(uint32_t)(wb_mc>>32)&0xFFFFu); gpu_mmio_write32(mmio,R_CP_WPTR_POLL_LO,(uint32_t)(wb_mc+8)); gpu_mmio_write32(mmio,R_CP_WPTR_POLL_HI,(uint32_t)((wb_mc+8)>>32));
    if (!cp_wr_ok(mmio,R_CP_RB0_CNTL,(9u|(7u<<8)),0x3FFFu,21)) { cp_vm_step=21; goto out; }
    if (!cp_wr_ok(mmio,R_CP_RB0_BASE,(uint32_t)(ring_mc>>8),0xFFFFFFFFu,22)) { cp_vm_step=22; goto out; }
    if (!cp_wr_ok(mmio,R_CP_RB0_BASE_HI,(uint32_t)((ring_mc>>8)>>32),0xFFFFFFFFu,23)) { cp_vm_step=23; goto out; }
    { uint32_t db=gpu_mmio_read32(mmio,R_CP_DOORBELL_CTRL); if(!cp_wr_ok(mmio,R_CP_DOORBELL_CTRL,db&~(1u<<30),1u<<30,24)){cp_vm_step=24;goto out;} }
    if (!cp_wr_ok(mmio,R_CP_MAX_CONTEXT,7u,0xFFFFFFFFu,25)) {cp_vm_step=25;goto out;} if(!cp_wr_ok(mmio,R_CP_DEVICE_ID,1u,0xFFFFFFFFu,26)){cp_vm_step=26;goto out;}
    if (!cp_wr_ok(mmio,R_SCRATCH_REG0,0xCAFEDEADu,0xFFFFFFFFu,27)){cp_vm_step=27;goto out;}

    /* V123 TEST A: use the already-proven CP writeback MC address.
       This isolates CP physical-address encoding from the GPUVM/GART target. */
    cp_phys_target = wb_mc + CPV_WB_TEST_OFF;
    cp_phys_before = vram_rd32(CP_WB_VRAM_OFF + CPV_WB_TEST_OFF);
    cpm_wr(0u,  0xC0033700u);
    cpm_wr(4u,  (5u << 8) | (1u << 20));
    cpm_wr(8u,  (uint32_t)cp_phys_target);
    cpm_wr(12u, (uint32_t)(cp_phys_target >> 32));
    cpm_wr(16u, 0xB16B00B5u);
    (void)cpm_rd(0x1FFCu);
    if (!cp_wr_ok(mmio, R_CP_ME_CNTL, me & ~CP_ME_HALT_BITS, CP_ME_HALT_BITS, 29)) { cp_phys_step = 29; goto vm_setup; }
    cp_delay_us(mmio, 50);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 5u);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0u);
    for (i = 0; i < 200000u; i++) {
        cp_phys_after = vram_rd32(CP_WB_VRAM_OFF + CPV_WB_TEST_OFF);
        if (cp_phys_after == 0xB16B00B5u) { cp_phys_pass = 1; break; }
    }
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, gpu_mmio_read32(mmio, R_CP_ME_CNTL) | CP_ME_HALT_BITS);
    cp_delay_us(mmio, 100);
    /* Second physical-address test: the actual VM target's MC physical address,
       but accessed as a CP physical destination. This contrasts with the
       already-proven writeback physical address without changing GART/PTE. */
    cp_phys_target2 = cp_vm_pa;
    cp_phys2_before = vram_rd32(physical_off);
    cpm_wr(0u,  0xC0033700u);
    cpm_wr(4u,  (5u << 8) | (1u << 20));
    cpm_wr(8u,  (uint32_t)cp_phys_target2);
    cpm_wr(12u, (uint32_t)(cp_phys_target2 >> 32));
    cpm_wr(16u, 0xB16B00B6u);
    (void)cpm_rd(0x1FFCu);
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    if (cp_wr_ok(mmio, R_CP_ME_CNTL, me & ~CP_ME_HALT_BITS, CP_ME_HALT_BITS, 30)) {
        cp_delay_us(mmio, 50);
        gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 5u);
        gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0u);
        for (i = 0; i < 200000u; i++) {
            cp_phys2_after = vram_rd32(physical_off);
            if (cp_phys2_after == 0xB16B00B6u) { cp_phys2_pass = 1; break; }
        }
        gpu_mmio_write32(mmio, R_CP_ME_CNTL, gpu_mmio_read32(mmio, R_CP_ME_CNTL) | CP_ME_HALT_BITS);
        cp_delay_us(mmio, 100);
    } else { cp_phys2_step = 30; }
    /* Restore both physical test markers before the VM test. */
    vram_wr32(CP_WB_VRAM_OFF + CPV_WB_TEST_OFF, cp_phys_before);
    vram_wr32(physical_off, cp_phys2_before);

vm_setup:
    /* V126: use the known-correct WRITE_DATA memory destination (DST_SEL=5)
       only.  V126 showed that comparing DST_SEL=1/5/4 is not useful here:
       Linux/amdgpu uses DST_SEL=5 for a memory destination on this packet.
       Now vary ONLY the GPU VA/PTE page and verify the matching physical
       target, while leaving VMID0/PTB/context configuration untouched. */
    cp_vm_case_index[0] = cp_vm_case_index[1] = cp_vm_case_index[2] = 0;
    cp_vm_case_va[0] = cp_vm_gart_base + 0x000000ULL;
    cp_vm_case_va[1] = cp_vm_gart_base + 0x100000ULL;
    cp_vm_case_va[2] = cp_vm_gart_base + 0x100000ULL;
    cp_vm_case_pa[0] = cp_vm_pa;
    cp_vm_case_pa[1] = cp_vm_pa;
    cp_vm_case_pa[2] = cp_vm_pa;
    cp_vm_case_before[0] = cp_vm_case_before[1] = cp_vm_case_before[2] = 0;
    cp_vm_case_after[0] = cp_vm_case_after[1] = cp_vm_case_after[2] = 0;
    cp_vm_case_pass[0] = cp_vm_case_pass[1] = cp_vm_case_pass[2] = 0;
    cp_vm_case_fault_gc[0] = cp_vm_case_fault_gc[1] = cp_vm_case_fault_gc[2] = 0;
    cp_vm_case_fault_mm[0] = cp_vm_case_fault_mm[1] = cp_vm_case_fault_mm[2] = 0;
    cp_vm_case_fault_addr_lo[0] = cp_vm_case_fault_addr_lo[1] = cp_vm_case_fault_addr_lo[2] = 0;
    cp_vm_case_vmid[0] = cp_vm_case_vmid[1] = cp_vm_case_vmid[2] = 0;
    cp_vm_case_step[0] = cp_vm_case_step[1] = cp_vm_case_step[2] = 0;
    cp_vm_case_dst[0] = cp_vm_case_dst[1] = cp_vm_case_dst[2] = 5;

    (void)cp_vm_submit_case(mmio, ring_mc, wb_mc,
        cp_vm_case_va[0], cp_vm_case_pa[0], physical_off, 0xC0DE5000u, 0, 300, 5, 0);
    (void)cp_vm_submit_case(mmio, ring_mc, wb_mc,
        cp_vm_case_va[1], cp_vm_case_pa[1], physical_off, 0xC0DE5100u, 1, 330, 5, 0);
    /* V141: case 2 (formerly the unused VMID1 slot) is now a GPUVA write
       whose PTE has SYSTEM=1 and points at a kernel-RAM page whose physical
       address is known exactly (no MC_VM_FB_OFFSET involved).
       Reading the three rows together:
         T1 FAIL, T2 PASS -> page-table walk works; FB_OFFSET/VRAM PA wrong.
         T1 FAIL, T2 FAIL -> walker/GFXHUB config or CP VMID problem.
         T1 PASS          -> VRAM GART path works. */
    if (g_sys_probe) {
        g_case_sys = 1;
        cp_vm_case_va[2] = cp_vm_gart_base + 0x101000ULL;
        cp_vm_case_pa[2] = g_sys_probe_pa;
        *g_sys_probe = 0x13579BDFu; cache_flush_line(g_sys_probe);
        (void)cp_vm_submit_case(mmio, ring_mc, wb_mc,
            cp_vm_case_va[2], cp_vm_case_pa[2], 0, 0xC0DE5200u, 2, 360, 5, 0);
        g_case_sys = 0;
    } else {
        cp_vm_case_pass[2] = 0; cp_vm_case_step[2] = 361; cp_vm_case_vmid[2] = 0;
    }

    /* Legacy summary remains the middle/current mode (VA +0x100000). */
    cp_vm_pass = cp_vm_case_pass[1];
    cp_vm_step = cp_vm_case_step[1];
    cp_vm_va = cp_vm_case_va[1];
    cp_vm_pa = cp_vm_case_pa[1];
    target_before = cp_vm_case_before[1];
    target_after = cp_vm_case_after[1];

out:
    cp_vm_rptr = gpu_mmio_read32(mmio, R_CP_RB0_RPTR);
    cp_vm_hdr_pfp = gpu_mmio_read32(mmio, R_CP_PFP_HDR);
    cp_vm_hdr_me  = gpu_mmio_read32(mmio, R_CP_ME_HDR);
    cp_vm_hdr_ce  = gpu_mmio_read32(mmio, R_CP_CE_HDR);
    cp_vm_busy = gpu_mmio_read32(mmio, R_CP_BUSY_STAT);
    cp_vm_stall1 = gpu_mmio_read32(mmio, R_CP_STALLED_STAT1);
    cp_vm_grbm = gpu_mmio_read32(mmio, GC_GRBM_STATUS_OFF);
    cp_vm_grbm2 = gpu_mmio_read32(mmio, R_GRBM_STAT2);
    cp_vm_rbv = gpu_mmio_read32(mmio, R_CP_RB_VMID);
    cp_vm_rbb = gpu_mmio_read32(mmio, R_CP_RB0_BASE);
    cp_vm_rbbhi = gpu_mmio_read32(mmio, R_CP_RB0_BASE_HI);
    cp_vm_me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    amd_native_fault_probe(mmio);
    cp_vm_fault_mm = amd_native_fault_mm_status();
    cp_vm_fault_gc = amd_native_fault_gc_status();
    target_after = vram_rd32(physical_off);
    cp_vm_va = va;
    if (!cp_vm_pa) cp_vm_pa = amd_native_cp_vm_mc_addr(physical_off, 0x1000u);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, gpu_mmio_read32(mmio, R_CP_ME_CNTL) | CP_ME_HALT_BITS);
    cp_delay_us(mmio, 100);
    if (cp_vm_pass) {
        cp_vm_step = 0;
        cp_vm_status = "Renoir native GFX CP active: VMID0 GART write PASS";
    } else if (!cp_vm_step) {
        cp_vm_step = 7;
        cp_vm_status = "CP VM/GART WRITE TIMEOUT: packet did not reach mapped PA";
    } else {
        cp_vm_status = "CP VM/GART WRITE ABORTED";
    }
    /* Restore the original CPU-visible contents when possible. */
    vram_wr32(physical_off, target_before);
    (void)target_after;
    return (int)cp_vm_pass;
}

/* ------------------------------------------------------------------ */
/* V146/V147: INDIRECT_BUFFER + fence tests.
   Ring:  INDIRECT_BUFFER(ib) ; WRITE_DATA fenceB ; RELEASE_MEM fenceC
   IB:    WRITE_DATA marker ; NOP padding (IB length is a multiple of 8 dw)
   mode 0: IB (VRAM) and fences (CP write-back page) via physical MC addresses.
   mode 1: IB (VRAM, GART page 0x102) and fences (kernel RAM, SYSTEM PTE,
           GART page 0x103) are ALL addressed by GART virtual address, VMID0.
   The ring itself stays at its physical MC address in both modes.        */
#define CP_IB_VRAM_OFF     0x0A002000u
#define CPIB_MARKER_OFF    0x100u
#define CPIB_FENCEB_OFF    0x104u
#define CPIB_FENCEC_OFF    0x108u
#define CPIB_MARKER_VAL    0xF00DCAFEu
#define CPIB_FENCEB_VAL    0xFE4CE001u
#define CPIB_FENCEC_VAL    0xFE4CE002u
#define CPIB_VA_IB_IDX     0x102u
#define CPIB_VA_FENCE_IDX  0x103u
typedef struct {
    uint32_t ran, pass, step, rptr, wptr, marker, fenceb, fencec, busy, grbm, stall1;
    const char *status;
} CpIbResult;
static CpIbResult cpib[2] = { {0,0,0,0,0,0,0,0,0,0,0,"IB/fence (physical) not run"},
                              {0,0,0,0,0,0,0,0,0,0,0,"IB/fence (GART VA) not run"} };

static void cpib_pte_set(uint32_t idx, uint64_t pa, int sys, int valid)
{
    uint32_t off = GART_TABLE_VRAM_OFF + idx * 8u;
    uint64_t pte = valid ? (pa | PTE_MTYPE_UC | PTE_VALID | PTE_SNOOP | PTE_EXEC | PTE_READ | PTE_WRITE |
                            (sys ? PTE_SYSTEM : 0)) : 0;
    pt_wr32(off, (uint32_t)pte);
    pt_wr32(off + 4u, (uint32_t)(pte >> 32));
}
static uint32_t cpib_rd_fence(int mode, uint32_t off)
{
    if (mode) { volatile uint32_t *p = &g_sys_probe[off >> 2]; cache_flush_line(p); return *p; }
    return vram_rd32(CP_WB_VRAM_OFF + off);
}
static void cpib_wr_fence(int mode, uint32_t off, uint32_t v)
{
    if (mode) { volatile uint32_t *p = &g_sys_probe[off >> 2]; *p = v; cache_flush_line(p); }
    else vram_wr32(CP_WB_VRAM_OFF + off, v);
}

static int cp_ib_run(uint64_t mmio, int mode)
{
    CpIbResult *r = &cpib[mode];
    uint64_t ring_mc, wb_mc, ib_addr, a_marker, a_fb, a_fc;
    uint32_t i, me, w = 0, ibw = 0;
    r->ran = 1; r->pass = 0; r->step = 0; r->marker = r->fenceb = r->fencec = 0;
    if (!gpu_mmio_can_access(mmio) || !g_cpu_vram) { r->status = "IB test skipped: no MMIO/VRAM"; r->step = 1; return 0; }
    ring_mc = amd_native_vram_mc_addr(CP_RING_VRAM_OFF, CP_RING_BYTES);
    wb_mc   = amd_native_vram_mc_addr(CP_WB_VRAM_OFF, 0x1000u);
    if (!ring_mc || !wb_mc) { r->status = "IB test skipped: no MC address"; r->step = 2; return 0; }
    if (mode == 0) {
        ib_addr  = amd_native_vram_mc_addr(CP_IB_VRAM_OFF, 0x1000u);
        a_marker = wb_mc + CPIB_MARKER_OFF; a_fb = wb_mc + CPIB_FENCEB_OFF; a_fc = wb_mc + CPIB_FENCEC_OFF;
        if (!ib_addr) { r->status = "IB test skipped: no MC address"; r->step = 2; return 0; }
    } else {
        uint64_t va_ib, va_fence, pa_ib;
        if (!g_sys_probe || !amd_native_gart_va()) { r->status = "IB VA test skipped: no GART/sysmem page"; r->step = 3; return 0; }
        va_ib    = amd_native_gart_va() + (uint64_t)CPIB_VA_IB_IDX * 0x1000ULL;
        va_fence = amd_native_gart_va() + (uint64_t)CPIB_VA_FENCE_IDX * 0x1000ULL;
        pa_ib    = amd_native_cp_vm_mc_addr(CP_IB_VRAM_OFF, 0x1000u);
        cpib_pte_set(CPIB_VA_IB_IDX, pa_ib, 0, 1);
        cpib_pte_set(CPIB_VA_FENCE_IDX, g_sys_probe_pa, 1, 1);
        if (!hub_invalidate(g_gart_mmio, &hub_gc)) { r->status = "IB VA test aborted: TLB invalidate failed"; r->step = 4; goto unmap; }
        ib_addr = va_ib;
        a_marker = va_fence + CPIB_MARKER_OFF; a_fb = va_fence + CPIB_FENCEB_OFF; a_fc = va_fence + CPIB_FENCEC_OFF;
    }

    for (i = 0; i < CP_RING_BYTES / 4u; i++) vram_wr32(CP_RING_VRAM_OFF + i * 4u, 0xFFFF1000u);
    for (i = 0; i < 0x1000u / 4u; i++)       vram_wr32(CP_IB_VRAM_OFF + i * 4u, 0xFFFF1000u);
    cpib_wr_fence(mode, CPIB_MARKER_OFF, 0);
    cpib_wr_fence(mode, CPIB_FENCEB_OFF, 0);
    cpib_wr_fence(mode, CPIB_FENCEC_OFF, 0);

    /* --- indirect buffer: WRITE_DATA marker + NOP padding to 8 dwords --- */
    vram_wr32(CP_IB_VRAM_OFF + 4u * ibw++, 0xC0033700u);
    vram_wr32(CP_IB_VRAM_OFF + 4u * ibw++, (5u << 8) | (1u << 20));
    vram_wr32(CP_IB_VRAM_OFF + 4u * ibw++, (uint32_t)a_marker);
    vram_wr32(CP_IB_VRAM_OFF + 4u * ibw++, (uint32_t)(a_marker >> 32));
    vram_wr32(CP_IB_VRAM_OFF + 4u * ibw++, CPIB_MARKER_VAL);
    while (ibw < 8u) vram_wr32(CP_IB_VRAM_OFF + 4u * ibw++, 0xFFFF1000u);

    /* --- ring --- */
    cpm_wr(4u * w++, 0xC0023F00u);                         /* INDIRECT_BUFFER, 3 dw body */
    cpm_wr(4u * w++, (uint32_t)ib_addr & 0xFFFFFFFCu);
    cpm_wr(4u * w++, (uint32_t)(ib_addr >> 32));
    cpm_wr(4u * w++, ibw | (0u << 24));                    /* IB_SIZE dw | VMID0 */
    cpm_wr(4u * w++, 0xC0033700u);                         /* WRITE_DATA fence B */
    cpm_wr(4u * w++, (5u << 8) | (1u << 20));
    cpm_wr(4u * w++, (uint32_t)a_fb);
    cpm_wr(4u * w++, (uint32_t)(a_fb >> 32));
    cpm_wr(4u * w++, CPIB_FENCEB_VAL);
    cpm_wr(4u * w++, 0xC0064900u);                         /* RELEASE_MEM, 7 dw body */
    cpm_wr(4u * w++, (1u << 15) | (1u << 19) | 0x14u | (5u << 8));
    cpm_wr(4u * w++, (1u << 29));                          /* DATA_SEL=1 (32-bit), INT_SEL=0 */
    cpm_wr(4u * w++, (uint32_t)a_fc);
    cpm_wr(4u * w++, (uint32_t)(a_fc >> 32));
    cpm_wr(4u * w++, CPIB_FENCEC_VAL);
    cpm_wr(4u * w++, 0u);
    cpm_wr(4u * w++, 0u);
    (void)cpm_rd(0x1FFCu);
    (void)vram_rd32(CP_IB_VRAM_OFF + 0x1FFCu);

    /* --- same known-good ring bring-up as V123/V139 --- */
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, me | CP_ME_HALT_BITS | R_ME_CNTL_PIPE_RESET);
    cp_delay_us(mmio, 100);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, me | CP_ME_HALT_BITS);
    cp_delay_us(mmio, 100);
    if (!cp_wr_ok(mmio, R_CP_RB_VMID, 0u, 0xFFFFFFFFu, 410)) { r->step = 410; goto done; }
    gpu_mmio_write32(mmio, R_CP_RB0_CNTL, (9u | (7u << 8)) | (1u << 31));
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR, (uint32_t)wb_mc);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR_HI, (uint32_t)(wb_mc >> 32) & 0xFFFFu);
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_LO, (uint32_t)(wb_mc + 8u));
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_HI, (uint32_t)((wb_mc + 8u) >> 32));
    if (!cp_wr_ok(mmio, R_CP_RB0_CNTL, (9u | (7u << 8)), 0x3FFFu, 411)) { r->step = 411; goto done; }
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE, (uint32_t)(ring_mc >> 8), 0xFFFFFFFFu, 412)) { r->step = 412; goto done; }
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE_HI, (uint32_t)((ring_mc >> 8) >> 32), 0xFFFFFFFFu, 413)) { r->step = 413; goto done; }
    { uint32_t db = gpu_mmio_read32(mmio, R_CP_DOORBELL_CTRL);
      if (!cp_wr_ok(mmio, R_CP_DOORBELL_CTRL, db & ~(1u << 30), 1u << 30, 414)) { r->step = 414; goto done; } }
    if (!cp_wr_ok(mmio, R_CP_MAX_CONTEXT, 7u, 0xFFFFFFFFu, 415)) { r->step = 415; goto done; }
    if (!cp_wr_ok(mmio, R_CP_DEVICE_ID, 1u, 0xFFFFFFFFu, 416)) { r->step = 416; goto done; }
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    if (!cp_wr_ok(mmio, R_CP_ME_CNTL, me & ~CP_ME_HALT_BITS, CP_ME_HALT_BITS, 417)) { r->step = 417; goto done; }
    cp_delay_us(mmio, 50);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, w);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0u);
    for (i = 0; i < 400000u; i++) {
        r->marker = cpib_rd_fence(mode, CPIB_MARKER_OFF);
        r->fenceb = cpib_rd_fence(mode, CPIB_FENCEB_OFF);
        r->fencec = cpib_rd_fence(mode, CPIB_FENCEC_OFF);
        if (r->marker == CPIB_MARKER_VAL && r->fenceb == CPIB_FENCEB_VAL && r->fencec == CPIB_FENCEC_VAL) break;
    }
    r->rptr = gpu_mmio_read32(mmio, R_CP_RB0_RPTR);
    r->wptr = gpu_mmio_read32(mmio, R_CP_RB0_WPTR);
    r->busy = gpu_mmio_read32(mmio, R_CP_BUSY_STAT);
    r->stall1 = gpu_mmio_read32(mmio, R_CP_STALLED_STAT1);
    r->grbm = gpu_mmio_read32(mmio, GC_GRBM_STATUS_OFF);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, gpu_mmio_read32(mmio, R_CP_ME_CNTL) | CP_ME_HALT_BITS);
    cp_delay_us(mmio, 100);
done:
    if (!r->step) {
        r->pass = (r->marker == CPIB_MARKER_VAL ? 1u : 0u) |
                  (r->fenceb == CPIB_FENCEB_VAL ? 2u : 0u) |
                  (r->fencec == CPIB_FENCEC_VAL ? 4u : 0u);
        if (r->pass == 7u) r->status = mode ? "CP IB + fences via GART VA PASS (VMID0)" : "CP IB + fence PASS: IB executed, WRITE_DATA and RELEASE_MEM fences landed";
        else if (r->pass & 1u) r->status = "CP IB runs; a fence did not land (see bits)";
        else r->status = mode ? "CP IB via GART VA did NOT execute" : "CP IB did NOT execute (marker missing)";
    } else r->status = "IB test aborted during ring setup";
    cpib_wr_fence(mode, CPIB_MARKER_OFF, 0);
    cpib_wr_fence(mode, CPIB_FENCEB_OFF, 0);
    cpib_wr_fence(mode, CPIB_FENCEC_OFF, 0);
unmap:
    if (mode) {
        cpib_pte_set(CPIB_VA_IB_IDX, 0, 0, 0);
        cpib_pte_set(CPIB_VA_FENCE_IDX, 0, 0, 0);
        (void)hub_invalidate(g_gart_mmio, &hub_gc);
    }
    return r->pass == 7u;
}

/* ------------------------------------------------------------------ */
/* V148: persistent GFX ring + submit/fence API (VMID0, GART VA).
     amd_gpu_ring_init(mmio)            bring the CP ring up and LEAVE IT RUNNING
     amd_gpu_submit_ib(ib_va, dwords)   queue IB (0 = fence only) + RELEASE_MEM
                                        fence; returns the fence sequence (0 = error)
     amd_gpu_fence_wait(seq, loops)     poll the fence page; 1 = reached, 0 = timeout
   The ring is circular (1024 dwords); packets may straddle the wrap.      */
#define RG_DW             1024u
#define RG_IB_IDX         0x108u      /* GART page: IB slots (VRAM)        */
#define RG_FENCE_IDX      0x109u      /* GART page: fence + markers (sysmem) */
#define RG_SLOT_BYTES     0x100u
#define RG_SLOTS          16u
#define RG_MARKER_OFF     0x100u      /* markers: fence page + 0x100 + 4*k  */
static uint64_t rg_mmio, rg_va_ib, rg_va_fence;
static volatile uint32_t *rg_fence;   /* kernel-RAM fence page (virt==phys) */
static uint32_t rg_w, rg_seq, rg_up, rg_dead, rg_wraps, rg_timeouts, rg_full;
static uint32_t rg_submits, rg_step, rg_pass, rg_a_ok, rg_b_ok, rg_last_fence;
static const char *rg_status = "persistent ring not run";
/* failure snapshot (V149): taken at the moment a fence wait times out */
static uint32_t rgd[10], rgd_valid;
static uint32_t rg_fence_read(uint32_t off);
static void rg_capture(void)
{
    rgd[0] = gpu_mmio_read32(rg_mmio, R_CP_RB0_RPTR);
    rgd[1] = gpu_mmio_read32(rg_mmio, R_CP_RB0_WPTR);
    rgd[2] = gpu_mmio_read32(rg_mmio, R_CP_BUSY_STAT);
    rgd[3] = gpu_mmio_read32(rg_mmio, R_CP_STALLED_STAT1);
    rgd[4] = gpu_mmio_read32(rg_mmio, GC_GRBM_STATUS_OFF);
    rgd[5] = rg_fence_read(RG_MARKER_OFF);
    rgd[6] = gpu_mmio_read32(rg_mmio, R_CP_PFP_HDR);
    rgd[7] = gpu_mmio_read32(rg_mmio, R_CP_ME_HDR);
    rgd[8] = gpu_mmio_read32(rg_mmio, R_CP_ME_CNTL);
    rgd[9] = vram_rd32(CP_WB_VRAM_OFF);        /* rptr write-back copy */
    rgd_valid = 1;
}

static void rg_put(uint32_t v)
{
    cpm_wr((rg_w & (RG_DW - 1u)) * 4u, v);
    rg_w++;
    if ((rg_w & (RG_DW - 1u)) == 0u) rg_wraps++;
}
static uint32_t rg_fence_read(uint32_t off)
{
    volatile uint32_t *p = &rg_fence[off >> 2];
    cache_flush_line(p);
    return *p;
}

int amd_gpu_fence_wait(uint32_t seq, uint32_t loops);
int amd_gpu_ring_init(uint64_t mmio)
{
    uint64_t ring_mc, wb_mc;
    uint32_t i, me;
    rg_up = 0; rg_dead = 0; rg_w = 0; rg_seq = 0; rg_wraps = 0; rg_step = 0;
    if (!gpu_mmio_can_access(mmio) || !g_cpu_vram) { rg_step = 1; return 0; }
    if (!rg_fence) rg_fence = sys_alloc_page_aligned(4096u);
    if (!rg_fence) { rg_step = 2; return 0; }
    for (i = 0; i < 1024u; i++) rg_fence[i] = 0;
    rg_mmio = mmio;
    rg_va_ib    = amd_native_gart_va() + (uint64_t)RG_IB_IDX * 0x1000ULL;
    rg_va_fence = amd_native_gart_va() + (uint64_t)RG_FENCE_IDX * 0x1000ULL;
    cpib_pte_set(RG_IB_IDX, amd_native_cp_vm_mc_addr(CP_IB_VRAM_OFF, 0x1000u), 0, 1);
    cpib_pte_set(RG_FENCE_IDX, (uint64_t)(unsigned long long)rg_fence, 1, 1);
    if (!hub_invalidate(g_gart_mmio, &hub_gc)) { rg_step = 3; return 0; }

    ring_mc = amd_native_vram_mc_addr(CP_RING_VRAM_OFF, CP_RING_BYTES);
    wb_mc   = amd_native_vram_mc_addr(CP_WB_VRAM_OFF, 0x1000u);
    if (!ring_mc || !wb_mc) { rg_step = 4; return 0; }
    for (i = 0; i < RG_DW; i++) vram_wr32(CP_RING_VRAM_OFF + i * 4u, 0xFFFF1000u);
    for (i = 0; i < 0x1000u / 4u; i++) vram_wr32(CP_IB_VRAM_OFF + i * 4u, 0xFFFF1000u);
    (void)cpm_rd(0x1FFCu);

    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, me | CP_ME_HALT_BITS | R_ME_CNTL_PIPE_RESET);
    cp_delay_us(mmio, 100);
    gpu_mmio_write32(mmio, R_CP_ME_CNTL, me | CP_ME_HALT_BITS);
    cp_delay_us(mmio, 100);
    if (!cp_wr_ok(mmio, R_CP_RB_VMID, 0u, 0xFFFFFFFFu, 510)) { rg_step = 510; return 0; }
    gpu_mmio_write32(mmio, R_CP_RB0_CNTL, (9u | (7u << 8)) | (1u << 31));
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR, (uint32_t)wb_mc);
    gpu_mmio_write32(mmio, R_CP_RB0_RPTR_ADDR_HI, (uint32_t)(wb_mc >> 32) & 0xFFFFu);
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_LO, (uint32_t)(wb_mc + 8u));
    gpu_mmio_write32(mmio, R_CP_WPTR_POLL_HI, (uint32_t)((wb_mc + 8u) >> 32));
    if (!cp_wr_ok(mmio, R_CP_RB0_CNTL, (9u | (7u << 8)), 0x3FFFu, 511)) { rg_step = 511; return 0; }
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE, (uint32_t)(ring_mc >> 8), 0xFFFFFFFFu, 512)) { rg_step = 512; return 0; }
    if (!cp_wr_ok(mmio, R_CP_RB0_BASE_HI, (uint32_t)((ring_mc >> 8) >> 32), 0xFFFFFFFFu, 513)) { rg_step = 513; return 0; }
    { uint32_t db = gpu_mmio_read32(mmio, R_CP_DOORBELL_CTRL);
      if (!cp_wr_ok(mmio, R_CP_DOORBELL_CTRL, db & ~(1u << 30), 1u << 30, 514)) { rg_step = 514; return 0; } }
    if (!cp_wr_ok(mmio, R_CP_MAX_CONTEXT, 7u, 0xFFFFFFFFu, 515)) { rg_step = 515; return 0; }
    if (!cp_wr_ok(mmio, R_CP_DEVICE_ID, 1u, 0xFFFFFFFFu, 516)) { rg_step = 516; return 0; }
    /* V149: prime the ring with one fence packet BEFORE un-halting, exactly
       the order every earlier passing test used (V148 wrote the first packet
       after un-halt and the fence never landed). */
    rg_up = 1;
    {
        uint64_t fa = rg_va_fence;
        rg_seq = 1;
        rg_put(0xC0064900u); rg_put((1u << 15) | (1u << 19) | 0x14u | (5u << 8)); rg_put(1u << 29);
        rg_put((uint32_t)fa); rg_put((uint32_t)(fa >> 32)); rg_put(rg_seq); rg_put(0u); rg_put(0u);
        (void)cpm_rd(0x1FFCu);
    }
    me = gpu_mmio_read32(mmio, R_CP_ME_CNTL);
    if (!cp_wr_ok(mmio, R_CP_ME_CNTL, me & ~CP_ME_HALT_BITS, CP_ME_HALT_BITS, 517)) { rg_step = 517; rg_up = 0; return 0; }
    cp_delay_us(mmio, 50);
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR, rg_w & (RG_DW - 1u));
    gpu_mmio_write32(mmio, R_CP_RB0_WPTR_HI, 0u);
    if (!amd_gpu_fence_wait(1u, 2000000u)) { rg_step = 520; rg_up = 0; return 0; }   /* priming fence */
    return 1;
}

int amd_gpu_fence_wait(uint32_t seq, uint32_t loops)
{
    uint32_t i;
    if (!rg_up || rg_dead) return 0;
    for (i = 0; i < loops; i++) {
        uint32_t v = rg_fence_read(0);
        rg_last_fence = v;
        if ((int32_t)(v - seq) >= 0) return 1;
    }
    rg_timeouts++;
    rg_capture();
    rg_dead = 1;          /* a lost fence means the CP is wedged: stop feeding it */
    return 0;
}

uint32_t amd_gpu_submit_ib(uint64_t ib_va, uint32_t ib_dw)
{
    uint32_t need = (ib_dw ? 4u : 0u) + 8u, spin, seq;
    uint64_t fa = rg_va_fence;
    if (!rg_up || rg_dead) return 0;
    for (spin = 0; ; spin++) {            /* wait for ring space */
        uint32_t rptr = gpu_mmio_read32(rg_mmio, R_CP_RB0_RPTR) & (RG_DW - 1u);
        uint32_t used = (rg_w - rptr) & (RG_DW - 1u);
        if (RG_DW - 1u - used >= need) break;
        if (spin > 2000000u) { rg_full++; rg_dead = 1; return 0; }
    }
    seq = ++rg_seq;
    if (ib_dw) {
        rg_put(0xC0023F00u);
        rg_put((uint32_t)ib_va & 0xFFFFFFFCu);
        rg_put((uint32_t)(ib_va >> 32));
        rg_put(ib_dw);                                   /* VMID0 */
    }
    rg_put(0xC0064900u);                                 /* RELEASE_MEM */
    rg_put((1u << 15) | (1u << 19) | 0x14u | (5u << 8));
    rg_put(1u << 29);
    rg_put((uint32_t)fa);
    rg_put((uint32_t)(fa >> 32));
    rg_put(seq);
    rg_put(0u);
    rg_put(0u);
    __asm__ volatile("mfence" ::: "memory");             /* the ring words sit in write-combining buffers: drain them, then wait for the posted writes */
    (void)cpm_rd(0x1FFCu);
    /* V150: write LO *then HI*, as Linux gfx_v9_0_ring_set_wptr_gfx does and
       as every passing test did.  The V149 output showed the register still
       at the previous value (wptr 8, rptr 8) after a LO-only write, i.e. the
       new write pointer was never latched. */
    gpu_mmio_write32(rg_mmio, R_CP_RB0_WPTR, rg_w & (RG_DW - 1u));
    gpu_mmio_write32(rg_mmio, R_CP_RB0_WPTR_HI, 0u);
    rg_submits++;
    return seq;
}

/* Packets written straight into the ring (not through an IB), then the same RELEASE_MEM fence. Linux gfx_v9_0_cp_gfx_start() writes
   its start-up packets (PREAMBLE_CNTL, the clear state, CLEAR_STATE, SET_BASE) this way, in the kernel ring. n + 8 must fit in the
   ring (RG_DW - 1); the call waits for that much free space first. */
uint32_t amd_gpu_submit_raw(const uint32_t *dw, uint32_t n)
{
    uint32_t need = n + 8u, spin, seq, i;
    uint64_t fa = rg_va_fence;
    if (!rg_up || rg_dead || need > RG_DW - 1u) return 0;
    for (spin = 0; ; spin++) {
        uint32_t rptr = gpu_mmio_read32(rg_mmio, R_CP_RB0_RPTR) & (RG_DW - 1u);
        uint32_t used = (rg_w - rptr) & (RG_DW - 1u);
        if (RG_DW - 1u - used >= need) break;
        if (spin > 2000000u) { rg_full++; rg_dead = 1; return 0; }
    }
    seq = ++rg_seq;
    for (i = 0; i < n; i++) rg_put(dw[i]);
    rg_put(0xC0064900u);                                 /* RELEASE_MEM, as in amd_gpu_submit_ib() */
    rg_put((1u << 15) | (1u << 19) | 0x14u | (5u << 8));
    rg_put(1u << 29);
    rg_put((uint32_t)fa);
    rg_put((uint32_t)(fa >> 32));
    rg_put(seq);
    rg_put(0u);
    rg_put(0u);
    __asm__ volatile("mfence" ::: "memory");
    (void)cpm_rd(0x1FFCu);
    gpu_mmio_write32(rg_mmio, R_CP_RB0_WPTR, rg_w & (RG_DW - 1u));
    gpu_mmio_write32(rg_mmio, R_CP_RB0_WPTR_HI, 0u);
    rg_submits++;
    return seq;
}

/* The frame IB (M4 compositor): 128 KiB of VRAM at CP_FRAME_IB_VRAM_OFF, mapped through 32 GART pages from RG_FRAME_IDX on, the same
   way as the IB page (cpib_pte_set + hub_invalidate), so the CP fetches it through VMID0 as it fetches every other IB. */
#define CP_FRAME_IB_VRAM_OFF 0x0A100000u
#define RG_FRAME_IDX         0x120u
#define RG_FRAME_PAGES       32u
static uint64_t rg_va_frame;

uint64_t amd_gpu_frame_ib_va(void)
{
    uint32_t i;
    if (rg_va_frame) return rg_va_frame;
    if (!rg_up || rg_dead || !amd_native_gart_va()) return 0;
    for (i = 0; i < RG_FRAME_PAGES; i++) {
        uint64_t pa = amd_native_cp_vm_mc_addr(CP_FRAME_IB_VRAM_OFF + i * 0x1000u, 0x1000u);
        if (!pa) return 0;
        cpib_pte_set(RG_FRAME_IDX + i, pa, 0, 1);
    }
    if (!hub_invalidate(g_gart_mmio, &hub_gc)) return 0;
    rg_va_frame = amd_native_gart_va() + (uint64_t)RG_FRAME_IDX * 0x1000ULL;
    return rg_va_frame;
}
uint32_t amd_gpu_frame_ib_max_dwords(void) { return RG_FRAME_PAGES * 1024u; }
void amd_gpu_frame_ib_write32(uint32_t dw, uint32_t v)
{
    if (dw < RG_FRAME_PAGES * 1024u) vram_wr32(CP_FRAME_IB_VRAM_OFF + 4u * dw, v);
}

/* IB slot k: WRITE_DATA(marker k) + NOP padding. */
static void rg_build_slot(uint32_t k, uint32_t value)
{
    uint32_t off = CP_IB_VRAM_OFF + k * RG_SLOT_BYTES, n = 0;
    uint64_t m = rg_va_fence + RG_MARKER_OFF + 4u * k;
    vram_wr32(off + 4u * n++, 0xC0033700u);
    vram_wr32(off + 4u * n++, (5u << 8) | (1u << 20));
    vram_wr32(off + 4u * n++, (uint32_t)m);
    vram_wr32(off + 4u * n++, (uint32_t)(m >> 32));
    vram_wr32(off + 4u * n++, value);
    while (n < 8u) vram_wr32(off + 4u * n++, 0xFFFF1000u);
}

int amd_gpu_ring_selftest(uint64_t mmio)
{
    uint32_t i, k, seq = 0, ok_a = 0, ok_b = 0;
    rg_pass = 0; rg_a_ok = rg_b_ok = 0; rg_submits = 0; rg_timeouts = 0; rg_full = 0;
    if (!amd_gpu_ring_init(mmio)) { rg_status = "persistent ring init FAILED"; return 0; }

    /* Phase A: 300 sequential IB submits (12 dw each = 3600 dw: wraps the
       1024-dw ring 3+ times); every fence and every IB marker is verified. */
    for (i = 0; i < 300u; i++) {
        rg_fence[(RG_MARKER_OFF >> 2)] = 0; cache_flush_line(&rg_fence[RG_MARKER_OFF >> 2]);
        rg_build_slot(0, 0xA0000000u + i);
        (void)vram_rd32(CP_IB_VRAM_OFF);
        seq = amd_gpu_submit_ib(rg_va_ib, 8u);
        if (!seq || !amd_gpu_fence_wait(seq, 2000000u)) break;
        if (rg_fence_read(RG_MARKER_OFF) != 0xA0000000u + i) break;
        ok_a++;
    }
    rg_a_ok = ok_a;

    /* Phase B: 16 submits queued back to back (distinct IB slots, distinct
       markers), then ONE wait on the last fence. */
    if (ok_a == 300u) {
        for (k = 0; k < RG_SLOTS; k++) {
            rg_fence[(RG_MARKER_OFF >> 2) + k] = 0;
            cache_flush_line(&rg_fence[(RG_MARKER_OFF >> 2) + k]);
            rg_build_slot(k, 0xB0000000u + k);
        }
        (void)vram_rd32(CP_IB_VRAM_OFF);
        for (k = 0; k < RG_SLOTS; k++) seq = amd_gpu_submit_ib(rg_va_ib + (uint64_t)k * RG_SLOT_BYTES, 8u);
        if (seq && amd_gpu_fence_wait(seq, 4000000u))
            for (k = 0; k < RG_SLOTS; k++)
                if (rg_fence_read(RG_MARKER_OFF + 4u * k) == 0xB0000000u + k) ok_b++;
    }
    rg_b_ok = ok_b;
    rg_pass = (ok_a == 300u ? 1u : 0u) | (ok_b == RG_SLOTS ? 2u : 0u) |
              (rg_wraps >= 3u ? 4u : 0u) | (rg_timeouts == 0u ? 8u : 0u);
    rg_status = rg_pass == 15u ? "Persistent ring PASS: 316 IB submits, wraps, fences ordered"
                               : "Persistent ring FAILED (see bits: 1=A 2=B 4=wrap 8=no-timeout)";
    if (rg_pass != 15u) {              /* leave a wedged CP halted */
        gpu_mmio_write32(mmio, R_CP_ME_CNTL, gpu_mmio_read32(mmio, R_CP_ME_CNTL) | CP_ME_HALT_BITS);
        rg_up = 0;
    }
    return rg_pass == 15u;
}
const char *amd_native_ring_status(void) { return rg_status; }
uint32_t amd_native_ring_value(int what)
{
    switch (what) {
        case 0: return rg_pass;      case 1: return rg_submits;  case 2: return rg_wraps;
        case 3: return rg_timeouts;  case 4: return rg_seq;      case 5: return rg_last_fence;
        case 6: return rg_up ? (gpu_mmio_read32(rg_mmio, R_CP_RB0_RPTR) & (RG_DW - 1u)) : 0u;
        case 7: return rg_w & (RG_DW - 1u);
        case 8: return rg_a_ok;      case 9: return rg_b_ok;     case 10: return rg_step;
        case 11: return rg_up;       case 12: return rg_full;
        case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
            return rgd_valid ? rgd[what - 20] : 0xFFFFFFFFu;
        default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* V151: GPU draw test.
   The CP executes an IB of PM4 WRITE_DATA packets that paint a 32x24 pixel
   gradient tile into a system-RAM buffer through the GART (VMID0).  The CPU
   then verifies every pixel (so a pass means the GPU really wrote them) and
   the overlay blits the tile on screen.  Same proven packet/fence path as the
   ring self-test; the on-screen scanout is NOT written by the GPU here.     */
#define GD_W        32u
#define GD_H        24u
#define GD_IDX      0x10Au                       /* GART page: pixel buffer */
static volatile uint32_t *gd_buf;
static uint32_t gd_state, gd_good, gd_seq, gd_ibdw;
static const char *gd_status = "GPU draw test not run";

static uint32_t gd_expect(uint32_t x, uint32_t y)
{
    if (x == 0 || y == 0 || x == GD_W - 1u || y == GD_H - 1u) return 0x00FFFFFFu;
    return ((x * 8u) << 16) | ((y * 10u) << 8) | (255u - x * 8u);
}

int amd_gpu_draw_test(void)
{
    uint32_t x, y, n = 0, i;
    uint64_t va;
    gd_state = 0; gd_good = 0; gd_seq = 0; gd_ibdw = 0;
    if (!rg_up || rg_dead) { gd_status = "GPU draw skipped: ring not running"; return 0; }
    if (!gd_buf) gd_buf = sys_alloc_page_aligned(4096u);
    if (!gd_buf) { gd_status = "GPU draw FAILED: no buffer"; gd_state = 1; return 0; }
    for (i = 0; i < 1024u; i++) gd_buf[i] = 0xDEADBEEFu;
    for (i = 0; i < 1024u; i += 16u) cache_flush_line(&gd_buf[i]);

    cpib_pte_set(GD_IDX, (uint64_t)(unsigned long long)gd_buf, 1, 1);
    if (!hub_invalidate(g_gart_mmio, &hub_gc)) { gd_status = "GPU draw FAILED: GART invalidate"; gd_state = 2; return 0; }
    va = amd_native_gart_va() + (uint64_t)GD_IDX * 0x1000ULL;

    /* One WRITE_DATA per row: header, ctl, addr lo/hi, GD_W pixel dwords. */
    for (y = 0; y < GD_H; y++) {
        uint64_t a = va + (uint64_t)y * GD_W * 4u;
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, 0xC0003700u | ((2u + GD_W) << 16));
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (5u << 8) | (1u << 20));
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (uint32_t)a);
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (uint32_t)(a >> 32));
        for (x = 0; x < GD_W; x++) vram_wr32(CP_IB_VRAM_OFF + 4u * n++, gd_expect(x, y));
    }
    gd_ibdw = n;                                  /* 24 * 36 = 864 dwords */
    (void)vram_rd32(CP_IB_VRAM_OFF + 4u * (n - 1u));   /* flush posted writes */

    gd_seq = amd_gpu_submit_ib(rg_va_ib, n);
    if (!gd_seq) { gd_status = "GPU draw FAILED: submit"; gd_state = 3; return 0; }
    if (!amd_gpu_fence_wait(gd_seq, 4000000u)) { gd_status = "GPU draw FAILED: fence timeout"; gd_state = 4; return 0; }

    for (y = 0; y < GD_H; y++)
        for (x = 0; x < GD_W; x++) {
            volatile uint32_t *p = &gd_buf[y * GD_W + x];
            cache_flush_line(p);
            if (*p == gd_expect(x, y)) gd_good++;
        }
    if (gd_good == GD_W * GD_H) { gd_status = "GPU draw PASS: tile painted by CP"; gd_state = 0x100; return 1; }
    gd_status = "GPU draw FAILED: pixel mismatch"; gd_state = 5;
    return 0;
}
const char *amd_native_draw_status(void) { return gd_status; }
const uint32_t *amd_native_draw_buf(void) { return (gd_state == 0x100) ? (const uint32_t *)gd_buf : 0; }
uint32_t amd_native_draw_value(int what)
{
    switch (what) {
        case 0: return gd_state;  case 1: return gd_good;  case 2: return GD_W * GD_H;
        case 3: return gd_seq;    case 4: return gd_ibdw;  case 5: return GD_W;
        case 6: return GD_H;
        default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* V152: GPU fill via CP DMA (PM4 DMA_DATA, src_sel=DATA).
   Four DMA_DATA packets (7 dwords each, 28 dword IB) fill a 128x128 XRGB
   surface in system RAM (16 GART pages, VMID0) with four solid bands.  The
   GPU generates all 16384 pixels itself; the CPU only verifies.             */
#define GF_W        128u
#define GF_H        128u
#define GF_BANDS    4u
#define GF_PAGES    16u                          /* 128*128*4 = 64 KiB */
#define GF_IDX      0x10Bu                       /* GART pages 0x10B..0x11A */
static volatile uint32_t *gf_buf;
static uint32_t gf_state, gf_good, gf_seq, gf_ibdw;
static const char *gf_status = "GPU fill test not run";
static const uint32_t gf_color[GF_BANDS] = { 0x00FF3030u, 0x0030FF30u, 0x003060FFu, 0x00FFD030u };

int amd_gpu_fill_test(void)
{
    uint32_t i, b, n = 0, band_bytes = GF_W * (GF_H / GF_BANDS) * 4u;
    uint64_t va;
    gf_state = 0; gf_good = 0; gf_seq = 0; gf_ibdw = 0;
    if (!rg_up || rg_dead) { gf_status = "GPU fill skipped: ring not running"; return 0; }
    if (!gf_buf) gf_buf = sys_alloc_page_aligned(GF_PAGES * 4096u);
    if (!gf_buf) { gf_status = "GPU fill FAILED: no buffer"; gf_state = 1; return 0; }
    for (i = 0; i < GF_W * GF_H; i++) gf_buf[i] = 0xDEADBEEFu;
    for (i = 0; i < GF_W * GF_H; i += 16u) cache_flush_line(&gf_buf[i]);

    for (i = 0; i < GF_PAGES; i++)
        cpib_pte_set(GF_IDX + i, (uint64_t)(unsigned long long)gf_buf + (uint64_t)i * 4096ULL, 1, 1);
    if (!hub_invalidate(g_gart_mmio, &hub_gc)) { gf_status = "GPU fill FAILED: GART invalidate"; gf_state = 2; return 0; }
    va = amd_native_gart_va() + (uint64_t)GF_IDX * 0x1000ULL;

    for (b = 0; b < GF_BANDS; b++) {
        uint64_t dst = va + (uint64_t)b * band_bytes;
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, 0xC0055000u);            /* PACKET3(DMA_DATA, 5) */
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (2u << 29) | (1u << 31)); /* SRC_SEL=DATA, CP_SYNC */
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, gf_color[b]);            /* src: immediate fill value */
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, 0u);
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (uint32_t)dst);
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (uint32_t)(dst >> 32));
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, band_bytes);             /* byte count, memory->memory */
    }
    gf_ibdw = n;
    (void)vram_rd32(CP_IB_VRAM_OFF + 4u * (n - 1u));

    gf_seq = amd_gpu_submit_ib(rg_va_ib, n);
    if (!gf_seq) { gf_status = "GPU fill FAILED: submit"; gf_state = 3; return 0; }
    if (!amd_gpu_fence_wait(gf_seq, 8000000u)) { gf_status = "GPU fill FAILED: fence timeout"; gf_state = 4; return 0; }

    for (i = 0; i < GF_W * GF_H; i++) {
        volatile uint32_t *p = &gf_buf[i];
        if ((i & 15u) == 0) cache_flush_line(p);
        if (*p == gf_color[i / (GF_W * (GF_H / GF_BANDS))]) gf_good++;
    }
    if (gf_good == GF_W * GF_H) { gf_status = "GPU fill PASS: 128x128 filled by CP DMA"; gf_state = 0x100; return 1; }
    gf_status = "GPU fill FAILED: pixel mismatch"; gf_state = 5;
    return 0;
}
const char *amd_native_fill_status(void) { return gf_status; }
const uint32_t *amd_native_fill_buf(void) { return (gf_state == 0x100) ? (const uint32_t *)gf_buf : 0; }
uint32_t amd_native_fill_value(int what)
{
    switch (what) {
        case 0: return gf_state;  case 1: return gf_good;  case 2: return GF_W * GF_H;
        case 3: return gf_seq;    case 4: return gf_ibdw;  case 5: return GF_W;
        case 6: return GF_H;      case 7: return GF_BANDS;
        default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* V153: GPU blit via CP DMA (PM4 DMA_DATA, src_sel=SRC_ADDR, memory->memory).
   Copies the four bands of the V152 surface into a second 128x128 surface in
   REVERSED order (band b -> band 3-b), so a pass proves the GPU really moved
   the data (a stale/unchanged destination cannot match).                    */
#define GB_IDX      0x11Bu                       /* GART pages 0x11B..0x12A */
static volatile uint32_t *gb_buf;
static uint32_t gb_state, gb_good, gb_seq, gb_ibdw;
static const char *gb_status = "GPU blit test not run";

int amd_gpu_blit_test(void)
{
    uint32_t i, b, n = 0, band_bytes = GF_W * (GF_H / GF_BANDS) * 4u;
    uint64_t src_va, dst_va;
    gb_state = 0; gb_good = 0; gb_seq = 0; gb_ibdw = 0;
    if (!rg_up || rg_dead) { gb_status = "GPU blit skipped: ring not running"; return 0; }
    if (gf_state != 0x100 || !gf_buf) { gb_status = "GPU blit skipped: fill source not valid"; return 0; }
    if (!gb_buf) gb_buf = sys_alloc_page_aligned(GF_PAGES * 4096u);
    if (!gb_buf) { gb_status = "GPU blit FAILED: no buffer"; gb_state = 1; return 0; }
    for (i = 0; i < GF_W * GF_H; i++) gb_buf[i] = 0xDEADBEEFu;
    for (i = 0; i < GF_W * GF_H; i += 16u) cache_flush_line(&gb_buf[i]);

    for (i = 0; i < GF_PAGES; i++)
        cpib_pte_set(GB_IDX + i, (uint64_t)(unsigned long long)gb_buf + (uint64_t)i * 4096ULL, 1, 1);
    if (!hub_invalidate(g_gart_mmio, &hub_gc)) { gb_status = "GPU blit FAILED: GART invalidate"; gb_state = 2; return 0; }
    src_va = amd_native_gart_va() + (uint64_t)GF_IDX * 0x1000ULL;
    dst_va = amd_native_gart_va() + (uint64_t)GB_IDX * 0x1000ULL;

    for (b = 0; b < GF_BANDS; b++) {
        uint64_t s_a = src_va + (uint64_t)b * band_bytes;
        uint64_t d_a = dst_va + (uint64_t)(GF_BANDS - 1u - b) * band_bytes;
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, 0xC0055000u);            /* PACKET3(DMA_DATA, 5) */
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (1u << 31));             /* SRC_SEL=SRC_ADDR, CP_SYNC */
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (uint32_t)s_a);
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (uint32_t)(s_a >> 32));
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (uint32_t)d_a);
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (uint32_t)(d_a >> 32));
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, band_bytes);
    }
    gb_ibdw = n;
    (void)vram_rd32(CP_IB_VRAM_OFF + 4u * (n - 1u));

    gb_seq = amd_gpu_submit_ib(rg_va_ib, n);
    if (!gb_seq) { gb_status = "GPU blit FAILED: submit"; gb_state = 3; return 0; }
    if (!amd_gpu_fence_wait(gb_seq, 8000000u)) { gb_status = "GPU blit FAILED: fence timeout"; gb_state = 4; return 0; }

    for (i = 0; i < GF_W * GF_H; i++) {
        volatile uint32_t *p = &gb_buf[i];
        if ((i & 15u) == 0) cache_flush_line(p);
        if (*p == gf_color[GF_BANDS - 1u - i / (GF_W * (GF_H / GF_BANDS))]) gb_good++;
    }
    if (gb_good == GF_W * GF_H) { gb_status = "GPU blit PASS: bands copied reversed by CP DMA"; gb_state = 0x100; return 1; }
    gb_status = "GPU blit FAILED: pixel mismatch"; gb_state = 5;
    return 0;
}
const char *amd_native_blit_status(void) { return gb_status; }
const uint32_t *amd_native_blit_buf(void) { return (gb_state == 0x100) ? (const uint32_t *)gb_buf : 0; }
uint32_t amd_native_blit_value(int what)
{
    switch (what) {
        case 0: return gb_state;  case 1: return gb_good;  case 2: return GF_W * GF_H;
        case 3: return gb_seq;    case 4: return gb_ibdw;  case 5: return GF_W;
        case 6: return GF_H;      case 7: return GF_BANDS;
        default: return 0;
    }
}


/* V155 2D engine state (defined with the V155 block below) */
static uint64_t g2_mc;
static uint32_t g2_pitch, g2_w, g2_h, g2_active;
/* ------------------------------------------------------------------ */
/* V154: GPU writes straight into the scanout framebuffer.
   amd_gpu_fb_strip() is called after every gfx_present().  The first call
   runs the safety checks + a verified test; later calls repaint a 256x16
   strip at the bottom of the screen with CP DMA_DATA fills (one packet per
   row, pitch-aware).  Safety: the framebuffer must sit inside the VRAM
   aperture AND must not overlap the driver's own VRAM regions
   (GART table / ring / IB / PSP TMR, 128..208 MiB).  Any failure latches the
   feature off so it can never stall the desktop twice.                      */
#define FB_STRIP_W      256u
#define FB_STRIP_H      16u
#define FB_STRIP_BANDS  4u
#ifndef FB_STRIP_REPAINT
#define FB_STRIP_REPAINT 0                       /* 1 = repaint the strip after every present */
#endif
#define FB_RES_LO       0x08000000u              /* GART table start */
#define FB_RES_HI       0x0D000000u              /* end of TMR region */
static uint32_t fs_state, fs_good, fs_seq, fs_ibdw, fs_off, fs_mc_lo, fs_fbp_lo, fs_fbp_hi, fs_calls;
static const char *fs_status = "GPU fb strip not run";
static const uint32_t fs_color[FB_STRIP_BANDS] = { 0x00FF3030u, 0x0030FF30u, 0x003060FFu, 0x00FFD030u };

static int fs_fire(uint64_t mc_base, uint32_t pitch, uint32_t x0, uint32_t y0, uint32_t *seq_out)
{
    uint32_t r, n = 0;
    for (r = 0; r < FB_STRIP_H; r++) {
        uint64_t d = mc_base + (uint64_t)(y0 + r) * pitch + (uint64_t)x0 * 4u;
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, 0xC0055000u);
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (2u << 29) | (1u << 31));
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, fs_color[r / (FB_STRIP_H / FB_STRIP_BANDS)]);
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, 0u);
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (uint32_t)d);
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, (uint32_t)(d >> 32));
        vram_wr32(CP_IB_VRAM_OFF + 4u * n++, FB_STRIP_W * 4u);
    }
    fs_ibdw = n;
    (void)vram_rd32(CP_IB_VRAM_OFF + 4u * (n - 1u));
    *seq_out = amd_gpu_submit_ib(rg_va_ib, n);
    if (!*seq_out) return 3;
    if (!amd_gpu_fence_wait(*seq_out, 8000000u)) return 4;
    return 0;
}

int amd_gpu_fb_strip(uint64_t fb_phys, uint32_t pitch, uint32_t width, uint32_t height,
                     volatile uint8_t *fb_cpu)
{
    uint64_t vram_bytes, span, mc;
    uint32_t off, y0, x0, r, c, rc;
    if (fs_state == 0x100) {                       /* proven: just repaint */
#if !FB_STRIP_REPAINT
        return 1;                                   /* V158: proven once, no per-frame GPU round trip */
#endif
        uint32_t seq;
        x0 = (width > FB_STRIP_W) ? (width - FB_STRIP_W) / 2u : 0u;
        y0 = height - FB_STRIP_H;
        mc = amd_native_vram_mc_addr(fs_off, pitch * height);
        if (!mc) return 0;
        fs_calls++;
        rc = fs_fire(mc, pitch, x0, y0, &seq);
        if (rc) { fs_state = rc; fs_status = "GPU fb strip FAILED later: disabled"; return 0; }
        fs_seq = seq;
        return 1;
    }
    if (fs_state != 0) return 0;                   /* latched off */

    fs_state = 1;                                  /* assume failure until proven */
    fs_fbp_lo = (uint32_t)fb_phys; fs_fbp_hi = (uint32_t)(fb_phys >> 32);
    if (!rg_up || rg_dead) { fs_status = "GPU fb skipped: ring not running"; return 0; }
    if (!fb_cpu || !pitch || !width || height < FB_STRIP_H || (pitch & 3u)) { fs_status = "GPU fb skipped: bad fb info"; return 0; }
    vram_bytes = (uint64_t)vram_mib << 20;
    span = (uint64_t)pitch * height;
    if (!vram_bytes || fb_phys < g_cpu_vram || fb_phys + span > g_cpu_vram + vram_bytes) {
        fs_status = "GPU fb skipped: fb not inside VRAM aperture"; return 0; }
    off = (uint32_t)(fb_phys - g_cpu_vram);
    fs_off = off;
    if ((uint64_t)off < FB_RES_HI && (uint64_t)off + span > FB_RES_LO) {
        fs_status = "GPU fb skipped: fb overlaps driver VRAM"; return 0; }
    mc = amd_native_vram_mc_addr(off, (uint32_t)span);
    if (!mc) { fs_status = "GPU fb skipped: no MC address"; return 0; }
    fs_mc_lo = (uint32_t)mc;

    x0 = (width > FB_STRIP_W) ? (width - FB_STRIP_W) / 2u : 0u;
    y0 = height - FB_STRIP_H;
    /* Poison the strip from the CPU so only the GPU can make it match. */
    for (r = 0; r < FB_STRIP_H; r++)
        for (c = 0; c < FB_STRIP_W; c++)
            *(volatile uint32_t *)(fb_cpu + (uint32_t)(y0 + r) * pitch + (x0 + c) * 4u) = 0xDEADBEEFu;
    (void)*(volatile uint32_t *)fb_cpu;            /* drain write-combining buffers */

    rc = fs_fire(mc, pitch, x0, y0, &fs_seq);
    if (rc) { fs_state = rc; fs_status = (rc == 3) ? "GPU fb FAILED: submit" : "GPU fb FAILED: fence timeout"; return 0; }

    fs_good = 0;
    for (r = 0; r < FB_STRIP_H; r++)
        for (c = 0; c < FB_STRIP_W; c++)
            if (*(volatile uint32_t *)(fb_cpu + (uint32_t)(y0 + r) * pitch + (x0 + c) * 4u)
                == fs_color[r / (FB_STRIP_H / FB_STRIP_BANDS)]) fs_good++;
    if (fs_good == FB_STRIP_W * FB_STRIP_H) {
        fs_state = 0x100; fs_status = "GPU fb PASS: CP painted the scanout buffer";
        g2_mc = mc; g2_pitch = pitch; g2_w = width; g2_h = height; g2_active = 1;   /* V155: 2D engine unlocked */
        return 1;
    }
    fs_state = 5; fs_status = "GPU fb FAILED: pixel mismatch (strip left as is)";
    return 0;
}
const char *amd_native_fb_status(void) { return fs_status; }
uint32_t amd_native_fb_value(int what)
{
    switch (what) {
        case 0: return fs_state;  case 1: return fs_good;  case 2: return FB_STRIP_W * FB_STRIP_H;
        case 3: return fs_seq;    case 4: return fs_ibdw;  case 5: return fs_off;
        case 6: return fs_mc_lo;  case 7: return fs_fbp_lo; case 8: return fs_fbp_hi;
        case 9: return (uint32_t)g_cpu_vram; case 10: return vram_mib; case 11: return fs_calls;
        default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* V155: batched GPU 2D engine on the scanout framebuffer.
   amd_gpu2d_fill()/amd_gpu2d_copy() append CP DMA_DATA packets to one IB
   (clipped; full-width rects collapse to a single packet; overlapping
   copies pick a safe row order).  amd_gpu2d_flush() submits the batch and
   waits for the fence once.  Unlocked only after the V154 test passed; any
   submit/fence error latches the engine off (callers fall back to the CPU).
   Single-threaded: always flush before anything else uses the IB page.     */
#define G2_MAX_DW     1000u                      /* of the 1024-dword IB page */
#define G2_MAX_BYTES  0x001FFFFCu                /* DMA_DATA byte-count limit, dword aligned */
static uint32_t g2_n, g2_pkts, g2_ops, g2_flushes, g2_fail;
static uint32_t g2_t_fill, g2_t_copy, g2_t_cpu, g2_t_cpucopy, g2_t_cpuram, g2_bench_state, g2_bench_good, g2_bench_total, g2_area_rows;

/* Copies n 32-bit pixels with "rep movsl" (fast strings), as gfx_internal_copy32() does in the graphics code. */
static inline void g2_copy_row(volatile uint8_t *dst, const volatile uint8_t *src, uint32_t n)
{
    __asm__ volatile ("rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

static inline uint32_t g2_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    (void)hi;
    return lo;
}

int amd_gpu2d_ready(void) { return g2_active && rg_up && !rg_dead; }

int amd_gpu2d_flush(void)
{
    uint32_t seq;
    if (!g2_n) return 1;
    if (!amd_gpu2d_ready()) { g2_n = 0; return 0; }
    (void)vram_rd32(CP_IB_VRAM_OFF + 4u * (g2_n - 1u));       /* flush posted IB writes */
    seq = amd_gpu_submit_ib(rg_va_ib, g2_n);
    g2_n = 0;
    if (!seq || !amd_gpu_fence_wait(seq, 16000000u)) { g2_active = 0; g2_fail++; return 0; }
    g2_flushes++;
    return 1;
}

static int g2_room(void)
{
    if (g2_n + 7u > G2_MAX_DW) return amd_gpu2d_flush();
    return 1;
}

static int g2_emit(uint32_t ctl, uint32_t src_lo, uint32_t src_hi, uint64_t dst, uint32_t bytes)
{
    if (!g2_room()) return 0;
    vram_wr32(CP_IB_VRAM_OFF + 4u * g2_n++, 0xC0055000u);
    vram_wr32(CP_IB_VRAM_OFF + 4u * g2_n++, ctl);
    vram_wr32(CP_IB_VRAM_OFF + 4u * g2_n++, src_lo);
    vram_wr32(CP_IB_VRAM_OFF + 4u * g2_n++, src_hi);
    vram_wr32(CP_IB_VRAM_OFF + 4u * g2_n++, (uint32_t)dst);
    vram_wr32(CP_IB_VRAM_OFF + 4u * g2_n++, (uint32_t)(dst >> 32));
    vram_wr32(CP_IB_VRAM_OFF + 4u * g2_n++, bytes);
    g2_pkts++;
    return 1;
}

static int g2_clip(int *x, int *y, int *w, int *h)
{
    int x1, y1;
    if (*w <= 0 || *h <= 0) return 0;
    x1 = *x + *w; y1 = *y + *h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (x1 > (int)g2_w) x1 = (int)g2_w;
    if (y1 > (int)g2_h) y1 = (int)g2_h;
    if (x1 <= *x || y1 <= *y) return 0;
    *w = x1 - *x; *h = y1 - *y;
    return 1;
}

int amd_gpu2d_fill(int x, int y, int w, int h, uint32_t color)
{
    if (!amd_gpu2d_ready()) return 0;
    if (!g2_clip(&x, &y, &w, &h)) return 1;                 /* fully clipped: nothing to do */
    g2_ops++;
    if (x == 0 && (uint32_t)w * 4u == g2_pitch) {           /* full width: contiguous block */
        uint64_t d = g2_mc + (uint64_t)y * g2_pitch;
        uint32_t left = (uint32_t)h * g2_pitch;
        while (left) {
            uint32_t chunk = left > G2_MAX_BYTES ? G2_MAX_BYTES : left;
            if (!g2_emit((2u << 29) | (1u << 31), color, 0u, d, chunk)) return 0;
            d += chunk; left -= chunk;
        }
        return 1;
    }
    for (int r = 0; r < h; r++) {
        uint64_t d = g2_mc + (uint64_t)(y + r) * g2_pitch + (uint64_t)x * 4u;
        if (!g2_emit((2u << 29) | (1u << 31), color, 0u, d, (uint32_t)w * 4u)) return 0;
    }
    return 1;
}

int amd_gpu2d_copy(int sx, int sy, int dx, int dy, int w, int h)
{
    int ox, oy, ow, oh, r;
    if (!amd_gpu2d_ready()) return 0;
    if (w <= 0 || h <= 0) return 1;
    /* require both rects fully on-screen (no per-pixel clipping of a copy) */
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0 ||
        sx + w > (int)g2_w || dx + w > (int)g2_w || sy + h > (int)g2_h || dy + h > (int)g2_h) return 0;
    ox = sx; oy = sy; ow = w; oh = h;
    (void)ox; (void)oy; (void)ow; (void)oh;
    if (sy == dy && sx != dx && dx < sx + w && sx < dx + w) return 0;   /* same-row overlap: unsupported */
    if (sx == dx && sy == dy) return 1;
    g2_ops++;
    for (r = 0; r < h; r++) {
        int rr = (dy > sy) ? (h - 1 - r) : r;                /* bottom-up when moving down */
        uint64_t s_a = g2_mc + (uint64_t)(sy + rr) * g2_pitch + (uint64_t)sx * 4u;
        uint64_t d_a = g2_mc + (uint64_t)(dy + rr) * g2_pitch + (uint64_t)dx * 4u;
        if (!g2_emit((1u << 31), (uint32_t)s_a, (uint32_t)(s_a >> 32), d_a, (uint32_t)w * 4u)) return 0;
    }
    return 1;
}

/* Self-test + benchmark on the bottom 128 rows (caller must gfx_invalidate()
   afterwards so the next present repaints them).  Cycles are TSC ticks.     */
int amd_gpu2d_bench(volatile uint8_t *fb_cpu)
{
    uint32_t area, y0, half, i, t0, t1, good = 0, total = 0, x;
    const uint32_t c1 = 0x0020C020u, c2 = 0x00C02020u;
    if (!amd_gpu2d_ready() || !fb_cpu || g2_h < 256u) { g2_bench_state = 1; return 0; }
    area = 128u; y0 = g2_h - area; half = area / 2u; g2_area_rows = area;

    /* CPU baseline: fill the area through the write-combining mapping. */
    t0 = g2_tsc();
    for (i = 0; i < area; i++)
        for (x = 0; x < g2_w; x++)
            *(volatile uint32_t *)(fb_cpu + (y0 + i) * g2_pitch + x * 4u) = 0u;
    (void)*(volatile uint32_t *)fb_cpu;
    t1 = g2_tsc(); g2_t_cpu = t1 - t0;

    /* GPU fill of the whole area: single packet, full width. */
    t0 = g2_tsc();
    (void)amd_gpu2d_fill(0, (int)y0, (int)g2_w, (int)area, c1);
    if (!amd_gpu2d_flush()) { g2_bench_state = 2; return 0; }
    t1 = g2_tsc(); g2_t_fill = t1 - t0;
    for (i = 0; i < area; i += 3u)
        for (x = 0; x < g2_w; x += 16u) { total++; if (*(volatile uint32_t *)(fb_cpu + (y0 + i) * g2_pitch + x * 4u) == c1) good++; }

    /* GPU rect fill (top half, narrower than the screen) then GPU copy to the bottom half. */
    (void)amd_gpu2d_fill(16, (int)y0, (int)g2_w - 32, (int)half, c2);
    t0 = g2_tsc();
    (void)amd_gpu2d_copy(16, (int)y0, 16, (int)(y0 + half), (int)g2_w - 32, (int)half);
    if (!amd_gpu2d_flush()) { g2_bench_state = 3; return 0; }
    t1 = g2_tsc(); g2_t_copy = t1 - t0;
    for (i = 0; i < half; i += 3u)
        for (x = 16u; x < g2_w - 16u; x += 16u) {
            total++;
            if (*(volatile uint32_t *)(fb_cpu + (y0 + half + i) * g2_pitch + x * 4u) == c2) good++;
        }

    /* The same copy done by the CPU, so the GPU copy has something to be compared with (rect: columns 16 .. w-17, `half` rows).
       (a) inside the framebuffer, top half -> bottom half: the reads come from the write-combined VRAM mapping.
       (b) from ordinary RAM into the framebuffer: what gfx_present() does with the back buffer.
       Both use "rep movsl" like gfx_internal_copy32(); the target is cleared first (not timed) and checked afterwards. */
    {
        const uint32_t cw = g2_w - 32u;                      /* pixels per row of the rect */
        volatile uint8_t *dst0 = fb_cpu + (y0 + half) * g2_pitch + 16u * 4u;
        const volatile uint8_t *src0 = fb_cpu + y0 * g2_pitch + 16u * 4u;
        uint32_t r;

        for (r = 0; r < half; r++)
            for (x = 0; x < cw; x++) *(volatile uint32_t *)(dst0 + r * g2_pitch + x * 4u) = 0u;
        (void)*(volatile uint32_t *)fb_cpu;
        t0 = g2_tsc();
        for (r = 0; r < half; r++) g2_copy_row(dst0 + r * g2_pitch, src0 + r * g2_pitch, cw);
        (void)*(volatile uint32_t *)fb_cpu;
        t1 = g2_tsc(); g2_t_cpucopy = t1 - t0;
        for (r = 0; r < half; r += 3u)
            for (x = 0; x < cw; x += 16u) { total++; if (*(volatile uint32_t *)(dst0 + r * g2_pitch + x * 4u) == c2) good++; }

        uint32_t *ram = (uint32_t *)kmalloc((uint64_t)cw * half * 4u);
        if (ram) {
            for (i = 0; i < cw * half; i++) ram[i] = c2;
            for (r = 0; r < half; r++)
                for (x = 0; x < cw; x++) *(volatile uint32_t *)(dst0 + r * g2_pitch + x * 4u) = 0u;
            (void)*(volatile uint32_t *)fb_cpu;
            t0 = g2_tsc();
            for (r = 0; r < half; r++) g2_copy_row(dst0 + r * g2_pitch, (const volatile uint8_t *)(ram + r * cw), cw);
            (void)*(volatile uint32_t *)fb_cpu;
            t1 = g2_tsc(); g2_t_cpuram = t1 - t0;
            for (r = 0; r < half; r += 3u)
                for (x = 0; x < cw; x += 16u) { total++; if (*(volatile uint32_t *)(dst0 + r * g2_pitch + x * 4u) == c2) good++; }
            kfree(ram);
        }
    }
    g2_bench_good = good; g2_bench_total = total;
    g2_bench_state = (good == total && total) ? 0x100u : 4u;
    return g2_bench_state == 0x100u;
}
uint32_t amd_gpu2d_value(int what)
{
    switch (what) {
        case 0: return g2_bench_state; case 1: return g2_bench_good; case 2: return g2_bench_total;
        case 3: return g2_t_fill;      case 4: return g2_t_copy;     case 5: return g2_t_cpu;
        case 6: return g2_ops;         case 7: return g2_pkts;       case 8: return g2_flushes;
        case 9: return g2_fail;        case 10: return g2_active;    case 11: return g2_area_rows * g2_pitch;
        case 12: return g2_pitch;
        case 13: return g2_t_cpucopy;  case 14: return g2_t_cpuram;
        default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* V156: GPU present.  The system-RAM back buffer is mapped into the GART
   (pages GP_IDX..) so the CP can DMA dirty spans straight from it into the
   scanout framebuffer instead of the CPU writing them through the slow
   write-combining mapping.  The GPU reads CPU-written (cached) data, so the
   init self-test proves the PTE snoop bit really gives coherency: it writes
   a pattern with plain stores (no clflush), lets the GPU copy it to the
   bottom screen rows and compares.  Mismatch => feature stays off.         */
#define GP_IDX 0x200u                            /* GART pages 0x200.. */
static uint64_t gp_va;
static uint32_t gp_state, gp_good, gp_total, gp_pages, gp_bytes, gp_calls;
static const char *gp_status = "GPU present not initialised";

int amd_gpu2d_present_ready(void) { return gp_state == 0x100u && amd_gpu2d_ready(); }

int amd_gpu2d_present_init(const void *back, uint32_t stride_px, uint32_t rows, volatile uint8_t *fb_cpu)
{
    uint32_t base, off0, pages, i, r, x, tw, t_rows = 8u, y0, good = 0, total = 0;
    uint32_t *save;
    uint64_t pa_page;
    gp_state = 1; gp_good = gp_total = 0;
    if (!amd_gpu2d_ready() || !back || !fb_cpu || !stride_px || !rows) { gp_status = "GPU present skipped: 2D engine not ready"; return 0; }
    if (rows < 64u || g2_h < 64u) { gp_status = "GPU present skipped: screen too small"; return 0; }
    base = (uint32_t)(uint64_t)back;
    off0 = base & 0xFFFu;
    gp_bytes = stride_px * rows * 4u;
    pages = (off0 + gp_bytes + 0xFFFu) >> 12;
    gp_pages = pages;
    if (GP_IDX + pages > GART_PAGES) { gp_state = 2; gp_status = "GPU present skipped: GART too small for back buffer"; return 0; }
    pa_page = (uint64_t)(base & ~0xFFFu);
    for (i = 0; i < pages; i++)
        cpib_pte_set(GP_IDX + i, pa_page + (uint64_t)i * 4096ULL, 1, 1);
    if (!hub_invalidate(g_gart_mmio, &hub_gc)) { gp_state = 3; gp_status = "GPU present FAILED: GART invalidate"; return 0; }
    (void)hub_invalidate(g_gart_mmio, &hub_mm);
    gp_va = amd_native_gart_va() + (uint64_t)GP_IDX * 0x1000ULL + off0;

    /* coherency self-test on the bottom rows (caller repaints the screen afterwards) */
    tw = stride_px < g2_w ? stride_px : g2_w;
    y0 = rows - t_rows;
    if (y0 + t_rows > g2_h) { gp_state = 4; gp_status = "GPU present skipped: back/screen height mismatch"; return 0; }
    save = (uint32_t *)sys_alloc_page_aligned(t_rows * stride_px * 4u);
    if (!save) { gp_state = 4; gp_status = "GPU present skipped: no save buffer"; return 0; }
    {
        uint32_t *b = (uint32_t *)back;
        for (i = 0; i < t_rows * stride_px; i++) save[i] = b[y0 * stride_px + i];
        for (r = 0; r < t_rows; r++)
            for (x = 0; x < stride_px; x++)
                b[(y0 + r) * stride_px + x] = 0x00A50000u ^ (((r * 4099u + x) * 0x9E3779B1u) & 0x00FFFFFFu);   /* plain stores */
    }
    gp_state = 0x100u;                              /* allow present_copy during the test */
    for (r = 0; r < t_rows; r++) {
        uint32_t soff = ((y0 + r) * stride_px) * 4u, doff = (y0 + r) * g2_pitch;
        if (!amd_gpu2d_present_copy(soff, doff, tw * 4u)) { gp_state = 5; gp_status = "GPU present FAILED: self-test submit"; goto restore; }
    }
    if (!amd_gpu2d_flush()) { gp_state = 6; gp_status = "GPU present FAILED: self-test fence"; goto restore; }
    {
        uint32_t *b = (uint32_t *)back;
        for (r = 0; r < t_rows; r++)
            for (x = 0; x < tw; x++) {
                total++;
                if (*(volatile uint32_t *)(fb_cpu + (y0 + r) * g2_pitch + x * 4u) == b[(y0 + r) * stride_px + x]) good++;
            }
    }
    gp_good = good; gp_total = total;
    if (good == total && total) { gp_state = 0x100u; gp_status = "GPU present PASS: GPU reads coherent back buffer"; }
    else { gp_state = 7; gp_status = "GPU present OFF: back buffer not coherent for GPU reads"; }
restore:
    {
        uint32_t *b = (uint32_t *)back;
        for (i = 0; i < t_rows * stride_px; i++) b[y0 * stride_px + i] = save[i];
    }
    return gp_state == 0x100u;
}

/* Queue a linear copy back-buffer[src_off..] -> framebuffer[dst_off..]. */
int amd_gpu2d_present_copy(uint32_t src_off, uint32_t dst_off, uint32_t bytes)
{
    if (gp_state != 0x100u || !amd_gpu2d_ready()) return 0;
    if (src_off + bytes > gp_bytes) return 0;
    gp_calls++;
    while (bytes) {
        uint32_t chunk = bytes > G2_MAX_BYTES ? G2_MAX_BYTES : bytes;
        uint64_t s_a = gp_va + src_off, d_a = g2_mc + dst_off;
        if (!g2_emit((1u << 31), (uint32_t)s_a, (uint32_t)(s_a >> 32), d_a, chunk)) return 0;
        src_off += chunk; dst_off += chunk; bytes -= chunk;
    }
    return 1;
}
const char *amd_gpu2d_present_status(void) { return gp_status; }
uint32_t amd_gpu2d_present_value(int what)
{
    switch (what) {
        case 0: return gp_state;  case 1: return gp_good;  case 2: return gp_total;
        case 3: return gp_pages;  case 4: return gp_bytes; case 5: return gp_calls;
        case 6: return GART_PAGES;
        default: return 0;
    }
}

#if FIROZ_GPU_BOOT_TESTS && !FIROZ_GPU_EXPERIMENTS
/* The boot proofs (draw / fill / blit) only need their buffers while they run:
   144 KiB of the 1 MiB kernel heap (about 14 %).  Unmap them from the GART and
   give them back once the tests are done. */
static void amd_gpu_boot_tests_release(void)
{
    uint32_t i;
    if (gd_buf) cpib_pte_set(GD_IDX, 0, 0, 0);
    if (gf_buf) for (i = 0; i < GF_PAGES; i++) cpib_pte_set(GF_IDX + i, 0, 0, 0);
    if (gb_buf) for (i = 0; i < GF_PAGES; i++) cpib_pte_set(GB_IDX + i, 0, 0, 0);
    (void)hub_invalidate(g_gart_mmio, &hub_gc);
    sys_free_page_aligned(gd_buf); gd_buf = 0;
    sys_free_page_aligned(gf_buf); gf_buf = 0;
    sys_free_page_aligned(gb_buf); gb_buf = 0;
}
#endif

int amd_native_cp_ib_fence_test(uint64_t mmio)
{
    int a = cp_ib_run(mmio, 0);
    int b = cp_ib_run(mmio, 1);
    int c = amd_gpu_ring_selftest(mmio);   /* V148: leaves the ring running on PASS */
#if FIROZ_GPU_BOOT_TESTS
    if (c) (void)amd_gpu_draw_test();      /* V151: GPU paints a tile */
    if (c) (void)amd_gpu_fill_test();      /* V152: CP DMA fill */
    if (c) (void)amd_gpu_blit_test();      /* V153: CP DMA blit */
#if !FIROZ_GPU_EXPERIMENTS
    amd_gpu_boot_tests_release();          /* V164: give the test buffers back to the heap */
#endif
#endif
    return a && b && c;
}
const char *amd_native_cp_ib_status(void) { return cpib[0].status; }
const char *amd_native_cp_ib_va_status(void) { return cpib[1].status; }
/* what 0..10 = physical run, 20..30 = GART-VA run */
uint32_t amd_native_cp_ib_value(int what)
{
    const CpIbResult *r = &cpib[what >= 20 ? 1 : 0];
    switch (what >= 20 ? what - 20 : what) {
        case 0: return r->ran;   case 1: return r->pass;  case 2: return r->step;
        case 3: return r->rptr;  case 4: return r->wptr;  case 5: return r->marker;
        case 6: return r->fenceb; case 7: return r->fencec; case 8: return r->busy;
        case 9: return r->stall1; case 10: return r->grbm;
        default: return 0;
    }
}
const char *amd_native_cp_vm_status(void) { return cp_vm_status; }
uint32_t amd_native_cp_vm_pte_addr_lo(void) {
    return (uint32_t)(gart_pt_pa + (uint64_t)cp_vm_pte_index * 8ULL);
}
uint32_t amd_native_cp_vm_pte_addr_hi(void) {
    return (uint32_t)((gart_pt_pa + (uint64_t)cp_vm_pte_index * 8ULL) >> 32);
}
uint32_t amd_native_cp_vmid0_ctx(void) { return cp_vm_vmid0_ctx; }
uint32_t amd_native_cp_vmid0_pt_lo(void) { return cp_vm_vmid0_pt_lo; }
uint32_t amd_native_cp_vmid0_pt_hi(void) { return cp_vm_vmid0_pt_hi; }
uint32_t amd_native_cp_vmid1_ctx(void) { return cp_vm_vmid1_ctx; }
uint32_t amd_native_cp_vmid1_pt_lo(void) { return cp_vm_vmid1_pt_lo; }
uint32_t amd_native_cp_vmid1_pt_hi(void) { return cp_vm_vmid1_pt_hi; }
uint32_t amd_native_cp_vmid1_setup_pass(void) { return cp_vm_vmid1_setup_pass; }
uint32_t amd_native_cp_vmid1_old_ctx(void) { return cp_vm_vmid1_old_ctx; }
uint32_t amd_native_cp_vmid1_old_pt_lo(void) { return cp_vm_vmid1_old_pt_lo; }
uint32_t amd_native_cp_vmid1_old_pt_hi(void) { return cp_vm_vmid1_old_pt_hi; }
uint32_t amd_native_cp_vmid1_after_ctx(void) { return cp_vm_vmid1_after_ctx; }
uint32_t amd_native_cp_vmid1_after_pt_lo(void) { return cp_vm_vmid1_after_pt_lo; }
uint32_t amd_native_cp_vmid1_after_pt_hi(void) { return cp_vm_vmid1_after_pt_hi; }

uint32_t amd_native_cp_vm_case_value(uint32_t c, uint32_t what) {
    if (c >= 3u) return 0;
    switch (what) {
        case 0: return cp_vm_case_pass[c];
        case 1: return cp_vm_case_step[c];
        case 2: return cp_vm_case_index[c];
        case 3: return (uint32_t)cp_vm_case_va[c];
        case 4: return (uint32_t)(cp_vm_case_va[c] >> 32);
        case 5: return (uint32_t)cp_vm_case_pa[c];
        case 6: return (uint32_t)(cp_vm_case_pa[c] >> 32);
        case 7: return cp_vm_case_dst[c];
        case 8: return cp_vm_case_before[c];
        case 9: return cp_vm_case_after[c];
        case 10: return cp_vm_case_vmid[c];
        case 11: return cp_vm_case_fault_gc[c];
        case 12: return cp_vm_case_fault_mm[c];
        case 13: return cp_vm_case_fault_addr_lo[c];
        case 14: return cp_vm_case_ctx[c];
        case 15: return cp_vm_case_pt_lo[c];
        case 16: return cp_vm_case_rb_vmid_before[c];
        case 17: return cp_vm_case_rb_vmid_after[c];
        case 18: return cp_vm_case_rptr[c];
        case 19: return cp_vm_case_wptr[c];
        case 20: return cp_vm_case_busy[c];
        case 21: return cp_vm_case_stall1[c];
        case 22: return cp_vm_case_grbm[c];
        case 23: return cp_vm_case_l2st[c];
        default: return 0;
    }
}

uint32_t amd_native_cp_vm_value(int what) {
    switch (what) {
        case 0: return cp_vm_pass; case 1: return cp_vm_step; case 2: return cp_vm_rptr;
        case 3: return cp_vm_me; case 4: return cp_vm_fault_mm; case 5: return cp_vm_fault_gc;
        case 6: return (uint32_t)cp_vm_va; case 7: return (uint32_t)(cp_vm_va >> 32);
        case 8: return (uint32_t)cp_vm_pa; case 9: return (uint32_t)(cp_vm_pa >> 32);
        case 10: return cp_vm_rbv; case 11: return cp_vm_rbb; case 12: return cp_vm_rbbhi;
        case 20: return cp_vm_cpu_pass; case 21: return cp_vm_cpu_before; case 22: return cp_vm_cpu_after;
        case 23: return cp_vm_pte_lo; case 24: return cp_vm_pte_hi; case 25: return (uint32_t)cp_vm_pte_pa; case 26: return (uint32_t)(cp_vm_pte_pa >> 32);
        case 27: return cp_vm_pkt[0];
        case 28: return cp_vm_pkt[1];
        case 29: return cp_vm_pkt[2];
        case 30: return cp_vm_pkt[3];
        case 31: return cp_vm_pkt[4];
        case 32: return cp_vm_hdr_pfp;
        case 33: return cp_vm_hdr_me;
        case 34: return cp_vm_hdr_ce;
        case 35: return cp_vm_busy;
        case 36: return cp_vm_stall1;
        case 37: return cp_vm_grbm;
        case 38: return cp_vm_grbm2;
        case 39: return (uint32_t)cp_vm_gart_base_snapshot;
        case 40: return (uint32_t)(cp_vm_gart_base_snapshot >> 32);
        case 41: return cp_vm_pte_index_snapshot;
        case 42: return (cp_vm_pkt[1] >> 8) & 7u;
        case 43: return (cp_vm_pkt[1] >> 20) & 1u;
        case 44: return (uint32_t)((cp_vm_pte_lo >> 3) & 1u);
        case 45: return (uint32_t)((cp_vm_pte_hi >> 16) & 0xFFFFu);
        case 46: return cp_vm_pte_lo;
        case 47: return cp_vm_pte_hi;
        case 50: return cp_phys_pass;
        case 51: return cp_phys_step;
        case 52: return cp_phys_before;
        case 53: return cp_phys_after;
        case 54: return cp_ring_sanity_pass;
        case 55: return cp_ring_sanity_step;
        case 56: return cp_ring_sanity_before;
        case 57: return cp_ring_sanity_after;
        case 58: return (uint32_t)cp_phys_target;
        case 59: return (uint32_t)(cp_phys_target >> 32);
        case 60: return cp_phys2_pass;
        case 61: return cp_phys2_step;
        case 62: return cp_phys2_before;
        case 63: return cp_phys2_after;
        case 64: return (uint32_t)cp_phys_target2;
        case 65: return (uint32_t)(cp_phys_target2 >> 32);
        case 13: return (uint32_t)cp_vm_gart_base; case 14: return (uint32_t)(cp_vm_gart_base >> 32);
        case 15: return (uint32_t)cp_vm_gart_end; case 16: return (uint32_t)(cp_vm_gart_end >> 32);
        case 17: return cp_vm_pte_index; case 18: return (uint32_t)cp_vm_pte_va; case 19: return (uint32_t)(cp_vm_pte_va >> 32);
        default: return 0;
    }
}

int amd_native_cp_ring_matrix(uint64_t mmio, uint64_t cpu_vram, int fw_ready, int direct_ucode)
{
    uint64_t ring_mc, wb_mc;
    int k, any = 0, npass = 0, nvar = 3;

    if (!fw_ready) { cpm_status = "CP matrix skipped: PSP firmware (CE/PFP/ME) not accepted this boot"; return 0; }
    if (!gpu_mmio_can_access(mmio) || !gpu_mmio_can_access(cpu_vram)) {
        cpm_status = "CP matrix skipped: MMIO/VRAM aperture not accessible"; return 0; }
    ring_mc = amd_native_vram_mc_addr(CP_RING_VRAM_OFF, CP_RING_BYTES);
    wb_mc   = amd_native_vram_mc_addr(CP_WB_VRAM_OFF, 0x1000u);
    if (!ring_mc || !wb_mc) { cpm_status = "CP matrix skipped: VRAM range unknown"; return 0; }
    g_cpu_vram = cpu_vram;
    vram_wr32(CP_RING_VRAM_OFF, 0x5AA5A55Au);
    if (vram_rd32(CP_RING_VRAM_OFF) != 0x5AA5A55Au) { cpm_status = "CP matrix abort: VRAM not CPU-writable"; return 0; }

    uc_expected();
    uc_probe(mmio, uc_pre);                       /* ucode RAM as PSP left it (CP halted) */
    uc_status = "ucode probe done (before any direct load)";

    for (k = 0; k < 3; k++) {
        int ok = cpm_variant(mmio, k, ring_mc, wb_mc);
        any |= ok; npass += ok;
    }
    if (direct_ucode) {
        if (uc_direct_load(mmio)) {
            nvar = 6;
            for (k = 3; k < 6; k++) {
                int ok = cpm_variant(mmio, k, ring_mc, wb_mc);
                any |= ok; npass += ok;
            }
        }
    }
    cpm_status = (npass == nvar) ? "CP matrix: all variants PASSED" :
                 any ? "CP matrix: some variants PASSED (see lines)" :
                       "CP matrix: all variants failed (CP re-halted)";
    return any;
}
const char *amd_native_cp_matrix_status(void) { return cpm_status; }
const char *amd_native_cp_var_name(int i) { return (i>=0&&i<CPV_COUNT)?cpv[i].name:""; }
uint32_t amd_native_cp_var(int i, int field)
{
    const CpVar *r; if (i<0||i>=CPV_COUNT) return 0; r=&cpv[i];
    switch (field) {
    case 0: return r->ran;  case 1: return r->pass; case 2: return r->step;
    case 3: return r->rptr; case 4: return r->wb;   case 5: return r->scr;
    case 6: return r->me;   case 7: return r->grbm; case 8: return r->busy;
    case 9: return r->stall1; case 10: return r->poll; case 11: return r->db;
    case 12: return r->base; case 13: return r->ap_lo; case 14: return r->ap_hi;
    case 15: return r->hd_pfp; case 16: return r->hd_me; case 17: return r->hd_ce;
    case 18: return r->st2; case 19: return r->st3; case 20: return r->cpf; case 21: return r->grbm2;
    }
    return 0;
}

int amd_native_cp_var_count(void) { return CPV_COUNT; }

/* V142: READ-ONLY VM diagnostics, captured once after the CP VM tests.
   Never writes a register.  Index -> meaning (see kernel.c overlay):
   0 PCI CMD   1 GC L2_CNTL   2 GC L2_STATUS   3 GC PROT_FAULT_CNTL
   4 GC PROT_FAULT_CNTL2   5 GC PROT_FAULT_STATUS  6 GC SYSAP_LOW
   7 GC SYSAP_HIGH   8 GC FB_OFFSET   9 MM FB_OFFSET   10 MM L2_STATUS
   11 MM PROT_FAULT_CNTL  12 GC FB_LOC_BASE  13 GC FB_LOC_TOP
   14 GC CTX0_CNTL  15 GC L2_CNTL2  16 GC L2_CNTL3  17 GC L2_CNTL4
   18 GC TLB_CNTL  19 GC CTX0 PT_BASE_LO  20 GC CTX0 PT_BASE_HI */
static uint32_t vmdiag_val[64];
static uint32_t vmdiag_pci_cmd = 0xFFFFFFFFu;
void amd_native_set_pci_cmd(uint32_t cmd) { vmdiag_pci_cmd = cmd; }
void amd_native_vm_diag(uint64_t m)
{
    uint32_t i;
    for (i = 0; i < 64u; i++) vmdiag_val[i] = 0xFFFFFFFFu;
    vmdiag_val[0] = vmdiag_pci_cmd;
    if (!gpu_mmio_can_access(m)) return;
    vmdiag_val[1]  = gpu_mmio_read32(m, RNO_GC_BYTE(0x0840u));
    vmdiag_val[2]  = gpu_mmio_read32(m, RNO_GC_BYTE(0x0843u));
    vmdiag_val[3]  = gpu_mmio_read32(m, RNO_GC_BYTE(0x0848u));
    vmdiag_val[4]  = gpu_mmio_read32(m, RNO_GC_BYTE(0x0849u));
    vmdiag_val[5]  = gpu_mmio_read32(m, RNO_GC_BYTE(0x084Bu));
    vmdiag_val[6]  = gpu_mmio_read32(m, RNO_GC_BYTE(0x0985u));
    vmdiag_val[7]  = gpu_mmio_read32(m, RNO_GC_BYTE(0x0986u));
    vmdiag_val[8]  = gpu_mmio_read32(m, RNO_GC_BYTE(GC_FB_OFFSET_R));
    vmdiag_val[9]  = gpu_mmio_read32(m, MMHUB_BYTE(MM_FB_OFFSET_R));
    vmdiag_val[10] = gpu_mmio_read32(m, MMHUB_BYTE(0x0683u));
    vmdiag_val[11] = gpu_mmio_read32(m, MMHUB_BYTE(0x0688u));
    vmdiag_val[12] = gpu_mmio_read32(m, RNO_GC_BYTE(GC_FB_LOCATION_BASE_R));
    vmdiag_val[13] = gpu_mmio_read32(m, RNO_GC_BYTE(GC_FB_LOCATION_TOP_R));
    vmdiag_val[14] = gpu_mmio_read32(m, RNO_GC_BYTE(0x0880u));
    vmdiag_val[15] = gpu_mmio_read32(m, RNO_GC_BYTE(0x0841u));
    vmdiag_val[16] = gpu_mmio_read32(m, RNO_GC_BYTE(0x0842u));
    vmdiag_val[17] = gpu_mmio_read32(m, RNO_GC_BYTE(0x0857u));
    vmdiag_val[18] = gpu_mmio_read32(m, RNO_GC_BYTE(0x0987u));
    vmdiag_val[19] = gpu_mmio_read32(m, RNO_GC_BYTE(0x08EBu));
    vmdiag_val[20] = gpu_mmio_read32(m, RNO_GC_BYTE(0x08ECu));
    /* raw dumps (read-only): GC 0x0840..0x084F -> 24..39, MM 0x0680..0x068F -> 40..55 */
    for (i = 0; i < 16u; i++) vmdiag_val[24 + i] = gpu_mmio_read32(m, RNO_GC_BYTE(0x0840u + i));
    for (i = 0; i < 16u; i++) vmdiag_val[40 + i] = gpu_mmio_read32(m, MMHUB_BYTE(0x0680u + i));
}
uint32_t amd_native_vmdiag(int i) { return (i >= 0 && i < 64) ? vmdiag_val[i] : 0; }

/* ------------------------------------------------------------------ */
/* Access for the 3D pipeline test (amd_3d.c): CPU access to VRAM, the IB page of the persistent ring. */
void amd_gpu_vram_write32(uint32_t off, uint32_t v) { vram_wr32(off, v); }
uint32_t amd_gpu_vram_read32(uint32_t off) { return vram_rd32(off); }
void amd_gpu_ib_write32(uint32_t dw, uint32_t v) { vram_wr32(CP_IB_VRAM_OFF + 4u * dw, v); }
/* The CPU wrote the IB through the write-combined BAR: mfence drains the CPU's buffers (a plain read of one dword does not flush the
   buffers that hold other lines), then a read waits for the posted writes. The last lines of an IB hold the draw packet. */
void amd_gpu_ib_flush(void)
{
    __asm__ volatile("mfence" ::: "memory");
    (void)vram_rd32(CP_IB_VRAM_OFF);
}
uint64_t amd_gpu_ib_va(void) { return rg_va_ib; }
int amd_gpu_ring_running(void) { return rg_up && !rg_dead; }
uint32_t amd_gpu_reg_read32(uint32_t dword_addr) { return gpu_mmio_read32(rg_mmio, dword_addr << 2); }
/* Everything the CPU wrote to VRAM (write combined) is out of the CPU's buffers: sfence, then a read. For data the GPU is about to use. */
void amd_gpu_wc_flush(void)
{
    __asm__ volatile("mfence" ::: "memory");
    (void)vram_rd32(0);
}
/* Reads the VM protection-fault registers (read only) into the accessors amd_native_fault_*(). */
void amd_gpu_fault_snapshot(void) { amd_native_fault_probe(rg_mmio); }
/* Clears the GC VM fault status as Linux gmc_v9_0_process_interrupt() does: VM_L2_PROTECTION_FAULT_CNTL (gc_9_1_offset.h 0x847,
   BASE_IDX 0) |= CLEAR_PROTECTION_FAULT_STATUS_ADDR (bit 0, gc_9_1_sh_mask.h). */
void amd_gpu_fault_clear(void)
{
    uint32_t off = RNO_GC_BYTE(0x0847u);
    if (!gpu_mmio_can_access(rg_mmio)) return;
    gpu_mmio_write32(rg_mmio, off, gpu_mmio_read32(rg_mmio, off) | 1u);
}
/* A register of the GPU by its dword address. WRITES the GPU: only for the few registers the 3D code sets directly. */
void amd_gpu_reg_write32(uint32_t dword_addr, uint32_t v) { gpu_mmio_write32(rg_mmio, dword_addr << 2, v); }
