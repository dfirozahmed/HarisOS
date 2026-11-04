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

/* Step 2 (WRITES the display hardware): points HUBP0's primary surface at a new MC address, with the vsync flip type left as the firmware
 * set it. Linux hubp1_program_surface_flip_and_addr(): high dword first, then the low one (the low write latches the address at the
 * next vsync). Only for a target machine, see amd_gpu_bringup_flip(). */
void amd_dcn_set_surface(uint64_t mmio, uint64_t mc_addr);
/* HUBP0 DCSURF_FLIP_CONTROL (bit 0 = a flip is pending) and DCSURF_SURFACE_INUSE (the address being scanned out now). */
uint32_t amd_dcn_flip_control(uint64_t mmio);
uint32_t amd_dcn_surface_inuse(uint64_t mmio);
#endif
