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
