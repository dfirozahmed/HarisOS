#ifndef AMD_DCN_H
#define AMD_DCN_H
#include <stdint.h>

/* AMD Renoir display core (DCN 2.1). Step 1: READ ONLY. Which register block the firmware left running, for the later page-flip /
 * mode-setting steps. Offsets: Linux dcn_2_1_0_offset.h (all BASE_IDX 2) + renoir_ip_offset.h (DCN_BASE seg2 = 0x34C0). */
typedef struct
{
    uint32_t otg_control, otg_status, otg_h_total, otg_v_total, otg_h_blank;
    uint32_t hubp_cntl, hubp_config, hubp_pitch, hubp_addr_lo, hubp_addr_hi;
    int readable;
} AmdDcnProbe;

/* Reads the registers of pipe 0 (OTG0 / HUBP0). Nothing is written. mmio = BAR5 physical base. */
int amd_dcn_probe(uint64_t mmio, AmdDcnProbe *p);
#endif
