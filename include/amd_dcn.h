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

/* Step 3 (READ ONLY): the rest of what a full display driver would take over from the firmware: DMCUB (display microcontroller),
 * DIG0 / DP0 (the eDP link), the panel power sequencer, the backlight PWM, and the display clock. Each entry is one register, read
 * by name; the log shows what the firmware left, so the next steps are written against real values. */
enum {
    AMD_DCN_X_DENTIST_DISPCLK, AMD_DCN_X_DCCG_GATE,
    AMD_DCN_X_DMCUB_SEC, AMD_DCN_X_DMCUB_CNTL, AMD_DCN_X_DMCUB_SCRATCH0, AMD_DCN_X_DMCUB_SCRATCH1, AMD_DCN_X_DMCUB_SCRATCH2,
    AMD_DCN_X_DIG_FE, AMD_DCN_X_DIG_BE, AMD_DCN_X_DIG_BE_EN,
    AMD_DCN_X_DP_LINK, AMD_DCN_X_DP_CONFIG, AMD_DCN_X_DP_VID, AMD_DCN_X_DP_FRAMING, AMD_DCN_X_DP_DPHY, AMD_DCN_X_DP_SEC,
    AMD_DCN_X_PWRSEQ_CNTL, AMD_DCN_X_PWRSEQ_STATE, AMD_DCN_X_BL_PWM_CNTL, AMD_DCN_X_BL_PWM_PERIOD,
    AMD_DCN_X_COUNT
};
const char *amd_dcn_ext_name(int i);
/* Fills v[AMD_DCN_X_COUNT]; 0 = registers not reachable. */
int amd_dcn_probe_ext(uint64_t mmio, uint32_t *v);
#endif
