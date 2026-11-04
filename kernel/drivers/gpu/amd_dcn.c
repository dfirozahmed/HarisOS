#include "amd_dcn.h"
#include "gpu_mmio.h"

#define DCN_SEG2 0x34C0u /* DCN_BASE__INST0_SEG2 (renoir_ip_offset.h); every register below has BASE_IDX 2 */
#define DCN_REG(mm) ((DCN_SEG2 + (mm)) << 2)

#define mmOTG0_OTG_H_TOTAL            0x1b2au
#define mmOTG0_OTG_H_BLANK_START_END  0x1b2bu
#define mmOTG0_OTG_V_TOTAL            0x1b2fu
#define mmOTG0_OTG_CONTROL            0x1b41u
#define mmOTG0_OTG_STATUS             0x1b49u
#define mmHUBP0_DCSURF_SURFACE_CONFIG 0x05e5u
#define mmHUBP0_DCHUBP_CNTL           0x05f3u
#define mmHUBPREQ0_DCSURF_SURFACE_PITCH 0x0607u
#define mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS 0x060au
#define mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH 0x060bu
#define mmHUBPREQ0_DCSURF_FLIP_CONTROL 0x061bu
#define mmHUBPREQ0_DCSURF_SURFACE_INUSE 0x0621u

int amd_dcn_probe(uint64_t mmio, AmdDcnProbe *p)
{
    p->readable = 0;
    if (!mmio || !gpu_mmio_can_access(mmio)) return 0;
    p->otg_control = gpu_mmio_read32(mmio, DCN_REG(mmOTG0_OTG_CONTROL));
    p->otg_status  = gpu_mmio_read32(mmio, DCN_REG(mmOTG0_OTG_STATUS));
    p->otg_h_total = gpu_mmio_read32(mmio, DCN_REG(mmOTG0_OTG_H_TOTAL));
    p->otg_v_total = gpu_mmio_read32(mmio, DCN_REG(mmOTG0_OTG_V_TOTAL));
    p->otg_h_blank = gpu_mmio_read32(mmio, DCN_REG(mmOTG0_OTG_H_BLANK_START_END));
    p->hubp_cntl   = gpu_mmio_read32(mmio, DCN_REG(mmHUBP0_DCHUBP_CNTL));
    p->hubp_config = gpu_mmio_read32(mmio, DCN_REG(mmHUBP0_DCSURF_SURFACE_CONFIG));
    p->hubp_pitch  = gpu_mmio_read32(mmio, DCN_REG(mmHUBPREQ0_DCSURF_SURFACE_PITCH));
    p->hubp_addr_lo = gpu_mmio_read32(mmio, DCN_REG(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS));
    p->hubp_addr_hi = gpu_mmio_read32(mmio, DCN_REG(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH));
    p->readable = 1;
    return 1;
}

void amd_dcn_set_surface(uint64_t mmio, uint64_t mc_addr)
{
    if (!mmio || !gpu_mmio_can_access(mmio)) return;
    gpu_mmio_write32(mmio, DCN_REG(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH), (uint32_t)(mc_addr >> 32) & 0xFFFFu);
    gpu_mmio_write32(mmio, DCN_REG(mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS), (uint32_t)mc_addr);
}
uint32_t amd_dcn_flip_control(uint64_t mmio) { return gpu_mmio_read32(mmio, DCN_REG(mmHUBPREQ0_DCSURF_FLIP_CONTROL)); }
uint32_t amd_dcn_surface_inuse(uint64_t mmio) { return gpu_mmio_read32(mmio, DCN_REG(mmHUBPREQ0_DCSURF_SURFACE_INUSE)); }

/* Segment 1 (DCN_BASE__INST0_SEG1 = 0xC0, renoir_ip_offset.h) for the clock registers, segment 2 for the rest; BASE_IDX from
 * dcn_2_1_0_offset.h. */
#define DCN_SEG1 0x00C0u
#define DCN_REG1(mm) ((DCN_SEG1 + (mm)) << 2)
static const struct { const char *name; uint32_t seg; uint32_t mm; } ext[AMD_DCN_X_COUNT] = {
    [AMD_DCN_X_DENTIST_DISPCLK] = { "DENTIST_DISPCLK_CNTL", 1, 0x0064u },
    [AMD_DCN_X_DCCG_GATE]       = { "DCCG_GATE_DISABLE_CNTL", 1, 0x0074u },
    [AMD_DCN_X_DMCUB_SEC]       = { "DMCUB_SEC_CNTL", 2, 0x3278u },
    [AMD_DCN_X_DMCUB_CNTL]      = { "DMCUB_CNTL", 2, 0x32a0u },
    [AMD_DCN_X_DMCUB_SCRATCH0]  = { "DMCUB_SCRATCH0", 2, 0x328du },
    [AMD_DCN_X_DMCUB_SCRATCH1]  = { "DMCUB_SCRATCH1", 2, 0x328eu },
    [AMD_DCN_X_DMCUB_SCRATCH2]  = { "DMCUB_SCRATCH2", 2, 0x328fu },
    [AMD_DCN_X_DIG_FE]          = { "DIG0_DIG_FE_CNTL", 2, 0x2068u },
    [AMD_DCN_X_DIG_BE]          = { "DIG0_DIG_BE_CNTL", 2, 0x20afu },
    [AMD_DCN_X_DIG_BE_EN]       = { "DIG0_DIG_BE_EN_CNTL", 2, 0x20b0u },
    [AMD_DCN_X_DP_LINK]         = { "DP0_DP_LINK_CNTL", 2, 0x2108u },
    [AMD_DCN_X_DP_CONFIG]       = { "DP0_DP_CONFIG", 2, 0x210bu },
    [AMD_DCN_X_DP_VID]          = { "DP0_DP_VID_STREAM_CNTL", 2, 0x210cu },
    [AMD_DCN_X_DP_FRAMING]      = { "DP0_DP_LINK_FRAMING_CNTL", 2, 0x2113u },
    [AMD_DCN_X_DP_DPHY]         = { "DP0_DP_DPHY_CNTL", 2, 0x2117u },
    [AMD_DCN_X_DP_SEC]          = { "DP0_DP_SEC_CNTL", 2, 0x212bu },
    [AMD_DCN_X_PWRSEQ_CNTL]     = { "LVTMA_PWRSEQ_CNTL", 2, 0x2883u },
    [AMD_DCN_X_PWRSEQ_STATE]    = { "LVTMA_PWRSEQ_STATE", 2, 0x2884u },
    [AMD_DCN_X_BL_PWM_CNTL]     = { "BL_PWM_CNTL", 2, 0x2888u },
    [AMD_DCN_X_BL_PWM_PERIOD]   = { "BL_PWM_PERIOD_CNTL", 2, 0x288au },
};
const char *amd_dcn_ext_name(int i) { return i >= 0 && i < AMD_DCN_X_COUNT ? ext[i].name : ""; }
int amd_dcn_probe_ext(uint64_t mmio, uint32_t *v)
{
    if (!mmio || !gpu_mmio_can_access(mmio)) return 0;
    for (int i = 0; i < AMD_DCN_X_COUNT; i++)
        v[i] = gpu_mmio_read32(mmio, ext[i].seg == 1 ? DCN_REG1(ext[i].mm) : DCN_REG(ext[i].mm));
    return 1;
}
